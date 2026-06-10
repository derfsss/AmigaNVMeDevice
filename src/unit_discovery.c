/*
 * unit_discovery.c — enumerate namespaces on one NVMe controller,
 * build NVMeUnit structs, provision per-unit I/O queues, and announce
 * to mounter.library.
 *
 * Called once per controller from _manager_Init after that controller's
 * admin-queue bring-up.  For each active NSID returned by Identify
 * CNS=2 (capped at NVME_MAX_UNITS_PER_CTRL):
 *
 *   1. AllocVec the NVMeUnit; record flat unit_num, nsid, queue_id,
 *      and back-pointers (ctrl + dev_base).
 *   2. Identify Namespace (CNS=0) — populates block_size / total_blocks.
 *   3. Allocate the I/O SQ and CQ (page-aligned, MEMF_SHARED, DMA-mapped).
 *   4. Admin Create I/O CQ, then Admin Create I/O SQ.
 *   5. Add unit to devBase->global_units[] and announce via mounter.
 *
 * Unit tasks are NOT spawned here — they start lazily on first Open().
 *
 * Returns the number of units successfully brought up on this controller.
 */

#include "unit_discovery.h"
#include "nvme_admin.h"
#include "nvme_device.h"
#include "version.h"
#include <exec/exec.h>
#include <exec/memory.h>
#include <interfaces/mounter.h>
#include <libraries/mounter.h>

ULONG DiscoverUnits(struct NVMeController *ctrl)
{
    struct NVMeBase  *devBase = ctrl->dev_base;
    struct ExecIFace *IExec   = devBase->IExec;

    ULONG nsids[NVME_MAX_UNITS_PER_CTRL];
    ULONG count = NVMe_IdentifyNSList(ctrl, nsids, NVME_MAX_UNITS_PER_CTRL);
    if (count == 0) {
        DLOG(IExec, "[nvme.device:discovery] ctrl %lu: no namespaces\n",
             ctrl->ctrl_idx);
        return 0;
    }

    ULONG brought_up = 0;

    for (ULONG i = 0; i < count; i++) {
        if (devBase->num_global_units >= NVME_MAX_GLOBAL_UNITS) {
            DLOG(IExec, "[nvme.device:discovery] Global unit cap %u"
                        " reached; dropping NSIDs on ctrl %lu\n",
                 NVME_MAX_GLOBAL_UNITS, ctrl->ctrl_idx);
            break;
        }

        struct NVMeUnit *unit = IExec->AllocVecTags(sizeof(struct NVMeUnit),
            AVT_Type,  MEMF_PRIVATE,
            AVT_Clear, 0,
            TAG_DONE);
        if (!unit) {
            DLOG(IExec, "[nvme.device:discovery] AllocVec NVMeUnit failed\n");
            continue;
        }
        NVME_LEAK_INC(nvme_leak_vec);

        unit->unit_num    = devBase->num_global_units;  /* flat numbering */
        unit->nsid        = nsids[i];
        unit->queue_id    = (UWORD)(ctrl->num_units + 1);  /* 1-based, local */
        unit->queue_depth = ctrl->io_queue_depth;  /* MQES-clamped in InitNVMe */
        unit->ctrl        = ctrl;
        unit->dev_base    = devBase;

        if (!NVMe_IdentifyNamespace(ctrl, unit)) {
            IExec->FreeVec(unit);
            NVME_LEAK_DEC(nvme_leak_vec);
            continue;
        }

        ULONG sq_bytes = NVME_IO_QUEUE_DEPTH * NVME_SQE_SIZE;
        ULONG cq_bytes = NVME_IO_QUEUE_DEPTH * NVME_CQE_SIZE;

        unit->io_sq = IExec->AllocVecTags(sq_bytes,
            AVT_Type,      MEMF_SHARED,
            AVT_Alignment, ctrl->page_size,
            AVT_Clear,     0, TAG_DONE);
        if (unit->io_sq) NVME_LEAK_INC(nvme_leak_vec);
        unit->io_cq = IExec->AllocVecTags(cq_bytes,
            AVT_Type,      MEMF_SHARED,
            AVT_Alignment, ctrl->page_size,
            AVT_Clear,     0, TAG_DONE);
        if (unit->io_cq) NVME_LEAK_INC(nvme_leak_vec);
        if (!unit->io_sq || !unit->io_cq) {
            DLOG(IExec, "[nvme.device:discovery] I/O queue alloc failed"
                        " for NS %lu\n", nsids[i]);
            if (unit->io_sq) { IExec->FreeVec(unit->io_sq); NVME_LEAK_DEC(nvme_leak_vec); }
            if (unit->io_cq) { IExec->FreeVec(unit->io_cq); NVME_LEAK_DEC(nvme_leak_vec); }
            IExec->FreeVec(unit); NVME_LEAK_DEC(nvme_leak_vec);
            continue;
        }

        ULONG sq_ents = IExec->StartDMA(unit->io_sq, sq_bytes, DMA_ReadFromRAM);
        if (sq_ents == 0) {
            DLOG(IExec, "[nvme.device:discovery] StartDMA(io_sq) refused"
                        " for NS %lu\n", nsids[i]);
            IExec->FreeVec(unit->io_sq); NVME_LEAK_DEC(nvme_leak_vec);
            IExec->FreeVec(unit->io_cq); NVME_LEAK_DEC(nvme_leak_vec);
            IExec->FreeVec(unit);        NVME_LEAK_DEC(nvme_leak_vec);
            continue;
        }
        NVME_LEAK_INC(nvme_leak_dma);
        struct DMAEntry *sq_dma = IExec->AllocSysObjectTags(ASOT_DMAENTRY,
            ASODMAE_NumEntries, sq_ents, TAG_DONE);
        if (sq_dma) NVME_LEAK_INC(nvme_leak_dmaentry);
        ULONG cq_ents = IExec->StartDMA(unit->io_cq, cq_bytes, DMA_ReadFromRAM);
        if (cq_ents == 0) {
            DLOG(IExec, "[nvme.device:discovery] StartDMA(io_cq) refused"
                        " for NS %lu\n", nsids[i]);
            if (sq_dma) { IExec->FreeSysObject(ASOT_DMAENTRY, sq_dma); NVME_LEAK_DEC(nvme_leak_dmaentry); }
            IExec->EndDMA(unit->io_sq, sq_bytes, DMA_ReadFromRAM); NVME_LEAK_DEC(nvme_leak_dma);
            IExec->FreeVec(unit->io_sq); NVME_LEAK_DEC(nvme_leak_vec);
            IExec->FreeVec(unit->io_cq); NVME_LEAK_DEC(nvme_leak_vec);
            IExec->FreeVec(unit);        NVME_LEAK_DEC(nvme_leak_vec);
            continue;
        }
        NVME_LEAK_INC(nvme_leak_dma);
        struct DMAEntry *cq_dma = IExec->AllocSysObjectTags(ASOT_DMAENTRY,
            ASODMAE_NumEntries, cq_ents, TAG_DONE);
        if (cq_dma) NVME_LEAK_INC(nvme_leak_dmaentry);

        if (!sq_dma || !cq_dma) {
            if (sq_dma) { IExec->FreeSysObject(ASOT_DMAENTRY, sq_dma); NVME_LEAK_DEC(nvme_leak_dmaentry); }
            if (cq_dma) { IExec->FreeSysObject(ASOT_DMAENTRY, cq_dma); NVME_LEAK_DEC(nvme_leak_dmaentry); }
            IExec->EndDMA(unit->io_sq, sq_bytes, DMA_ReadFromRAM); NVME_LEAK_DEC(nvme_leak_dma);
            IExec->EndDMA(unit->io_cq, cq_bytes, DMA_ReadFromRAM); NVME_LEAK_DEC(nvme_leak_dma);
            IExec->FreeVec(unit->io_sq); NVME_LEAK_DEC(nvme_leak_vec);
            IExec->FreeVec(unit->io_cq); NVME_LEAK_DEC(nvme_leak_vec);
            IExec->FreeVec(unit);        NVME_LEAK_DEC(nvme_leak_vec);
            continue;
        }

        IExec->GetDMAList(unit->io_sq, sq_bytes, DMA_ReadFromRAM, sq_dma);
        IExec->GetDMAList(unit->io_cq, cq_bytes, DMA_ReadFromRAM, cq_dma);
        unit->io_sq_phys  = (ULONG)sq_dma[0].PhysicalAddress;
        unit->io_cq_phys  = (ULONG)cq_dma[0].PhysicalAddress;
        unit->io_cq_phase = 1;

        /* Physical addresses are captured; free the entry arrays. */
        IExec->FreeSysObject(ASOT_DMAENTRY, sq_dma); NVME_LEAK_DEC(nvme_leak_dmaentry);
        IExec->FreeSysObject(ASOT_DMAENTRY, cq_dma); NVME_LEAK_DEC(nvme_leak_dmaentry);

        if (!NVMe_CreateIOCQ(ctrl, unit) || !NVMe_CreateIOSQ(ctrl, unit)) {
            IExec->EndDMA(unit->io_sq, sq_bytes, DMA_ReadFromRAM); NVME_LEAK_DEC(nvme_leak_dma);
            IExec->EndDMA(unit->io_cq, cq_bytes, DMA_ReadFromRAM); NVME_LEAK_DEC(nvme_leak_dma);
            IExec->FreeVec(unit->io_sq); NVME_LEAK_DEC(nvme_leak_vec);
            IExec->FreeVec(unit->io_cq); NVME_LEAK_DEC(nvme_leak_vec);
            IExec->FreeVec(unit);        NVME_LEAK_DEC(nvme_leak_vec);
            continue;
        }

        /* Register unit in both the per-controller and the flat table. */
        ctrl->units[ctrl->num_units++]                 = unit;
        devBase->global_units[devBase->num_global_units++] = unit;
        brought_up++;

        DLOG(IExec, "[nvme.device:discovery] ctrl %lu unit %lu (NS %lu):"
                    " blocks=(hi:%lu lo:%lu) bytes/block=%lu\n",
             ctrl->ctrl_idx, unit->unit_num, unit->nsid,
             (ULONG)(unit->total_blocks >> 32),
             (ULONG)(unit->total_blocks & 0xFFFFFFFFu),
             unit->block_size);
    }

    /* Announce each unit on this controller to mounter.library. */
    if (brought_up == 0) return 0;

    struct Library      *MounterBase = IExec->OpenLibrary("mounter.library", 0);
    struct MounterIFace *IMounter    = MounterBase ?
        (struct MounterIFace *)IExec->GetInterface(MounterBase, "main", 1, NULL)
                                                  : NULL;

    if (IMounter) {
        for (ULONG i = ctrl->num_units - brought_up; i < ctrl->num_units; i++) {
            DLOG(IExec, "[nvme.device:discovery] Announcing unit %lu\n",
                 ctrl->units[i]->unit_num);
            IMounter->AnnounceDeviceTags(DEVNAME, ctrl->units[i]->unit_num, TAG_END);
        }
        IExec->DropInterface((struct Interface *)IMounter);
    } else {
        DLOG(IExec, "[nvme.device:discovery] mounter.library unavailable\n");
    }

    if (MounterBase)
        IExec->CloseLibrary(MounterBase);

    return brought_up;
}
