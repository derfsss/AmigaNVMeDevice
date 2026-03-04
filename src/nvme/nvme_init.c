#include "nvme_init.h"
#include "nvme_admin.h"
#include "nvme_device.h"
#include <exec/exec.h>
#include <exec/memory.h>

/*
 * nvme_init.c — NVMe controller reset and initialisation sequence.
 *
 * Sequence (NVMe spec section 3.5.1):
 *   1. Read CAP — extract MQES, DSTRD, MPSMIN
 *   2. Disable controller (CC.EN=0), poll CSTS.RDY=0
 *   3. Allocate admin SQ + CQ (MEMF_SHARED), DMA-map them, write ASQ/ACQ/AQA
 *   4. Set CC (enable, page size, SQ/CQ entry sizes), write CC.EN=1
 *   5. Poll CSTS.RDY=1
 *   6. Allocate identify scratch buffer
 *   7. Admin: Identify Controller — extract model, MDTS
 *   (Namespace discovery and I/O queue creation done in unit_discovery.c)
 */

/* Busy-poll CSTS.RDY with ~5s timeout (iterations at ~1µs each on fast PPC) */
#define NVME_READY_POLL_ITERS  5000000UL

BOOL InitNVMe(struct NVMeBase *devBase)
{
    struct ExecIFace *IExec  = devBase->IExec;
    struct PCIDevice *pciDev = devBase->pciDevice;
    ULONG             base   = devBase->iobase;

    /* 1. Read CAP */
    ULONG cap_lo = NVME_R32(base, pciDev, NVME_REG_CAP_LO);
    ULONG cap_hi = NVME_R32(base, pciDev, NVME_REG_CAP_HI);

    devBase->cap_mqes  = NVME_CAP_MQES(cap_lo) + 1; /* spec value is max-1 */
    devBase->cap_dstrd = NVME_CAP_DSTRD(cap_lo);    /* doorbell stride exponent */

    /* Host page size: 4KB minimum (log2(4096)-12 = 0) */
    ULONG mpsmin = NVME_CAP_MPSMIN_HI(cap_hi);
    devBase->page_size = 4096u << mpsmin;

    IExec->DebugPrintF("[nvme.device:init] CAP: MQES=%lu DSTRD=%lu MPSMIN=%lu page_size=%lu\n",
                       devBase->cap_mqes, devBase->cap_dstrd, mpsmin, devBase->page_size);

    /* 2. Disable controller */
    ULONG cc = NVME_R32(base, pciDev, NVME_REG_CC);
    if (cc & NVME_CC_EN) {
        NVME_W32(base, pciDev, NVME_REG_CC, cc & ~NVME_CC_EN);
        /* Poll CSTS.RDY=0 */
        for (ULONG i = 0; i < NVME_READY_POLL_ITERS; i++) {
            if (!(NVME_R32(base, pciDev, NVME_REG_CSTS) & NVME_CSTS_RDY))
                goto disabled;
        }
        IExec->DebugPrintF("[nvme.device:init] Timeout waiting for controller to disable\n");
        return FALSE;
    }
disabled:

    /* 3. Allocate admin queues */
    ULONG sq_bytes = NVME_ADMIN_QUEUE_DEPTH * NVME_SQE_SIZE;
    ULONG cq_bytes = NVME_ADMIN_QUEUE_DEPTH * NVME_CQE_SIZE;

    devBase->admin_sq = IExec->AllocVecTags(sq_bytes,
        AVT_Type,       MEMF_SHARED,
        AVT_Alignment,  devBase->page_size,
        AVT_Clear,      0,
        TAG_DONE);
    devBase->admin_cq = IExec->AllocVecTags(cq_bytes,
        AVT_Type,       MEMF_SHARED,
        AVT_Alignment,  devBase->page_size,
        AVT_Clear,      0,
        TAG_DONE);
    if (!devBase->admin_sq || !devBase->admin_cq) {
        IExec->DebugPrintF("[nvme.device:init] Failed to allocate admin queues\n");
        goto fail_queues;
    }

    /* DMA-map admin queues */
    ULONG sq_dma_entries = IExec->StartDMA(devBase->admin_sq, sq_bytes, DMA_ReadFromRAM);
    devBase->admin_sq_dma = IExec->AllocSysObjectTags(ASOT_DMAENTRY,
        ASODMAE_NumEntries, sq_dma_entries, TAG_DONE);
    if (!devBase->admin_sq_dma) goto fail_dma;
    IExec->GetDMAList(devBase->admin_sq, sq_bytes, DMA_ReadFromRAM, devBase->admin_sq_dma);
    devBase->admin_sq_phys = (ULONG)devBase->admin_sq_dma[0].PhysicalAddress;

    ULONG cq_dma_entries = IExec->StartDMA(devBase->admin_cq, cq_bytes, DMA_ReadFromRAM);
    devBase->admin_cq_dma = IExec->AllocSysObjectTags(ASOT_DMAENTRY,
        ASODMAE_NumEntries, cq_dma_entries, TAG_DONE);
    if (!devBase->admin_cq_dma) goto fail_dma;
    IExec->GetDMAList(devBase->admin_cq, cq_bytes, DMA_ReadFromRAM, devBase->admin_cq_dma);
    devBase->admin_cq_phys = (ULONG)devBase->admin_cq_dma[0].PhysicalAddress;

    devBase->admin_sq_tail = 0;
    devBase->admin_cq_head = 0;
    devBase->admin_cq_phase = 1; /* phase starts at 1 */
    devBase->next_cmd_id   = 1;

    /* Write ASQ / ACQ base addresses (64-bit — high word 0 on 32-bit PPC DMA) */
    NVME_W32(base, pciDev, NVME_REG_ASQ_LO, devBase->admin_sq_phys);
    NVME_W32(base, pciDev, NVME_REG_ASQ_HI, 0);
    NVME_W32(base, pciDev, NVME_REG_ACQ_LO, devBase->admin_cq_phys);
    NVME_W32(base, pciDev, NVME_REG_ACQ_HI, 0);

    /* Admin Queue Attributes: ASQS and ACQS (size-1) */
    NVME_W32(base, pciDev, NVME_REG_AQA,
             NVME_AQA_ASQS(NVME_ADMIN_QUEUE_DEPTH) |
             NVME_AQA_ACQS(NVME_ADMIN_QUEUE_DEPTH));

    /* 4. Configure and enable controller */
    ULONG new_cc = NVME_CC_DEFAULT | NVME_CC_EN;
    NVME_W32(base, pciDev, NVME_REG_CC, new_cc);

    /* 5. Poll CSTS.RDY=1 */
    for (ULONG i = 0; i < NVME_READY_POLL_ITERS; i++) {
        ULONG csts = NVME_R32(base, pciDev, NVME_REG_CSTS);
        if (csts & NVME_CSTS_CFS) {
            IExec->DebugPrintF("[nvme.device:init] Controller Fatal Status during enable\n");
            goto fail_dma;
        }
        if (csts & NVME_CSTS_RDY)
            goto ready;
    }
    IExec->DebugPrintF("[nvme.device:init] Timeout waiting for controller ready\n");
    goto fail_dma;

ready:
    IExec->DebugPrintF("[nvme.device:init] Controller ready. VS=0x%08lx\n",
                       NVME_R32(base, pciDev, NVME_REG_VS));

    /* 6. Allocate 4KB identify scratch buffer */
    devBase->identify_buf = IExec->AllocVecTags(4096,
        AVT_Type,      MEMF_SHARED,
        AVT_Alignment, devBase->page_size,
        AVT_Clear,     0,
        TAG_DONE);
    if (!devBase->identify_buf) {
        IExec->DebugPrintF("[nvme.device:init] Failed to allocate identify buffer\n");
        goto fail_dma;
    }
    ULONG id_dma_entries = IExec->StartDMA(devBase->identify_buf, 4096, DMA_ReadFromRAM);
    devBase->identify_dma = IExec->AllocSysObjectTags(ASOT_DMAENTRY,
        ASODMAE_NumEntries, id_dma_entries, TAG_DONE);
    if (!devBase->identify_dma) goto fail_identify;
    IExec->GetDMAList(devBase->identify_buf, 4096, DMA_ReadFromRAM, devBase->identify_dma);
    devBase->identify_phys = (ULONG)devBase->identify_dma[0].PhysicalAddress;

    /* 7. Identify Controller */
    if (!NVMe_IdentifyController(devBase)) {
        IExec->DebugPrintF("[nvme.device:init] Identify Controller failed\n");
        goto fail_identify;
    }

    return TRUE;

fail_identify:
    if (devBase->identify_buf) {
        IExec->EndDMA(devBase->identify_buf, 4096, DMA_ReadFromRAM);
        IExec->FreeVec(devBase->identify_buf);
        devBase->identify_buf = NULL;
    }
fail_dma:
    if (devBase->admin_sq_dma) {
        IExec->EndDMA(devBase->admin_sq, sq_bytes, DMA_ReadFromRAM);
        IExec->FreeSysObject(ASOT_DMAENTRY, devBase->admin_sq_dma);
        devBase->admin_sq_dma = NULL;
    }
    if (devBase->admin_cq_dma) {
        IExec->EndDMA(devBase->admin_cq, cq_bytes, DMA_ReadFromRAM);
        IExec->FreeSysObject(ASOT_DMAENTRY, devBase->admin_cq_dma);
        devBase->admin_cq_dma = NULL;
    }
fail_queues:
    if (devBase->admin_sq) { IExec->FreeVec(devBase->admin_sq); devBase->admin_sq = NULL; }
    if (devBase->admin_cq) { IExec->FreeVec(devBase->admin_cq); devBase->admin_cq = NULL; }
    return FALSE;
}

void CleanupNVMe(struct NVMeBase *devBase)
{
    struct ExecIFace *IExec  = devBase->IExec;
    struct PCIDevice *pciDev = devBase->pciDevice;
    ULONG             base   = devBase->iobase;

    if (!devBase->admin_sq) return; /* not initialised */

    /* Normal shutdown */
    ULONG cc = NVME_R32(base, pciDev, NVME_REG_CC);
    NVME_W32(base, pciDev, NVME_REG_CC, (cc & ~(3 << 14)) | NVME_CC_SHN_NORM);
    /* Poll CSTS.SHST == 2 (shutdown complete) — best effort, no hard timeout */
    for (ULONG i = 0; i < 1000000UL; i++) {
        if (NVME_CSTS_SHST(NVME_R32(base, pciDev, NVME_REG_CSTS)) == 2)
            break;
    }

    /* Free identify buffer */
    if (devBase->identify_buf) {
        IExec->EndDMA(devBase->identify_buf, 4096, DMA_ReadFromRAM);
        if (devBase->identify_dma)
            IExec->FreeSysObject(ASOT_DMAENTRY, devBase->identify_dma);
        IExec->FreeVec(devBase->identify_buf);
        devBase->identify_buf = NULL;
        devBase->identify_dma = NULL;
    }

    /* Free admin queues */
    ULONG sq_bytes = NVME_ADMIN_QUEUE_DEPTH * NVME_SQE_SIZE;
    ULONG cq_bytes = NVME_ADMIN_QUEUE_DEPTH * NVME_CQE_SIZE;
    if (devBase->admin_sq_dma) {
        IExec->EndDMA(devBase->admin_sq, sq_bytes, DMA_ReadFromRAM);
        IExec->FreeSysObject(ASOT_DMAENTRY, devBase->admin_sq_dma);
        devBase->admin_sq_dma = NULL;
    }
    if (devBase->admin_cq_dma) {
        IExec->EndDMA(devBase->admin_cq, cq_bytes, DMA_ReadFromRAM);
        IExec->FreeSysObject(ASOT_DMAENTRY, devBase->admin_cq_dma);
        devBase->admin_cq_dma = NULL;
    }
    if (devBase->admin_sq) { IExec->FreeVec(devBase->admin_sq); devBase->admin_sq = NULL; }
    if (devBase->admin_cq) { IExec->FreeVec(devBase->admin_cq); devBase->admin_cq = NULL; }
}
