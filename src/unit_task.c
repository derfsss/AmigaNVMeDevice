#include "unit_task.h"
#include "nvme_io.h"
#include "nvme_device.h"
#include <exec/exec.h>
#include <exec/memory.h>
#include <devices/trackdisk.h>
#include <devices/newstyle.h>

/* ------------------------------------------------------------------ */
/* Unit task dispatch — handle a single IOStdReq from the message port */
/* ------------------------------------------------------------------ */

static void dispatch_ioreq(struct NVMeBase *devBase, struct NVMeUnit *unit,
                             struct IOStdReq *ioreq)
{
    struct ExecIFace *IExec = devBase->IExec;

    switch (ioreq->io_Command) {

    case CMD_READ:
    case TD_READ64:
    case NSCMD_TD_READ64: {
        LONG rc = NVMeIO_Submit(devBase, unit, ioreq, FALSE);
        if (rc == 0) return; /* async — Harvest will ReplyMsg */
        if (rc == -1) {
            /* Queue full: fall back to synchronous (just error for now) */
            ioreq->io_Error = IOERR_UNITBUSY;
            IExec->ReplyMsg((struct Message *)ioreq);
        } else {
            /* error already set or nothing to do */
            IExec->ReplyMsg((struct Message *)ioreq);
        }
        return;
    }

    case CMD_WRITE:
    case TD_WRITE64:
    case NSCMD_TD_WRITE64: {
        LONG rc = NVMeIO_Submit(devBase, unit, ioreq, TRUE);
        if (rc == 0) return;
        if (rc == -1) ioreq->io_Error = IOERR_UNITBUSY;
        IExec->ReplyMsg((struct Message *)ioreq);
        return;
    }

    case CMD_UPDATE: {
        LONG rc = NVMeIO_Flush(devBase, unit, ioreq);
        if (rc == 0) return;
        if (rc == -1) ioreq->io_Error = IOERR_UNITBUSY;
        IExec->ReplyMsg((struct Message *)ioreq);
        return;
    }

    case TD_GETGEOMETRY: {
        struct DriveGeometry *dg = (struct DriveGeometry *)ioreq->io_Data;
        if (dg && ioreq->io_Length >= sizeof(struct DriveGeometry)) {
            dg->dg_SectorSize   = unit->block_size;
            dg->dg_TotalSectors = (ULONG)(unit->total_blocks > 0xFFFFFFFFULL ?
                                          0xFFFFFFFF : (ULONG)unit->total_blocks);
            dg->dg_Cylinders    = 0;
            dg->dg_CylSectors   = 0;
            dg->dg_Heads        = 0;
            dg->dg_TrackSectors = 0;
            dg->dg_BufMemType   = MEMF_PUBLIC;
            dg->dg_DeviceType   = DG_DIRECT_ACCESS;
            dg->dg_Flags        = 0;
            ioreq->io_Actual    = sizeof(struct DriveGeometry);
            ioreq->io_Error     = 0;
        } else {
            ioreq->io_Error = IOERR_BADLENGTH;
        }
        IExec->ReplyMsg((struct Message *)ioreq);
        return;
    }

    case HD_SCSICMD:
        /* NVMe is not SCSI — return IOERR_NOCMD */
        ioreq->io_Error = IOERR_NOCMD;
        IExec->ReplyMsg((struct Message *)ioreq);
        return;

    default:
        ioreq->io_Error = IOERR_NOCMD;
        IExec->ReplyMsg((struct Message *)ioreq);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Unit task entry point                                               */
/* ------------------------------------------------------------------ */

static void unit_task_entry(void)
{
    struct ExecIFace *IExec;
    struct NVMeUnit  *unit;
    struct NVMeBase  *devBase;
    struct Task      *parent;

    /* Retrieve arguments passed via task user data */
    struct Task *self = ((struct ExecIFace *)((struct ExecBase *)*((ULONG *)4)
        /* This will be populated properly via AllocSysObjectTags ASOT_TASK userData */ ))->FindTask(NULL);

    /* TODO: use proper task userData passing — for now retrieve from tc_UserData */
    unit    = (struct NVMeUnit  *)self->tc_UserData;
    devBase = unit->dev_base;
    IExec   = devBase->IExec;
    parent  = (struct Task *)unit->io_wait_task; /* temporarily stores parent */

    /* Allocate message port */
    unit->io_port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_DONE);
    if (!unit->io_port) {
        IExec->DebugPrintF("[nvme.device:unit_task] Failed to create message port\n");
        unit->task = NULL;
        IExec->Signal(parent, SIGBREAKF_CTRL_C);
        return;
    }
    unit->io_port_mask = 1UL << unit->io_port->mp_SigBit;

    /* Allocate persistent ISR signal bit */
    LONG sig = IExec->AllocSignal(-1);
    if (sig < 0) {
        IExec->DebugPrintF("[nvme.device:unit_task] Failed to allocate signal bit\n");
        IExec->FreeSysObject(ASOT_PORT, unit->io_port);
        unit->io_port = NULL;
        unit->task = NULL;
        IExec->Signal(parent, SIGBREAKF_CTRL_C);
        return;
    }
    unit->io_signal_mask = 1UL << sig;
    unit->io_wait_task   = self; /* ISR will signal self */

    /* Signal parent that we're ready */
    IExec->Signal(parent, SIGBREAKF_CTRL_C);

    /* Main event loop */
    while (!unit->task_shutdown) {
        ULONG signals = IExec->Wait(unit->io_port_mask |
                                    unit->io_signal_mask |
                                    SIGBREAKF_CTRL_C);

        if (signals & SIGBREAKF_CTRL_C)
            break;

        /* Harvest completions first (reduces latency) */
        if (signals & unit->io_signal_mask)
            NVMeIO_Harvest(devBase, unit);

        /* Dispatch pending I/O requests */
        if (signals & unit->io_port_mask) {
            struct IOStdReq *req;
            while ((req = (struct IOStdReq *)IExec->GetMsg(unit->io_port)) != NULL) {
                dispatch_ioreq(devBase, unit, req);
            }
            /* Harvest again after submitting to catch fast completions */
            NVMeIO_Harvest(devBase, unit);
        }
    }

    /* Drain any pending I/O with abort */
    struct IOStdReq *req;
    while ((req = (struct IOStdReq *)IExec->GetMsg(unit->io_port)) != NULL) {
        req->io_Error = IOERR_ABORTED;
        IExec->ReplyMsg((struct Message *)req);
    }

    IExec->FreeSignal(sig);
    IExec->FreeSysObject(ASOT_PORT, unit->io_port);
    unit->io_port        = NULL;
    unit->io_wait_task   = NULL;
    unit->io_signal_mask = 0;
    unit->task           = NULL;
}

/* ------------------------------------------------------------------ */
/* UnitTask_Start / UnitTask_Shutdown                                  */
/* ------------------------------------------------------------------ */

BOOL UnitTask_Start(struct NVMeBase *devBase, struct NVMeUnit *unit)
{
    struct ExecIFace *IExec = devBase->IExec;

    /* Allocate pre-pinned bounce buffers and PRP list pages */
    for (int i = 0; i < NVME_MAX_INFLIGHT; i++) {
        unit->bounce_bufs[i] = IExec->AllocVecTags(NVME_BOUNCE_SIZE,
            AVT_Type,      MEMF_SHARED,
            AVT_Alignment, devBase->page_size,
            AVT_Clear,     0, TAG_DONE);
        if (!unit->bounce_bufs[i]) goto fail;

        ULONG entries = IExec->StartDMA(unit->bounce_bufs[i], NVME_BOUNCE_SIZE, DMA_ReadFromRAM);
        struct DMAEntry *dma = IExec->AllocSysObjectTags(ASOT_DMAENTRY,
            ASODMAE_NumEntries, entries, TAG_DONE);
        if (!dma) goto fail;
        IExec->GetDMAList(unit->bounce_bufs[i], NVME_BOUNCE_SIZE, DMA_ReadFromRAM, dma);
        unit->bounce_phys[i]        = (ULONG)dma[0].PhysicalAddress;
        unit->bounce_dma_entries[i] = entries;
        IExec->FreeSysObject(ASOT_DMAENTRY, dma);

        unit->prp_list_pages[i] = IExec->AllocVecTags(devBase->page_size,
            AVT_Type,      MEMF_SHARED,
            AVT_Alignment, devBase->page_size,
            AVT_Clear,     0, TAG_DONE);
        if (!unit->prp_list_pages[i]) goto fail;

        ULONG prp_entries = IExec->StartDMA(unit->prp_list_pages[i], devBase->page_size, DMA_ReadFromRAM);
        struct DMAEntry *prp_dma = IExec->AllocSysObjectTags(ASOT_DMAENTRY,
            ASODMAE_NumEntries, prp_entries, TAG_DONE);
        if (!prp_dma) goto fail;
        IExec->GetDMAList(unit->prp_list_pages[i], devBase->page_size, DMA_ReadFromRAM, prp_dma);
        unit->prp_list_phys[i] = (ULONG)prp_dma[0].PhysicalAddress;
        IExec->FreeSysObject(ASOT_DMAENTRY, prp_dma);
    }

    /* Store parent task pointer temporarily in io_wait_task for handshake */
    unit->io_wait_task   = IExec->FindTask(NULL);
    unit->task_shutdown  = FALSE;

    /* Create the unit task */
    struct Task *t = IExec->AllocSysObjectTags(ASOT_TASK,
        ASOTASK_FuncEntry, unit_task_entry,
        ASOTASK_Name,      "nvme.device unit task",
        ASOTASK_Priority,  0,
        ASOTASK_UserData,  unit,
        TAG_DONE);
    if (!t) goto fail;

    unit->task = t;

    /* Wait for task to signal us when ready */
    IExec->Wait(SIGBREAKF_CTRL_C);

    if (!unit->io_port) {
        /* Task failed to initialise */
        unit->task = NULL;
        return FALSE;
    }

    return TRUE;

fail:
    /* Free any bounce/prp buffers already allocated */
    for (int i = 0; i < NVME_MAX_INFLIGHT; i++) {
        if (unit->bounce_bufs[i]) {
            IExec->EndDMA(unit->bounce_bufs[i], NVME_BOUNCE_SIZE, DMA_ReadFromRAM);
            IExec->FreeVec(unit->bounce_bufs[i]);
            unit->bounce_bufs[i] = NULL;
        }
        if (unit->prp_list_pages[i]) {
            IExec->EndDMA(unit->prp_list_pages[i], devBase->page_size, DMA_ReadFromRAM);
            IExec->FreeVec(unit->prp_list_pages[i]);
            unit->prp_list_pages[i] = NULL;
        }
    }
    return FALSE;
}

void UnitTask_Shutdown(struct NVMeBase *devBase, struct NVMeUnit *unit)
{
    struct ExecIFace *IExec = devBase->IExec;

    if (!unit->task) return;

    unit->task_shutdown = TRUE;
    IExec->Signal(unit->task, SIGBREAKF_CTRL_C);

    /* Busy-wait for task to clear its task pointer (rare — only at close) */
    while (unit->task != NULL)
        IExec->Delay(1);

    /* Free pre-pinned buffers */
    for (int i = 0; i < NVME_MAX_INFLIGHT; i++) {
        if (unit->bounce_bufs[i]) {
            IExec->EndDMA(unit->bounce_bufs[i], NVME_BOUNCE_SIZE, DMA_ReadFromRAM);
            IExec->FreeVec(unit->bounce_bufs[i]);
            unit->bounce_bufs[i] = NULL;
        }
        if (unit->prp_list_pages[i]) {
            IExec->EndDMA(unit->prp_list_pages[i], devBase->page_size, DMA_ReadFromRAM);
            IExec->FreeVec(unit->prp_list_pages[i]);
            unit->prp_list_pages[i] = NULL;
        }
    }
}
