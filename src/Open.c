/*
 * Open.c — driver-manager Open vector (_manager_Open).
 *
 * Called by OpenDevice() for every unit open.  On the FIRST open of a
 * given unit it lazily spawns the unit's async I/O task (bringing up
 * DMA bounces, PRP pages, and the message port).  On subsequent opens
 * it just bumps the unit's open_count.
 *
 * Always clears LIBF_DELEXP — if Expunge was deferred while the device
 * was fully closed, a new open must re-claim it.
 *
 * On failure the IORequest is poisoned (io_Unit = -1, io_Device = -1)
 * per exec convention so the caller detects the error via OpenDevice's
 * return code.
 */

#include "nvme_device.h"
#include "unit_task.h"
#include <exec/errors.h>
#include <exec/exec.h>

struct NVMeBase *_manager_Open(struct DeviceManagerInterface *Self, struct IOStdReq *ioreq,
                               ULONG unitNum, ULONG flags)
{
    struct NVMeBase *devBase = (struct NVMeBase *)Self->Data.LibBase;
    struct NVMeUnit *unit;

    devBase->dev_Base.dd_Library.lib_OpenCnt++;

#ifdef DEBUG
    {
        struct Task *caller = devBase->IExec->FindTask(NULL);
        DPRINTF(devBase->IExec,
                "[nvme.device:Open] Open unit %lu requested by task '%s' (flags=%lu)\n",
                unitNum,
                caller && caller->tc_Node.ln_Name ? caller->tc_Node.ln_Name : "(null)",
                flags);
    }
#endif

    if (unitNum >= devBase->num_global_units) {
        ioreq->io_Error = IOERR_OPENFAIL;
        goto bailout;
    }

    unit = devBase->global_units[unitNum];
    if (unit == NULL) {
        ioreq->io_Error = IOERR_OPENFAIL;
        goto bailout;
    }

    /* Start unit task on first open */
    if (unit->open_count == 0) {
        if (!UnitTask_Start(devBase, unit)) {
            DPRINTF(devBase->IExec, "[nvme.device:Open] UnitTask_Start failed for unit %lu\n", unitNum);
            ioreq->io_Error = IOERR_OPENFAIL;
            goto bailout;
        }
    }

    unit->open_count++;
    ioreq->io_Unit                        = (struct Unit *)unit;
    ioreq->io_Error                       = 0;
    ioreq->io_Message.mn_Node.ln_Type     = NT_REPLYMSG;

    devBase->dev_Base.dd_Library.lib_Flags &= ~LIBF_DELEXP;
    return devBase;

bailout:
    ioreq->io_Unit   = (struct Unit *)-1;
    ioreq->io_Device = (struct Device *)-1;
    devBase->dev_Base.dd_Library.lib_OpenCnt--;
    return NULL;
}
