#include "unit_discovery.h"
#include "nvme_admin.h"
#include "nvme_device.h"
#include <exec/exec.h>
#include <exec/memory.h>
#include <proto/mounter.h>

void DiscoverUnits(struct NVMeBase *devBase)
{
    struct ExecIFace *IExec = devBase->IExec;

    /* Get active namespace list */
    ULONG nsids[8];
    ULONG count = NVMe_IdentifyNSList(devBase, nsids, 8);
    if (count == 0) {
        IExec->DebugPrintF("[nvme.device:discovery] No namespaces found\n");
        return;
    }

    for (ULONG i = 0; i < count; i++) {
        struct NVMeUnit *unit = IExec->AllocVecTags(sizeof(struct NVMeUnit),
            AVT_Type,  MEMF_PRIVATE,
            AVT_Clear, 0,
            TAG_DONE);
        if (!unit) {
            IExec->DebugPrintF("[nvme.device:discovery] AllocVec failed for unit %lu\n", i);
            continue;
        }

        unit->unit_num  = i;
        unit->nsid      = nsids[i];
        unit->queue_id  = (UWORD)(i + 1); /* NVMe queue IDs are 1-based */
        unit->queue_depth = NVME_IO_QUEUE_DEPTH;
        unit->dev_base  = devBase;

        /* Identify namespace — get geometry */
        if (!NVMe_IdentifyNamespace(devBase, unit)) {
            IExec->FreeVec(unit);
            continue;
        }

        /* Allocate I/O queues for this unit */
        ULONG sq_bytes = NVME_IO_QUEUE_DEPTH * NVME_SQE_SIZE;
        ULONG cq_bytes = NVME_IO_QUEUE_DEPTH * NVME_CQE_SIZE;

        unit->io_sq = IExec->AllocVecTags(sq_bytes,
            AVT_Type,      MEMF_SHARED,
            AVT_Alignment, devBase->page_size,
            AVT_Clear,     0, TAG_DONE);
        unit->io_cq = IExec->AllocVecTags(cq_bytes,
            AVT_Type,      MEMF_SHARED,
            AVT_Alignment, devBase->page_size,
            AVT_Clear,     0, TAG_DONE);
        if (!unit->io_sq || !unit->io_cq) {
            IExec->DebugPrintF("[nvme.device:discovery] Failed to alloc I/O queues for NS %lu\n", nsids[i]);
            if (unit->io_sq) IExec->FreeVec(unit->io_sq);
            if (unit->io_cq) IExec->FreeVec(unit->io_cq);
            IExec->FreeVec(unit);
            continue;
        }

        /* DMA-map I/O queues */
        ULONG sq_ents = IExec->StartDMA(unit->io_sq, sq_bytes, DMA_ReadFromRAM);
        struct DMAEntry *sq_dma = IExec->AllocSysObjectTags(ASOT_DMAENTRY,
            ASODMAE_NumEntries, sq_ents, TAG_DONE);
        ULONG cq_ents = IExec->StartDMA(unit->io_cq, cq_bytes, DMA_ReadFromRAM);
        struct DMAEntry *cq_dma = IExec->AllocSysObjectTags(ASOT_DMAENTRY,
            ASODMAE_NumEntries, cq_ents, TAG_DONE);

        if (!sq_dma || !cq_dma) {
            if (sq_dma) IExec->FreeSysObject(ASOT_DMAENTRY, sq_dma);
            if (cq_dma) IExec->FreeSysObject(ASOT_DMAENTRY, cq_dma);
            IExec->EndDMA(unit->io_sq, sq_bytes, DMA_ReadFromRAM);
            IExec->EndDMA(unit->io_cq, cq_bytes, DMA_ReadFromRAM);
            IExec->FreeVec(unit->io_sq);
            IExec->FreeVec(unit->io_cq);
            IExec->FreeVec(unit);
            continue;
        }

        IExec->GetDMAList(unit->io_sq, sq_bytes, DMA_ReadFromRAM, sq_dma);
        IExec->GetDMAList(unit->io_cq, cq_bytes, DMA_ReadFromRAM, cq_dma);
        unit->io_sq_phys = (ULONG)sq_dma[0].PhysicalAddress;
        unit->io_cq_phys = (ULONG)cq_dma[0].PhysicalAddress;
        unit->io_cq_phase = 1; /* phase starts at 1 */

        /* Immediately free the DMA entry arrays — physical addresses cached */
        IExec->FreeSysObject(ASOT_DMAENTRY, sq_dma);
        IExec->FreeSysObject(ASOT_DMAENTRY, cq_dma);

        /* Create I/O CQ then SQ via admin commands */
        if (!NVMe_CreateIOCQ(devBase, unit) || !NVMe_CreateIOSQ(devBase, unit)) {
            IExec->EndDMA(unit->io_sq, sq_bytes, DMA_ReadFromRAM);
            IExec->EndDMA(unit->io_cq, cq_bytes, DMA_ReadFromRAM);
            IExec->FreeVec(unit->io_sq);
            IExec->FreeVec(unit->io_cq);
            IExec->FreeVec(unit);
            continue;
        }

        devBase->units[devBase->num_units] = unit;
        devBase->num_units++;

        IExec->DebugPrintF("[nvme.device:discovery] Unit %lu: NS %lu, %llu blocks x %lu bytes\n",
                           unit->unit_num, unit->nsid, unit->total_blocks, unit->block_size);

        /* Announce to mounter.library so filesystems get mounted */
        /* TODO: implement mounter.library announce (same pattern as virtioscsi.device) */
    }
}
