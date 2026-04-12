/*
 * Close.c — driver-manager Close vector (_manager_Close).
 *
 * Decrements the unit open count.  On the LAST close of a unit, any
 * held TD_ADDCHANGEINT / TD_REMOVE requests are replied (they were
 * deliberately never completed during open, per trackdisk.device
 * convention for fixed media), and the unit's I/O task is shut down.
 *
 * If the whole device has reached OpenCnt==0 AND LIBF_DELEXP is set
 * (indicating a deferred Expunge request), we invoke Expunge directly
 * so exec can free the library segment list.
 */

#include "nvme_device.h"
#include "unit_task.h"
#include <exec/exec.h>

BPTR _manager_Expunge(struct DeviceManagerInterface *Self);

BPTR _manager_Close(struct DeviceManagerInterface *Self, struct IOStdReq *ioreq)
{
    struct NVMeBase *devBase = (struct NVMeBase *)Self->Data.LibBase;
    struct NVMeUnit *unit    = (struct NVMeUnit *)ioreq->io_Unit;
    BPTR seglist             = (BPTR)NULL;

    DLOG(devBase->IExec, "[nvme.device:Close] ENTER unit=%p open_count=%lu lib_OpenCnt=%u\n",
         unit, unit ? (ULONG)unit->open_count : 0xDEADBEEFUL,
         devBase->dev_Base.dd_Library.lib_OpenCnt);

    if (unit && unit != (struct NVMeUnit *)-1) {
        if (unit->open_count > 0) {
            unit->open_count--;
            if (unit->open_count == 0) {
                /* Reply to any held notification requests before shutdown */
                if (unit->changeint_req) {
                    unit->changeint_req->io_Error = 0;
                    devBase->IExec->ReplyMsg((struct Message *)unit->changeint_req);
                    unit->changeint_req = NULL;
                }
                if (unit->remove_req) {
                    unit->remove_req->io_Error = 0;
                    devBase->IExec->ReplyMsg((struct Message *)unit->remove_req);
                    unit->remove_req = NULL;
                }
                UnitTask_Shutdown(devBase, unit);
            }
        }
    }

    ioreq->io_Unit   = (struct Unit *)-1;
    ioreq->io_Device = (struct Device *)-1;

    devBase->dev_Base.dd_Library.lib_OpenCnt--;

    if (devBase->dev_Base.dd_Library.lib_OpenCnt == 0) {
        if (devBase->dev_Base.dd_Library.lib_Flags & LIBF_DELEXP) {
            DLOG(devBase->IExec, "[nvme.device:Close] LIBF_DELEXP set —"
                                 " calling Expunge from Close\n");
            seglist = _manager_Expunge(Self);
        }
    }

    DLOG(devBase->IExec, "[nvme.device:Close] EXIT seglist=%p"
                         " lib_OpenCnt=%u\n",
         (APTR)seglist, devBase->dev_Base.dd_Library.lib_OpenCnt);
    return seglist;
}
