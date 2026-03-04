#include "nvme_admin.h"
#include "nvme_device.h"
#include <exec/exec.h>
#include <string.h>

/* Polling timeout for admin command completions (~5s) */
#define ADMIN_POLL_ITERS  5000000UL

/*
 * Submit one admin SQE and poll the admin CQ for its completion.
 * Must be called with devBase->io_lock held.
 * Returns the 16-bit status word (0 = success).
 */
UWORD NVMe_AdminCmd(struct NVMeBase *devBase, struct nvme_sqe *sqe)
{
    struct ExecIFace *IExec  = devBase->IExec;
    struct PCIDevice *pciDev = devBase->pciDevice;
    ULONG             base   = devBase->iobase;

    /* Assign command ID */
    UWORD cid = devBase->next_cmd_id++;
    sqe->cdw0 = (sqe->cdw0 & 0xFFFF) | ((ULONG)cid << 16);

    /* Copy SQE into admin submission queue */
    struct nvme_sqe *sq = (struct nvme_sqe *)devBase->admin_sq;
    sq[devBase->admin_sq_tail] = *sqe;

    /* Advance tail and ring doorbell */
    devBase->admin_sq_tail = (devBase->admin_sq_tail + 1) % NVME_ADMIN_QUEUE_DEPTH;
    NVME_W32(base, pciDev, NVME_SQ_TAIL_DB(0, devBase->cap_dstrd),
             devBase->admin_sq_tail);

    /* Poll admin CQ for matching completion */
    struct nvme_cqe *cq = (struct nvme_cqe *)devBase->admin_cq;
    for (ULONG i = 0; i < ADMIN_POLL_ITERS; i++) {
        struct nvme_cqe *entry = &cq[devBase->admin_cq_head];
        if (NVME_CQE_PHASE(entry->status) == devBase->admin_cq_phase) {
            /* Got a completion — check it matches our command */
            UWORD status = entry->status;
            /* Advance head and ring CQ head doorbell */
            devBase->admin_cq_head = (devBase->admin_cq_head + 1) % NVME_ADMIN_QUEUE_DEPTH;
            if (devBase->admin_cq_head == 0)
                devBase->admin_cq_phase ^= 1; /* phase flips on wrap */
            NVME_W32(base, pciDev, NVME_CQ_HEAD_DB(0, devBase->cap_dstrd),
                     devBase->admin_cq_head);
            return NVME_CQE_STATUS(status);
        }
    }

    IExec->DebugPrintF("[nvme.device:admin] Admin command timeout (cid=%u)\n", cid);
    return 0xFFFF; /* timeout sentinel */
}

BOOL NVMe_IdentifyController(struct NVMeBase *devBase)
{
    struct ExecIFace *IExec = devBase->IExec;

    struct nvme_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.cdw0  = NVME_ADMIN_IDENTIFY;      /* opcode */
    sqe.nsid  = 0;
    sqe.prp1_lo = devBase->identify_phys;
    sqe.prp1_hi = 0;
    sqe.cdw10 = NVME_ID_CNS_CONTROLLER;

    IExec->ObtainSemaphore(&devBase->io_lock);
    UWORD status = NVMe_AdminCmd(devBase, &sqe);
    IExec->ReleaseSemaphore(&devBase->io_lock);

    if (status != NVME_STATUS_SUCCESS) {
        IExec->DebugPrintF("[nvme.device:admin] Identify Controller failed, status 0x%04x\n", status);
        return FALSE;
    }

    struct nvme_id_ctrl *ctrl = (struct nvme_id_ctrl *)devBase->identify_buf;

    /* Null-terminate and print model number */
    char mn[41];
    memcpy(mn, ctrl->mn, 40);
    mn[40] = '\0';
    /* Trim trailing spaces */
    for (int i = 39; i >= 0 && mn[i] == ' '; i--) mn[i] = '\0';

    char fr[9];
    memcpy(fr, ctrl->fr, 8);
    fr[8] = '\0';

    IExec->DebugPrintF("[nvme.device:admin] Controller: \"%s\" FW: \"%s\" MDTS=%u\n",
                       mn, fr, ctrl->mdts);

    return TRUE;
}

ULONG NVMe_IdentifyNSList(struct NVMeBase *devBase, ULONG *nsids, ULONG max_nsids)
{
    struct ExecIFace *IExec = devBase->IExec;

    struct nvme_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.cdw0    = NVME_ADMIN_IDENTIFY;
    sqe.nsid    = 0;
    sqe.prp1_lo = devBase->identify_phys;
    sqe.prp1_hi = 0;
    sqe.cdw10   = NVME_ID_CNS_NS_LIST;

    IExec->ObtainSemaphore(&devBase->io_lock);
    UWORD status = NVMe_AdminCmd(devBase, &sqe);
    IExec->ReleaseSemaphore(&devBase->io_lock);

    if (status != NVME_STATUS_SUCCESS) {
        IExec->DebugPrintF("[nvme.device:admin] Identify NS List failed, status 0x%04x\n", status);
        return 0;
    }

    ULONG *list = (ULONG *)devBase->identify_buf;
    ULONG  count = 0;
    for (ULONG i = 0; i < 1024 && count < max_nsids; i++) {
        if (list[i] == 0) break; /* list terminated by NSID=0 */
        nsids[count++] = list[i];
    }

    IExec->DebugPrintF("[nvme.device:admin] Found %lu namespace(s)\n", count);
    return count;
}

BOOL NVMe_IdentifyNamespace(struct NVMeBase *devBase, struct NVMeUnit *unit)
{
    struct ExecIFace *IExec = devBase->IExec;

    struct nvme_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.cdw0    = NVME_ADMIN_IDENTIFY;
    sqe.nsid    = unit->nsid;
    sqe.prp1_lo = devBase->identify_phys;
    sqe.prp1_hi = 0;
    sqe.cdw10   = NVME_ID_CNS_NAMESPACE;

    IExec->ObtainSemaphore(&devBase->io_lock);
    UWORD status = NVMe_AdminCmd(devBase, &sqe);
    IExec->ReleaseSemaphore(&devBase->io_lock);

    if (status != NVME_STATUS_SUCCESS) {
        IExec->DebugPrintF("[nvme.device:admin] Identify NS %lu failed, status 0x%04x\n",
                           unit->nsid, status);
        return FALSE;
    }

    struct nvme_id_ns *ns = (struct nvme_id_ns *)devBase->identify_buf;

    unit->total_blocks = ns->nsze;

    /* Extract LBA size from formatted LBA format */
    UBYTE flbas_idx = ns->flbas & 0xF;
    ULONG ds = NVME_LBAF_DS(ns->lbaf[flbas_idx].ds);
    unit->block_size  = 512u << ds;
    unit->block_shift = 9 + ds;

    IExec->DebugPrintF("[nvme.device:admin] NS %lu: %llu blocks, %lu bytes/block\n",
                       unit->nsid, unit->total_blocks, unit->block_size);
    return TRUE;
}

BOOL NVMe_CreateIOCQ(struct NVMeBase *devBase, struct NVMeUnit *unit)
{
    struct ExecIFace *IExec = devBase->IExec;

    struct nvme_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.cdw0    = NVME_ADMIN_CREATE_CQ;
    sqe.nsid    = 0;
    sqe.prp1_lo = unit->io_cq_phys;
    sqe.prp1_hi = 0;
    /* CDW10: QSIZE [31:16] = depth-1, QID [15:0] = queue ID */
    sqe.cdw10   = ((ULONG)(NVME_IO_QUEUE_DEPTH - 1) << 16) | unit->queue_id;
    /* CDW11: IEN=0 (use polling), PC=1 (physically contiguous) */
    sqe.cdw11   = NVME_CQ_FLAGS_PC;

    IExec->ObtainSemaphore(&devBase->io_lock);
    UWORD status = NVMe_AdminCmd(devBase, &sqe);
    IExec->ReleaseSemaphore(&devBase->io_lock);

    if (status != NVME_STATUS_SUCCESS) {
        IExec->DebugPrintF("[nvme.device:admin] Create IO CQ %lu failed, status 0x%04x\n",
                           unit->queue_id, status);
        return FALSE;
    }
    return TRUE;
}

BOOL NVMe_CreateIOSQ(struct NVMeBase *devBase, struct NVMeUnit *unit)
{
    struct ExecIFace *IExec = devBase->IExec;

    struct nvme_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.cdw0    = NVME_ADMIN_CREATE_SQ;
    sqe.nsid    = 0;
    sqe.prp1_lo = unit->io_sq_phys;
    sqe.prp1_hi = 0;
    /* CDW10: QSIZE [31:16] = depth-1, QID [15:0] = queue ID */
    sqe.cdw10   = ((ULONG)(NVME_IO_QUEUE_DEPTH - 1) << 16) | unit->queue_id;
    /* CDW11: CQID [31:16] = paired CQ ID, PRIO=low, PC=1 */
    sqe.cdw11   = NVME_SQ_FLAGS_PC | NVME_SQ_PRIO_LOW |
                  ((ULONG)unit->queue_id << 16);

    IExec->ObtainSemaphore(&devBase->io_lock);
    UWORD status = NVMe_AdminCmd(devBase, &sqe);
    IExec->ReleaseSemaphore(&devBase->io_lock);

    if (status != NVME_STATUS_SUCCESS) {
        IExec->DebugPrintF("[nvme.device:admin] Create IO SQ %lu failed, status 0x%04x\n",
                           unit->queue_id, status);
        return FALSE;
    }
    return TRUE;
}
