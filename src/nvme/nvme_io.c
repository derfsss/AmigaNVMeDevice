#include "nvme_io.h"
#include "nvme_device.h"
#include <exec/exec.h>
#include <exec/memory.h>
#include <string.h>

/* Find a free inflight slot for unit. Returns index or -1 if full. */
static LONG find_free_slot(struct NVMeUnit *unit)
{
    for (int i = 0; i < NVME_MAX_INFLIGHT; i++) {
        if (unit->inflight[i].ioreq == NULL)
            return i;
    }
    return -1;
}

/* Build and submit a read or write SQE for the given IOStdReq. */
LONG NVMeIO_Submit(struct NVMeBase *devBase, struct NVMeUnit *unit,
                   struct IOStdReq *ioreq, BOOL is_write)
{
    struct ExecIFace *IExec  = devBase->IExec;
    struct PCIDevice *pciDev = devBase->pciDevice;
    ULONG             base   = devBase->iobase;

    LONG slot = find_free_slot(unit);
    if (slot < 0)
        return -1; /* no free slot — caller falls back to synchronous */

    /* Compute LBA and block count from IOStdReq */
    UQUAD byte_offset;
    ULONG byte_length = ioreq->io_Length;

    if (ioreq->io_Command == TD_READ64 || ioreq->io_Command == TD_WRITE64 ||
        ioreq->io_Command == NSCMD_TD_READ64 || ioreq->io_Command == NSCMD_TD_WRITE64) {
        /* 64-bit offset: high 32 bits in io_Actual for TD_READ64/WRITE64 */
        /* TODO: verify the actual field layout for 64-bit offset on OS4 */
        byte_offset = (UQUAD)ioreq->io_Offset;
    } else {
        byte_offset = (UQUAD)ioreq->io_Offset;
    }

    UQUAD start_lba = byte_offset >> unit->block_shift;
    ULONG nlb        = (byte_length >> unit->block_shift); /* block count */
    if (nlb == 0) {
        ioreq->io_Error  = 0;
        ioreq->io_Actual = 0;
        return -2; /* nothing to do */
    }

    struct NVMeInflight *inf = &unit->inflight[slot];
    inf->ioreq    = ioreq;
    inf->is_write = is_write;
    inf->use_bounce = FALSE;

    APTR   data_virt = ioreq->io_Data;
    ULONG  prp1_phys, prp2_phys;

    /* Choose bounce vs. direct DMA */
    if (byte_length <= NVME_BOUNCE_SIZE) {
        inf->use_bounce = TRUE;
        if (is_write)
            memcpy(unit->bounce_bufs[slot], data_virt, byte_length);
        prp1_phys = unit->bounce_phys[slot];
        prp2_phys = 0;
    } else {
        /* Direct DMA: StartDMA on user buffer */
        ULONG flags = is_write ? DMA_ReadFromRAM : DMA_WriteToRAM;
        ULONG entries = IExec->StartDMA(data_virt, byte_length, flags);
        inf->dma_list = IExec->AllocSysObjectTags(ASOT_DMAENTRY,
            ASODMAE_NumEntries, entries, TAG_DONE);
        if (!inf->dma_list) {
            inf->ioreq = NULL;
            ioreq->io_Error = IOERR_NOMEMORY;
            return -3;
        }
        IExec->GetDMAList(data_virt, byte_length, flags, inf->dma_list);
        inf->dma_buf     = data_virt;
        inf->dma_phys    = (ULONG)inf->dma_list[0].PhysicalAddress;
        inf->dma_size    = byte_length;
        inf->dma_entries = entries;

        prp1_phys = inf->dma_phys;

        /* Build PRP2: either second page or PRP list */
        if (byte_length <= devBase->page_size) {
            prp2_phys = 0;
        } else if (byte_length <= 2 * devBase->page_size) {
            prp2_phys = inf->dma_list[1].PhysicalAddress;
        } else {
            /* PRP list needed: fill unit->prp_list_pages[slot] */
            ULONG  *prp_list = (ULONG *)unit->prp_list_pages[slot];
            ULONG   offset   = devBase->page_size; /* first entry is page 2 */
            ULONG   prp_idx  = 0;
            while (offset < byte_length) {
                /* Each DMAEntry covers one contiguous physical block;
                 * for now assume contiguous (single DMAEntry from QEMU) */
                prp_list[prp_idx * 2]     = inf->dma_phys + offset;
                prp_list[prp_idx * 2 + 1] = 0; /* upper 32 bits */
                prp_idx++;
                offset += devBase->page_size;
            }
            prp2_phys = unit->prp_list_phys[slot];
        }
    }

    /* Assign command ID = (slot + 1) to avoid 0 */
    UWORD cid = (UWORD)(slot + 1);
    inf->cmd_id = cid;

    /* Build I/O SQE */
    struct nvme_sqe *sq = (struct nvme_sqe *)unit->io_sq;
    struct nvme_sqe *entry = &sq[unit->io_sq_tail];
    memset(entry, 0, sizeof(*entry));
    entry->cdw0    = (is_write ? NVME_CMD_WRITE : NVME_CMD_READ) | ((ULONG)cid << 16);
    entry->nsid    = unit->nsid;
    entry->prp1_lo = prp1_phys;
    entry->prp1_hi = 0;
    entry->prp2_lo = prp2_phys;
    entry->prp2_hi = 0;
    entry->cdw10   = (ULONG)(start_lba & 0xFFFFFFFF);       /* SLBA[31:0] */
    entry->cdw11   = (ULONG)(start_lba >> 32);              /* SLBA[63:32] */
    entry->cdw12   = (nlb - 1) & 0xFFFF;                    /* NLB (0-based) */

    /* Advance SQ tail and ring doorbell */
    unit->io_sq_tail = (unit->io_sq_tail + 1) % unit->queue_depth;
    NVME_W32(base, pciDev,
             NVME_SQ_TAIL_DB(unit->queue_id, devBase->cap_dstrd),
             unit->io_sq_tail);

    return 0; /* submitted; Harvest will ReplyMsg when done */
}

LONG NVMeIO_Flush(struct NVMeBase *devBase, struct NVMeUnit *unit,
                  struct IOStdReq *ioreq)
{
    struct ExecIFace *IExec  = devBase->IExec;
    struct PCIDevice *pciDev = devBase->pciDevice;
    ULONG             base   = devBase->iobase;

    LONG slot = find_free_slot(unit);
    if (slot < 0) return -1;

    UWORD cid = (UWORD)(slot + 1);
    unit->inflight[slot].ioreq    = ioreq;
    unit->inflight[slot].cmd_id   = cid;
    unit->inflight[slot].is_write = FALSE;
    unit->inflight[slot].use_bounce = FALSE;
    unit->inflight[slot].dma_list   = NULL;

    struct nvme_sqe *sq = (struct nvme_sqe *)unit->io_sq;
    struct nvme_sqe *entry = &sq[unit->io_sq_tail];
    memset(entry, 0, sizeof(*entry));
    entry->cdw0 = NVME_CMD_FLUSH | ((ULONG)cid << 16);
    entry->nsid = unit->nsid;

    unit->io_sq_tail = (unit->io_sq_tail + 1) % unit->queue_depth;
    NVME_W32(base, pciDev,
             NVME_SQ_TAIL_DB(unit->queue_id, devBase->cap_dstrd),
             unit->io_sq_tail);
    return 0;
}

void NVMeIO_Harvest(struct NVMeBase *devBase, struct NVMeUnit *unit)
{
    struct ExecIFace *IExec  = devBase->IExec;
    struct PCIDevice *pciDev = devBase->pciDevice;
    ULONG             base   = devBase->iobase;

    struct nvme_cqe *cq = (struct nvme_cqe *)unit->io_cq;

    while (1) {
        struct nvme_cqe *entry = &cq[unit->io_cq_head];
        if (NVME_CQE_PHASE(entry->status) != unit->io_cq_phase)
            break; /* no more completions */

        UWORD cid    = entry->cid;
        UWORD status = entry->status;

        /* Advance CQ head and ring doorbell */
        unit->io_cq_head = (unit->io_cq_head + 1) % unit->queue_depth;
        if (unit->io_cq_head == 0)
            unit->io_cq_phase ^= 1;
        NVME_W32(base, pciDev,
                 NVME_CQ_HEAD_DB(unit->queue_id, devBase->cap_dstrd),
                 unit->io_cq_head);

        /* Find inflight slot by cid (cid = slot + 1) */
        LONG slot = (LONG)cid - 1;
        if (slot < 0 || slot >= NVME_MAX_INFLIGHT || !unit->inflight[slot].ioreq) {
            DPRINTF(IExec, "[nvme.device:io] Harvest: unmatched cid %u\n", cid);
            continue;
        }

        struct NVMeInflight *inf = &unit->inflight[slot];
        struct IOStdReq     *req = inf->ioreq;

        /* Decode completion status */
        UWORD sc = NVME_CQE_SC(status);
        if (sc == 0) {
            req->io_Error  = 0;
            req->io_Actual = req->io_Length;
        } else {
            DPRINTF(IExec, "[nvme.device:io] Harvest: NVMe error sc=0x%02x for slot %ld\n",
                    sc, slot);
            req->io_Error  = IOERR_BADADDRESS; /* generic I/O error */
            req->io_Actual = 0;
        }

        /* Clean up DMA / bounce */
        if (inf->use_bounce) {
            if (!inf->is_write && sc == 0)
                memcpy(req->io_Data, unit->bounce_bufs[slot], req->io_Length);
        } else if (inf->dma_list) {
            ULONG flags = inf->is_write ? DMA_ReadFromRAM : DMA_WriteToRAM;
            IExec->EndDMA(inf->dma_buf, inf->dma_size, flags);
            IExec->FreeSysObject(ASOT_DMAENTRY, inf->dma_list);
            inf->dma_list = NULL;
        }

        inf->ioreq = NULL; /* free slot */
        IExec->ReplyMsg((struct Message *)req);
    }
}
