#include "nvme_device.h"
#include "nvme_cmds.h"
#include <devices/newstyle.h>
#include <exec/errors.h>

/* NSD supported command list */
static const UWORD nvme_supported_cmds[] = {
    CMD_READ, CMD_WRITE, CMD_UPDATE, CMD_CLEAR,
    CMD_START, CMD_STOP,
    TD_MOTOR, TD_SEEK, TD_EJECT,
    TD_GETGEOMETRY, TD_GETNUMTRACKS, TD_GETDRIVETYPE,
    TD_ADDCHANGEINT, TD_REMCHANGEINT, TD_REMOVE,
    TD_CHANGENUM, TD_CHANGESTATE, TD_PROTSTATUS,
    TD_READ64, TD_WRITE64,
    NSCMD_DEVICEQUERY,
    NSCMD_TD_READ64, NSCMD_TD_WRITE64,
    0
};

/* Helper: inline reply for quick commands */
static void inline_reply(struct ExecIFace *IExec, struct IOStdReq *ioreq)
{
    if (ioreq->io_Flags & IOF_QUICK)
        return;
    IExec->ReplyMsg((struct Message *)ioreq);
}

void _manager_BeginIO(struct DeviceManagerInterface *Self, struct IOStdReq *ioreq)
{
    struct NVMeBase *libBase = (struct NVMeBase *)Self->Data.LibBase;
    struct ExecIFace *IExec  = libBase->IExec;
    struct NVMeUnit  *unit   = (struct NVMeUnit *)ioreq->io_Unit;

    ioreq->io_Error = 0;

    DPRINTF(IExec, "[nvme.device:BeginIO] Cmd %u unit %p\n",
            (ULONG)ioreq->io_Command, unit);

    switch (ioreq->io_Command) {

    /* ---- Held change-notification commands ---- */

    case TD_ADDCHANGEINT:
        /* NVMe fixed media — hold the request; never fire a change notification */
        if (unit)
            unit->changeint_req = ioreq;
        ioreq->io_Flags &= ~IOF_QUICK;
        return; /* Do NOT reply */

    case TD_REMOVE:
        ioreq->io_Flags &= ~IOF_QUICK;
        return; /* Do NOT reply */

    case TD_REMCHANGEINT:
        if (unit && unit->changeint_req) {
            unit->changeint_req->io_Error = 0;
            IExec->ReplyMsg((struct Message *)unit->changeint_req);
            unit->changeint_req = NULL;
        }
        ioreq->io_Error = 0;
        inline_reply(IExec, ioreq);
        return;

    /* ---- Simple inline commands ---- */

    case CMD_START:
    case CMD_STOP:
    case TD_MOTOR:
        ioreq->io_Actual = 1; /* motor always on */
        ioreq->io_Error  = 0;
        inline_reply(IExec, ioreq);
        return;

    case TD_SEEK:
    case TD_EJECT:
    case CMD_CLEAR:
        ioreq->io_Error = 0;
        inline_reply(IExec, ioreq);
        return;

    case TD_CHANGENUM:
        ioreq->io_Actual = 0; /* no disk changes ever */
        ioreq->io_Error  = 0;
        inline_reply(IExec, ioreq);
        return;

    case TD_CHANGESTATE:
        ioreq->io_Actual = 0; /* 0 = disk present */
        ioreq->io_Error  = 0;
        inline_reply(IExec, ioreq);
        return;

    case TD_PROTSTATUS:
        ioreq->io_Actual = 0; /* 0 = not write-protected */
        ioreq->io_Error  = 0;
        inline_reply(IExec, ioreq);
        return;

    case TD_GETDRIVETYPE:
        ioreq->io_Actual = DRIVE_NEWSTYLE; /* 0x44 */
        ioreq->io_Error  = 0;
        inline_reply(IExec, ioreq);
        return;

    case TD_GETNUMTRACKS:
        ioreq->io_Actual = 0;
        ioreq->io_Error  = 0;
        inline_reply(IExec, ioreq);
        return;

    case NSCMD_DEVICEQUERY: {
        struct NSDeviceQueryResult *qr = (struct NSDeviceQueryResult *)ioreq->io_Data;
        if (qr && ioreq->io_Length >= sizeof(struct NSDeviceQueryResult)) {
            qr->DevQueryFormat    = 0;
            qr->SizeAvailable     = sizeof(struct NSDeviceQueryResult);
            qr->DeviceType        = NSDEVTYPE_TRACKDISK;
            qr->DeviceSubType     = 0;
            qr->SupportedCommands = (UWORD *)nvme_supported_cmds;
            ioreq->io_Actual      = sizeof(struct NSDeviceQueryResult);
            ioreq->io_Error       = 0;
        } else {
            ioreq->io_Error = IOERR_BADLENGTH;
        }
        inline_reply(IExec, ioreq);
        return;
    }

    /* ---- Async I/O commands — queued to unit task ---- */

    case CMD_READ:
    case CMD_WRITE:
    case CMD_UPDATE:
    case TD_GETGEOMETRY:
    case TD_READ64:
    case TD_WRITE64:
    case NSCMD_TD_READ64:
    case NSCMD_TD_WRITE64:
    case HD_SCSICMD:
        if (!unit || !unit->io_port) {
            ioreq->io_Error = IOERR_OPENFAIL;
            inline_reply(IExec, ioreq);
            return;
        }
        ioreq->io_Flags &= ~IOF_QUICK; /* caller must not touch ioreq until ReplyMsg */
        IExec->PutMsg(unit->io_port, (struct Message *)ioreq);
        return;

    default:
        ioreq->io_Error = IOERR_NOCMD;
        inline_reply(IExec, ioreq);
        return;
    }
}

LONG _manager_AbortIO(struct DeviceManagerInterface *Self, struct IOStdReq *ioreq)
{
    struct NVMeBase  *libBase = (struct NVMeBase *)Self->Data.LibBase;
    struct ExecIFace *IExec   = libBase->IExec;
    struct NVMeUnit  *unit    = (struct NVMeUnit *)ioreq->io_Unit;

    if (!unit || !unit->io_port)
        return IOERR_NOCMD;

    IExec->Forbid();

    struct Message *msg   = (struct Message *)unit->io_port->mp_MsgList.lh_Head;
    struct Message *found = NULL;
    while (msg->mn_Node.ln_Succ) {
        if (msg == (struct Message *)ioreq) { found = msg; break; }
        msg = (struct Message *)msg->mn_Node.ln_Succ;
    }
    if (found)
        IExec->Remove((struct Node *)found);

    IExec->Permit();

    if (found) {
        ioreq->io_Error = IOERR_ABORTED;
        IExec->ReplyMsg((struct Message *)ioreq);
        return 0;
    }
    return IOERR_NOCMD;
}
