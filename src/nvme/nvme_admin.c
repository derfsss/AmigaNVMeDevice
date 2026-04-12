/*
 * nvme_admin.c — Admin SQ/CQ command submission and the specific
 * Identify / Create-CQ / Create-SQ wrappers used at Init time.
 *
 * All entries on a given controller are issued serially under
 * ctrl->io_lock, and all completions are consumed by polling the
 * admin CQ phase bit.  Admin interrupts are deliberately masked
 * throughout Init to avoid racing an IRQ against the first unit task
 * coming up on the shared INTx line.
 *
 * The CPU sees SQE memory as big-endian bytes; NVMe expects little-
 * endian.  dma_w32 / dma_r32 below apply the swap per 32-bit DWORD.
 */

#include "nvme_admin.h"
#include "nvme_device.h"
#include "nvme_stats.h"
#include <exec/exec.h>
#include <string.h>

/* NVMe SQE/CQE structures in DMA memory are little-endian.
 * Use stwbrx/lwbrx to byte-swap each 32-bit word on PPC. */
static inline void dma_w32(void *addr, ULONG val)
{
    __asm__ volatile ("stwbrx %0, 0, %1" : : "r"(val), "r"(addr) : "memory");
}

static inline ULONG dma_r32(const void *addr)
{
    ULONG val;
    __asm__ volatile ("lwbrx %0, 0, %1" : "=r"(val) : "r"(addr));
    return val;
}

/* Write a SQE (16 × ULONG) into the DMA queue buffer with LE byte-swap */
static void write_sqe(struct nvme_sqe *dst, const struct nvme_sqe *src)
{
    const ULONG *s = (const ULONG *)src;
    ULONG       *d = (ULONG *)dst;
    for (int i = 0; i < 16; i++)
        dma_w32(&d[i], s[i]);
}

/* Polling timeout for admin command completions (~5s) */
#define ADMIN_POLL_ITERS  5000000UL

UWORD NVMe_AdminCmd(struct NVMeController *ctrl, struct nvme_sqe *sqe)
{
    struct NVMeBase  *devBase = ctrl->dev_base;
    struct ExecIFace *IExec   = devBase->IExec;
    ULONG             base    = ctrl->iobase;

    /* CRITICAL: mask admin interrupt (vector 0) for the duration of this
     * admin command.  We poll the admin CQ ourselves; we don't want an
     * IRQ to fire when the device posts the CQE.  On a shared level-
     * triggered INTx line (Pegasos2: nvme + virtioscsi + ide share the
     * same line), our ISR — which only inspects I/O CQs — correctly
     * returns "not ours" for an admin CQE.  No handler in the chain
     * claims, the line stays asserted, exec calls the chain again, and
     * so on: instant IRQ storm / hard-freeze.  Masking INTMS bit 0 at
     * the source is the simplest mechanical fix. */
    nvme_w32(base + NVME_REG_INTMS, 1);

    UWORD cid = ctrl->next_cmd_id++;
    sqe->cdw0 = (sqe->cdw0 & 0xFFFF) | ((ULONG)cid << 16);

    UWORD slot = ctrl->admin_sq_tail;
    DPRINTF(IExec, "[nvme.device:admin] enter cid=%u slot=%u sq_tail=%u"
                   " cq_head=%u cq_phase=%u opcode=0x%02lx\n",
            cid, slot, ctrl->admin_sq_tail, ctrl->admin_cq_head,
            (ULONG)ctrl->admin_cq_phase, (ULONG)(sqe->cdw0 & 0xFF));

    /* Copy SQE into admin submission queue with LE byte-swap. */
    struct nvme_sqe *sq = (struct nvme_sqe *)ctrl->admin_sq;
    write_sqe(&sq[slot], sqe);
    DPRINTF(IExec, "[nvme.device:admin] SQE written to sq[%u]\n", slot);

    /* Advance tail and ring doorbell. */
    ctrl->admin_sq_tail = (slot + 1) % NVME_ADMIN_QUEUE_DEPTH;
    ULONG db_addr = base + NVME_SQ_TAIL_DB(0, ctrl->cap_dstrd);
    DPRINTF(IExec, "[nvme.device:admin] ringing SQ tail doorbell @ 0x%08lx := %u\n",
            db_addr, ctrl->admin_sq_tail);
    nvme_w32(db_addr, ctrl->admin_sq_tail);
    DPRINTF(IExec, "[nvme.device:admin] doorbell rung; entering poll loop\n");

    /* Poll admin CQ — bounded 5 s to protect against a dead controller. */
    UBYTE *cq_base = (UBYTE *)ctrl->admin_cq;
    for (ULONG i = 0; i < ADMIN_POLL_ITERS; i++) {
        UBYTE *entry = cq_base + ctrl->admin_cq_head * NVME_CQE_SIZE;
        ULONG  dw3          = dma_r32(entry + 12);
        UWORD  phase_status = (UWORD)(dw3 >> 16);

        if (NVME_CQE_PHASE(phase_status) == ctrl->admin_cq_phase) {
            UWORD status_val = NVME_CQE_STATUS(phase_status);
            DPRINTF(IExec, "[nvme.device:admin] CQE matched at iter=%lu"
                           " cq[%u] raw_status=0x%04lx status=0x%04lx\n",
                    i, (UWORD)ctrl->admin_cq_head,
                    (ULONG)phase_status, (ULONG)status_val);
            ctrl->admin_cq_head = (ctrl->admin_cq_head + 1) % NVME_ADMIN_QUEUE_DEPTH;
            if (ctrl->admin_cq_head == 0)
                ctrl->admin_cq_phase ^= 1;
            nvme_w32(base + NVME_CQ_HEAD_DB(0, ctrl->cap_dstrd), ctrl->admin_cq_head);
            return status_val;
        }
        /* CFS watchdog every ~1 M spins. */
        if ((i & 0xFFFFF) == 0 && i > 0) {
            ULONG csts = NVME_R32(base, NULL, NVME_REG_CSTS);
            DPRINTF(IExec, "[nvme.device:admin] poll iter=%lu CSTS=0x%08lx"
                           " cq[%u] dw3=0x%08lx expected_phase=%u\n",
                    i, csts, (ULONG)ctrl->admin_cq_head,
                    dw3, (ULONG)ctrl->admin_cq_phase);
            if (csts & NVME_CSTS_CFS) {
                DPRINTF(IExec, "[nvme.device:admin] CFS asserted — abandoning cid=%u\n", cid);
                break;
            }
        }
    }

    DPRINTF(IExec, "[nvme.device:admin] Admin cmd timeout (cid=%u, ctrl=%lu)\n",
            cid, ctrl->ctrl_idx);
    return 0xFFFF;
}

BOOL NVMe_IdentifyController(struct NVMeController *ctrl)
{
    struct ExecIFace *IExec = ctrl->dev_base->IExec;

    struct nvme_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.cdw0    = NVME_ADMIN_IDENTIFY;
    sqe.nsid    = 0;
    sqe.prp1_lo = ctrl->identify_phys;
    sqe.cdw10   = NVME_ID_CNS_CONTROLLER;

    IExec->ObtainSemaphore(&ctrl->io_lock);
    UWORD status = NVMe_AdminCmd(ctrl, &sqe);
    IExec->ReleaseSemaphore(&ctrl->io_lock);

    if (status != NVME_STATUS_SUCCESS) {
        DLOG(IExec, "[nvme.device:admin] Identify Controller (ctrl %lu)"
                    " failed status=0x%04x\n", ctrl->ctrl_idx, status);
        return FALSE;
    }

    struct nvme_id_ctrl *id = (struct nvme_id_ctrl *)ctrl->identify_buf;

    /* Extract model / serial / firmware into the controller struct so
     * the stats snapshot can copy them without re-running Identify. */
    memcpy(ctrl->model,  id->mn, 40); ctrl->model[40]  = '\0';
    memcpy(ctrl->serial, id->sn, 20); ctrl->serial[20] = '\0';
    memcpy(ctrl->fw_rev, id->fr,  8); ctrl->fw_rev[8]  = '\0';
    for (int i = 39; i >= 0 && ctrl->model[i]  == ' '; i--) ctrl->model[i]  = '\0';
    for (int i = 19; i >= 0 && ctrl->serial[i] == ' '; i--) ctrl->serial[i] = '\0';
    for (int i =  7; i >= 0 && ctrl->fw_rev[i] == ' '; i--) ctrl->fw_rev[i] = '\0';

    const char *mn = ctrl->model;
    const char *fr = ctrl->fw_rev;

    /* MDTS=0 → unlimited (we cap at 1 MiB for sanity);
     * MDTS>0 → max transfer = 2^MDTS × page_size bytes. */
    if (id->mdts > 0)
        ctrl->max_transfer_bytes = (1u << id->mdts) * ctrl->page_size;
    else
        ctrl->max_transfer_bytes = 1024u * 1024u;

    DLOG(IExec, "[nvme.device:admin] ctrl %lu: \"%s\" FW \"%s\""
                " MDTS=%u max_xfer=%lu\n",
         ctrl->ctrl_idx, mn, fr, id->mdts, ctrl->max_transfer_bytes);

    return TRUE;
}

ULONG NVMe_IdentifyNSList(struct NVMeController *ctrl, ULONG *nsids, ULONG max_nsids)
{
    struct ExecIFace *IExec = ctrl->dev_base->IExec;

    struct nvme_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.cdw0    = NVME_ADMIN_IDENTIFY;
    sqe.nsid    = 0;
    sqe.prp1_lo = ctrl->identify_phys;
    sqe.cdw10   = NVME_ID_CNS_NS_LIST;

    IExec->ObtainSemaphore(&ctrl->io_lock);
    UWORD status = NVMe_AdminCmd(ctrl, &sqe);
    IExec->ReleaseSemaphore(&ctrl->io_lock);

    if (status != NVME_STATUS_SUCCESS) {
        DLOG(IExec, "[nvme.device:admin] Identify NS list (ctrl %lu)"
                    " failed status=0x%04x\n", ctrl->ctrl_idx, status);
        return 0;
    }

    ULONG *list  = (ULONG *)ctrl->identify_buf;
    ULONG  count = 0;
    for (ULONG i = 0; i < 1024 && count < max_nsids; i++) {
        ULONG nsid = dma_r32(&list[i]);
        if (nsid == 0) break;
        nsids[count++] = nsid;
    }

    DLOG(IExec, "[nvme.device:admin] ctrl %lu: %lu namespace(s)\n",
         ctrl->ctrl_idx, count);
    return count;
}

BOOL NVMe_IdentifyNamespace(struct NVMeController *ctrl, struct NVMeUnit *unit)
{
    struct ExecIFace *IExec = ctrl->dev_base->IExec;

    struct nvme_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.cdw0    = NVME_ADMIN_IDENTIFY;
    sqe.nsid    = unit->nsid;
    sqe.prp1_lo = ctrl->identify_phys;
    sqe.cdw10   = NVME_ID_CNS_NAMESPACE;

    IExec->ObtainSemaphore(&ctrl->io_lock);
    UWORD status = NVMe_AdminCmd(ctrl, &sqe);
    IExec->ReleaseSemaphore(&ctrl->io_lock);

    if (status != NVME_STATUS_SUCCESS) {
        DLOG(IExec, "[nvme.device:admin] Identify NS %lu (ctrl %lu) failed"
                    " status=0x%04x\n", unit->nsid, ctrl->ctrl_idx, status);
        return FALSE;
    }

    UBYTE *buf = (UBYTE *)ctrl->identify_buf;

    ULONG nsze_lo = dma_r32(buf + 0);
    ULONG nsze_hi = dma_r32(buf + 4);
    unit->total_blocks = ((uint64)nsze_hi << 32) | nsze_lo;

    UBYTE flbas_idx = buf[26] & 0xF;
    ULONG lbaf_val  = dma_r32(buf + 128 + flbas_idx * 4);
    ULONG lbads     = NVME_LBAF_LBADS(lbaf_val);
    unit->block_size  = 1u << lbads;
    unit->block_shift = lbads;

    DLOG(IExec, "[nvme.device:admin] ctrl %lu NS %lu: blocks=(hi:%lu lo:%lu)"
                " bytes/block=%lu\n",
         ctrl->ctrl_idx, unit->nsid,
         (ULONG)(unit->total_blocks >> 32), (ULONG)(unit->total_blocks & 0xFFFFFFFFu),
         unit->block_size);
    return TRUE;
}

BOOL NVMe_CreateIOCQ(struct NVMeController *ctrl, struct NVMeUnit *unit)
{
    struct ExecIFace *IExec = ctrl->dev_base->IExec;

    struct nvme_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.cdw0    = NVME_ADMIN_CREATE_CQ;
    sqe.prp1_lo = unit->io_cq_phys;
    sqe.cdw10   = ((ULONG)(NVME_IO_QUEUE_DEPTH - 1) << 16) | unit->queue_id;
    sqe.cdw11   = NVME_CQ_FLAGS_PC;  /* IEN=0: polling-style; PC=1 */

    IExec->ObtainSemaphore(&ctrl->io_lock);
    UWORD status = NVMe_AdminCmd(ctrl, &sqe);
    IExec->ReleaseSemaphore(&ctrl->io_lock);

    if (status != NVME_STATUS_SUCCESS) {
        DLOG(IExec, "[nvme.device:admin] Create IOCQ ctrl %lu qid %u failed"
                    " status=0x%04x\n", ctrl->ctrl_idx, unit->queue_id, status);
        return FALSE;
    }
    return TRUE;
}

BOOL NVMe_CreateIOSQ(struct NVMeController *ctrl, struct NVMeUnit *unit)
{
    struct ExecIFace *IExec = ctrl->dev_base->IExec;

    struct nvme_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.cdw0    = NVME_ADMIN_CREATE_SQ;
    sqe.prp1_lo = unit->io_sq_phys;
    sqe.cdw10   = ((ULONG)(NVME_IO_QUEUE_DEPTH - 1) << 16) | unit->queue_id;
    sqe.cdw11   = NVME_SQ_FLAGS_PC | NVME_SQ_PRIO_LOW |
                  ((ULONG)unit->queue_id << 16);

    IExec->ObtainSemaphore(&ctrl->io_lock);
    UWORD status = NVMe_AdminCmd(ctrl, &sqe);
    IExec->ReleaseSemaphore(&ctrl->io_lock);

    if (status != NVME_STATUS_SUCCESS) {
        DLOG(IExec, "[nvme.device:admin] Create IOSQ ctrl %lu qid %u failed"
                    " status=0x%04x\n", ctrl->ctrl_idx, unit->queue_id, status);
        return FALSE;
    }
    return TRUE;
}

#ifdef ENABLE_SMART

/* Read a 64-bit little-endian value from a DMA buffer using the
 * existing per-dword LE reader. */
static uint64 read_le64(const UBYTE *p)
{
    ULONG lo = dma_r32(p + 0);
    ULONG hi = dma_r32(p + 4);
    return ((uint64)hi << 32) | (uint64)lo;
}

BOOL NVMe_RefreshSMART(struct NVMeController *ctrl)
{
    struct ExecIFace *IExec = ctrl->dev_base->IExec;

    DPRINTF(IExec, "[nvme.device:smart] Refresh: enter ctrl=%lu\n", ctrl->ctrl_idx);

    /* SMART log is 512 bytes.  Reuse identify_buf as the destination —
     * admin commands are serialised under io_lock so no race with
     * any concurrent Identify. */
    ULONG log_bytes     = 512;
    ULONG num_dwords_m1 = (log_bytes / 4) - 1;   /* NUMDL field */

    struct nvme_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.cdw0    = NVME_ADMIN_GET_LOG_PAGE;
    sqe.nsid    = 0xFFFFFFFFu;      /* controller-wide SMART */
    sqe.prp1_lo = ctrl->identify_phys;
    sqe.cdw10   = (num_dwords_m1 << 16) | 0x02;  /* LID=0x02 (SMART) */
    sqe.cdw11   = 0;
    sqe.cdw12   = 0;                /* log page offset lo */
    sqe.cdw13   = 0;                /* log page offset hi */

    DPRINTF(IExec, "[nvme.device:smart] Refresh: memset identify_buf (phys=0x%08lx)\n",
            ctrl->identify_phys);
    memset(ctrl->identify_buf, 0, log_bytes);

    DPRINTF(IExec, "[nvme.device:smart] Refresh: ObtainSemaphore\n");
    IExec->ObtainSemaphore(&ctrl->io_lock);
    DPRINTF(IExec, "[nvme.device:smart] Refresh: calling NVMe_AdminCmd\n");
    UWORD status = NVMe_AdminCmd(ctrl, &sqe);
    DPRINTF(IExec, "[nvme.device:smart] Refresh: AdminCmd returned 0x%04lx\n",
            (ULONG)status);
    IExec->ReleaseSemaphore(&ctrl->io_lock);
    DPRINTF(IExec, "[nvme.device:smart] Refresh: released semaphore\n");

    if (status != NVME_STATUS_SUCCESS) {
        DPRINTF(IExec, "[nvme.device:admin] ctrl %lu SMART refresh failed"
                       " status=0x%04x\n", ctrl->ctrl_idx, status);
        return FALSE;
    }

    /* Parse the Health Information Log (NVMe 1.4 §5.14.1.2). */
    UBYTE *b = (UBYTE *)ctrl->identify_buf;
    struct NVMeSMARTCache *sc = &ctrl->smart_cache;

    sc->critical_warning = b[0];
    sc->temp_k           = (UWORD)(b[1] | (b[2] << 8));
    sc->spare_pct        = b[3];
    sc->spare_threshold  = b[4];
    sc->life_used_pct    = b[5];

    /* Each 16-byte total field is a 128-bit LE integer — we read the
     * low 64 bits only, which covers any non-enterprise device. */
    sc->data_read_units    = read_le64(b + 32);
    sc->data_written_units = read_le64(b + 48);
    sc->host_reads         = read_le64(b + 64);
    sc->host_writes        = read_le64(b + 80);
    sc->power_cycles       = read_le64(b + 112);
    sc->power_on_hours     = read_le64(b + 128);
    sc->unsafe_shutdowns   = read_le64(b + 144);
    sc->media_errors       = read_le64(b + 160);

    sc->last_refresh_tbr = nvme_read_tbr();
    sc->valid            = TRUE;

    DPRINTF(IExec, "[nvme.device:admin] ctrl %lu SMART: temp=%luK"
                   " spare=%u%% life=%u%% pcycles(lo)=%lu poh(lo)=%lu\n",
            ctrl->ctrl_idx, (ULONG)sc->temp_k,
            (ULONG)sc->spare_pct, (ULONG)sc->life_used_pct,
            (ULONG)(sc->power_cycles   & 0xFFFFFFFFu),
            (ULONG)(sc->power_on_hours & 0xFFFFFFFFu));
    return TRUE;
}

#endif /* ENABLE_SMART */
