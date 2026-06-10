/*
 * nvme_init.c — per-controller reset / enable / admin-queue bring-up.
 *
 * Sequence (NVMe 1.4 §3.5.1), applied independently to each controller:
 *
 *   1. Read CAP — extract MQES, DSTRD, MPSMIN
 *   2. Disable (CC.EN=0), wait CSTS.RDY=0
 *   3. Allocate admin SQ + CQ (MEMF_SHARED), DMA-map, write ASQ/ACQ/AQA
 *   4. Program CC (page size, entry sizes) and set CC.EN=1
 *   5. Wait CSTS.RDY=1
 *   6. Allocate the 4 KiB Identify scratch buffer
 *   7. Admin Identify Controller — populate MDTS / model / FW
 *
 * Namespace enumeration and per-unit I/O queue creation happen later
 * in unit_discovery.c on a per-controller basis.
 */

#include "nvme_init.h"
#include "nvme_admin.h"
#include "nvme_device.h"
#include <exec/exec.h>
#include <exec/memory.h>

/* Busy-poll CSTS.RDY floor (iterations at roughly 1 µs each — every
 * pass is a guarded, cache-inhibited MMIO read, so it cannot run much
 * faster than the bus round-trip even on a fast CPU).  The actual
 * budget is scaled by CAP.TO below: real-silicon controllers may
 * legitimately take many seconds to come ready after an enable. */
#define NVME_READY_POLL_ITERS  5000000UL

/* Per-CAP.TO ready budget: TO is in 500 ms units; at ~1 µs per MMIO
 * poll, one TO unit needs ~500k iterations.  Double it for margin. */
static ULONG ready_poll_budget(ULONG cap_lo)
{
    ULONG to = NVME_CAP_TO(cap_lo);
    ULONG scaled = (to + 1) * 1000000UL;
    return (scaled > NVME_READY_POLL_ITERS) ? scaled : NVME_READY_POLL_ITERS;
}

BOOL InitNVMe(struct NVMeController *ctrl)
{
    struct NVMeBase  *devBase = ctrl->dev_base;
    struct ExecIFace *IExec   = devBase->IExec;
    ULONG             base    = ctrl->iobase;

    BOOL have_sq_vec = FALSE;
    BOOL have_cq_vec = FALSE;
    BOOL have_sq_dma = FALSE;
    BOOL have_cq_dma = FALSE;
    BOOL have_id_vec = FALSE;
    BOOL have_id_dma = FALSE;

    ULONG sq_bytes = NVME_ADMIN_QUEUE_DEPTH * NVME_SQE_SIZE;
    ULONG cq_bytes = NVME_ADMIN_QUEUE_DEPTH * NVME_CQE_SIZE;

    /* 1. Read CAP. */
    ULONG cap_lo = NVME_R32(base, NULL, NVME_REG_CAP_LO);
    ULONG cap_hi = NVME_R32(base, NULL, NVME_REG_CAP_HI);

    ctrl->cap_mqes  = NVME_CAP_MQES(cap_lo) + 1;
    ctrl->cap_dstrd = NVME_CAP_DSTRD(cap_lo);

    ULONG mpsmin = NVME_CAP_MPSMIN_HI(cap_hi);
    ctrl->page_size = 4096u << mpsmin;

    /* Per-controller I/O queue depth: our compile-time depth, clamped
     * to what the controller actually supports.  The inflight pipeline
     * keys cids 1..NVME_MAX_INFLIGHT to SQ slots, so the queue must be
     * strictly deeper than the inflight count or the SQ could fill. */
    ctrl->io_queue_depth = (ctrl->cap_mqes < NVME_IO_QUEUE_DEPTH)
                         ? (UWORD)ctrl->cap_mqes : NVME_IO_QUEUE_DEPTH;
    if (ctrl->io_queue_depth <= NVME_MAX_INFLIGHT) {
        DLOG(IExec, "[nvme.device:init] ctrl %lu: MQES=%lu too shallow for"
                    " %u inflight slots — rejecting controller\n",
             ctrl->ctrl_idx, ctrl->cap_mqes, NVME_MAX_INFLIGHT);
        return FALSE;
    }

    ULONG ready_iters = ready_poll_budget(cap_lo);

    DLOG(IExec, "[nvme.device:init] ctrl %lu CAP: MQES=%lu DSTRD=%lu"
                " MPSMIN=%lu page_size=%lu io_qdepth=%u TO=%lu\n",
         ctrl->ctrl_idx, ctrl->cap_mqes, ctrl->cap_dstrd, mpsmin,
         ctrl->page_size, ctrl->io_queue_depth, NVME_CAP_TO(cap_lo));

    /* 2. Disable controller, wait CSTS.RDY=0. */
    ULONG cc = NVME_R32(base, NULL, NVME_REG_CC);
    if (cc & NVME_CC_EN) {
        NVME_W32(base, NULL, NVME_REG_CC, cc & ~NVME_CC_EN);
        ULONG i;
        for (i = 0; i < ready_iters; i++) {
            if (!(NVME_R32(base, NULL, NVME_REG_CSTS) & NVME_CSTS_RDY))
                break;
        }
        if (i == ready_iters) {
            DLOG(IExec, "[nvme.device:init] ctrl %lu: CSTS.RDY=0 timeout\n",
                 ctrl->ctrl_idx);
            goto err;
        }
    }

    /* 3. Allocate admin SQ + CQ. */
    ctrl->admin_sq = IExec->AllocVecTags(sq_bytes,
        AVT_Type,           MEMF_SHARED,
        AVT_Alignment,      ctrl->page_size,
        AVT_ClearWithValue, 0,
        TAG_DONE);
    if (!ctrl->admin_sq) {
        DLOG(IExec, "[nvme.device:init] ctrl %lu: admin SQ alloc failed\n",
             ctrl->ctrl_idx);
        goto err;
    }
    NVME_LEAK_INC(nvme_leak_vec);
    have_sq_vec = TRUE;

    ctrl->admin_cq = IExec->AllocVecTags(cq_bytes,
        AVT_Type,           MEMF_SHARED,
        AVT_Alignment,      ctrl->page_size,
        AVT_ClearWithValue, 0,
        TAG_DONE);
    if (!ctrl->admin_cq) {
        DLOG(IExec, "[nvme.device:init] ctrl %lu: admin CQ alloc failed\n",
             ctrl->ctrl_idx);
        goto err;
    }
    NVME_LEAK_INC(nvme_leak_vec);
    have_cq_vec = TRUE;

    /* DMA-map admin SQ. */
    ULONG sq_entries = IExec->StartDMA(ctrl->admin_sq, sq_bytes, DMA_ReadFromRAM);
    have_sq_dma = TRUE;
    NVME_LEAK_INC(nvme_leak_dma);
    if (sq_entries == 0) goto err;
    ctrl->admin_sq_dma = IExec->AllocSysObjectTags(ASOT_DMAENTRY,
        ASODMAE_NumEntries, sq_entries, TAG_DONE);
    if (!ctrl->admin_sq_dma) goto err;
    NVME_LEAK_INC(nvme_leak_dmaentry);
    IExec->GetDMAList(ctrl->admin_sq, sq_bytes, DMA_ReadFromRAM, ctrl->admin_sq_dma);
    ctrl->admin_sq_phys = (ULONG)ctrl->admin_sq_dma[0].PhysicalAddress;

    /* DMA-map admin CQ. */
    ULONG cq_entries = IExec->StartDMA(ctrl->admin_cq, cq_bytes, 0);
    have_cq_dma = TRUE;
    NVME_LEAK_INC(nvme_leak_dma);
    if (cq_entries == 0) goto err;
    ctrl->admin_cq_dma = IExec->AllocSysObjectTags(ASOT_DMAENTRY,
        ASODMAE_NumEntries, cq_entries, TAG_DONE);
    if (!ctrl->admin_cq_dma) goto err;
    NVME_LEAK_INC(nvme_leak_dmaentry);
    IExec->GetDMAList(ctrl->admin_cq, cq_bytes, 0, ctrl->admin_cq_dma);
    ctrl->admin_cq_phys = (ULONG)ctrl->admin_cq_dma[0].PhysicalAddress;

    ctrl->admin_sq_tail  = 0;
    ctrl->admin_cq_head  = 0;
    ctrl->admin_cq_phase = 1;
    ctrl->next_cmd_id    = 1;

    /* Program ASQ / ACQ / AQA. */
    NVME_W32(base, NULL, NVME_REG_ASQ_LO, ctrl->admin_sq_phys);
    NVME_W32(base, NULL, NVME_REG_ASQ_HI, 0);
    NVME_W32(base, NULL, NVME_REG_ACQ_LO, ctrl->admin_cq_phys);
    NVME_W32(base, NULL, NVME_REG_ACQ_HI, 0);
    NVME_W32(base, NULL, NVME_REG_AQA,
             NVME_AQA_ASQS(NVME_ADMIN_QUEUE_DEPTH) |
             NVME_AQA_ACQS(NVME_ADMIN_QUEUE_DEPTH));

    /* 4. Enable controller.  CC.MPS must encode the page size we are
     * actually using; NVME_CC_DEFAULT bakes in MPS(0) (4 KiB), which a
     * controller whose CAP.MPSMIN > 0 would reject at enable time. */
    NVME_W32(base, NULL, NVME_REG_CC,
             NVME_CC_DEFAULT | NVME_CC_MPS(mpsmin) | NVME_CC_EN);

    /* 5. Wait CSTS.RDY=1; abort on CFS. */
    {
        ULONG i;
        for (i = 0; i < ready_iters; i++) {
            ULONG csts = NVME_R32(base, NULL, NVME_REG_CSTS);
            if (csts & NVME_CSTS_CFS) {
                DLOG(IExec, "[nvme.device:init] ctrl %lu: CSTS.CFS during enable\n",
                     ctrl->ctrl_idx);
                goto err;
            }
            if (csts & NVME_CSTS_RDY) break;
        }
        if (i == ready_iters) {
            DLOG(IExec, "[nvme.device:init] ctrl %lu: CSTS.RDY=1 timeout\n",
                 ctrl->ctrl_idx);
            goto err;
        }
    }

    DLOG(IExec, "[nvme.device:init] ctrl %lu ready — VS=0x%08lx\n",
         ctrl->ctrl_idx, NVME_R32(base, NULL, NVME_REG_VS));

    /* Mask admin interrupt until all unit tasks are up. */
    NVME_W32(base, NULL, NVME_REG_INTMS, 0xFFFFFFFF);

    /* 6. Identify scratch buffer. */
    ctrl->identify_buf = IExec->AllocVecTags(4096,
        AVT_Type,           MEMF_SHARED,
        AVT_Alignment,      ctrl->page_size,
        AVT_ClearWithValue, 0,
        TAG_DONE);
    if (!ctrl->identify_buf) goto err;
    NVME_LEAK_INC(nvme_leak_vec);
    have_id_vec = TRUE;

    ULONG id_entries = IExec->StartDMA(ctrl->identify_buf, 4096, 0);
    have_id_dma = TRUE;
    NVME_LEAK_INC(nvme_leak_dma);
    if (id_entries == 0) goto err;
    ctrl->identify_dma = IExec->AllocSysObjectTags(ASOT_DMAENTRY,
        ASODMAE_NumEntries, id_entries, TAG_DONE);
    if (!ctrl->identify_dma) goto err;
    NVME_LEAK_INC(nvme_leak_dmaentry);
    IExec->GetDMAList(ctrl->identify_buf, 4096, 0, ctrl->identify_dma);
    ctrl->identify_phys = (ULONG)ctrl->identify_dma[0].PhysicalAddress;

    /* 7. Identify Controller. */
    if (!NVMe_IdentifyController(ctrl))
        goto err;

    return TRUE;

err:
    if (ctrl->identify_dma) {
        IExec->FreeSysObject(ASOT_DMAENTRY, ctrl->identify_dma);
        NVME_LEAK_DEC(nvme_leak_dmaentry);
        ctrl->identify_dma = NULL;
    }
    if (have_id_dma) {
        IExec->EndDMA(ctrl->identify_buf, 4096, 0);
        NVME_LEAK_DEC(nvme_leak_dma);
    }
    if (have_id_vec) {
        IExec->FreeVec(ctrl->identify_buf);
        NVME_LEAK_DEC(nvme_leak_vec);
        ctrl->identify_buf = NULL;
    }
    if (ctrl->admin_cq_dma) {
        IExec->FreeSysObject(ASOT_DMAENTRY, ctrl->admin_cq_dma);
        NVME_LEAK_DEC(nvme_leak_dmaentry);
        ctrl->admin_cq_dma = NULL;
    }
    if (have_cq_dma) {
        IExec->EndDMA(ctrl->admin_cq, cq_bytes, 0);
        NVME_LEAK_DEC(nvme_leak_dma);
    }
    if (ctrl->admin_sq_dma) {
        IExec->FreeSysObject(ASOT_DMAENTRY, ctrl->admin_sq_dma);
        NVME_LEAK_DEC(nvme_leak_dmaentry);
        ctrl->admin_sq_dma = NULL;
    }
    if (have_sq_dma) {
        IExec->EndDMA(ctrl->admin_sq, sq_bytes, DMA_ReadFromRAM);
        NVME_LEAK_DEC(nvme_leak_dma);
    }
    if (have_cq_vec) {
        IExec->FreeVec(ctrl->admin_cq);
        NVME_LEAK_DEC(nvme_leak_vec);
        ctrl->admin_cq = NULL;
    }
    if (have_sq_vec) {
        IExec->FreeVec(ctrl->admin_sq);
        NVME_LEAK_DEC(nvme_leak_vec);
        ctrl->admin_sq = NULL;
    }
    return FALSE;
}

void CleanupNVMe(struct NVMeController *ctrl)
{
    struct ExecIFace *IExec = ctrl->dev_base->IExec;
    ULONG             base  = ctrl->iobase;

    if (!ctrl->admin_sq) return;   /* never initialised */

    /* Request normal shutdown; wait for CSTS.SHST = 10b (complete),
     * bounded poll — the backing memory is about to be freed. */
    ULONG cc = NVME_R32(base, NULL, NVME_REG_CC);
    NVME_W32(base, NULL, NVME_REG_CC, (cc & ~(3 << 14)) | NVME_CC_SHN_NORM);
    ULONG i;
    for (i = 0; i < 1000000UL; i++) {
        if (NVME_CSTS_SHST(NVME_R32(base, NULL, NVME_REG_CSTS)) == 2)
            break;
    }
    if (i == 1000000UL)
        DLOG(IExec, "[nvme.device:init] ctrl %lu shutdown poll timed out\n",
             ctrl->ctrl_idx);

    if (ctrl->identify_buf) {
        IExec->EndDMA(ctrl->identify_buf, 4096, 0);
        NVME_LEAK_DEC(nvme_leak_dma);
        if (ctrl->identify_dma) {
            IExec->FreeSysObject(ASOT_DMAENTRY, ctrl->identify_dma);
            NVME_LEAK_DEC(nvme_leak_dmaentry);
        }
        IExec->FreeVec(ctrl->identify_buf);
        NVME_LEAK_DEC(nvme_leak_vec);
        ctrl->identify_buf = NULL;
        ctrl->identify_dma = NULL;
    }

    ULONG sq_bytes = NVME_ADMIN_QUEUE_DEPTH * NVME_SQE_SIZE;
    ULONG cq_bytes = NVME_ADMIN_QUEUE_DEPTH * NVME_CQE_SIZE;

    if (ctrl->admin_sq_dma) {
        IExec->EndDMA(ctrl->admin_sq, sq_bytes, DMA_ReadFromRAM);
        NVME_LEAK_DEC(nvme_leak_dma);
        IExec->FreeSysObject(ASOT_DMAENTRY, ctrl->admin_sq_dma);
        NVME_LEAK_DEC(nvme_leak_dmaentry);
        ctrl->admin_sq_dma = NULL;
    }
    if (ctrl->admin_cq_dma) {
        IExec->EndDMA(ctrl->admin_cq, cq_bytes, 0);
        NVME_LEAK_DEC(nvme_leak_dma);
        IExec->FreeSysObject(ASOT_DMAENTRY, ctrl->admin_cq_dma);
        NVME_LEAK_DEC(nvme_leak_dmaentry);
        ctrl->admin_cq_dma = NULL;
    }
    if (ctrl->admin_sq) {
        IExec->FreeVec(ctrl->admin_sq);
        NVME_LEAK_DEC(nvme_leak_vec);
        ctrl->admin_sq = NULL;
    }
    if (ctrl->admin_cq) {
        IExec->FreeVec(ctrl->admin_cq);
        NVME_LEAK_DEC(nvme_leak_vec);
        ctrl->admin_cq = NULL;
    }
}
