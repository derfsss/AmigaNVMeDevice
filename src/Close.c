#include "nvme_device.h"
#include "unit_task.h"
#include <exec/exec.h>

BPTR _manager_Expunge(struct DeviceManagerInterface *Self);

BPTR _manager_Close(struct DeviceManagerInterface *Self, struct IOStdReq *ioreq)
{
    struct NVMeBase *devBase = (struct NVMeBase *)Self->Data.LibBase;
    struct NVMeUnit *unit    = (struct NVMeUnit *)ioreq->io_Unit;
    BPTR seglist             = (BPTR)NULL;

    DPRINTF(devBase->IExec, "[nvme.device:Close] Close called for unit %p\n", unit);

    if (unit && unit != (struct NVMeUnit *)-1) {
        if (unit->open_count > 0) {
            unit->open_count--;
            if (unit->open_count == 0)
                UnitTask_Shutdown(devBase, unit);
        }
    }

    ioreq->io_Unit   = (struct Unit *)-1;
    ioreq->io_Device = (struct Device *)-1;

    devBase->dev_Base.dd_Library.lib_OpenCnt--;

    if (devBase->dev_Base.dd_Library.lib_OpenCnt == 0) {
        if (devBase->dev_Base.dd_Library.lib_Flags & LIBF_DELEXP)
            seglist = _manager_Expunge(Self);
    }

    return seglist;
}
