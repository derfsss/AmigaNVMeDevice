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

    DPRINTF(devBase->IExec, "[nvme.device:Open] Open unit %lu requested\n", unitNum);

    if (unitNum >= devBase->num_units) {
        ioreq->io_Error = IOERR_OPENFAIL;
        goto bailout;
    }

    unit = devBase->units[unitNum];
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
