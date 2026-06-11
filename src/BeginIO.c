/*
 * BeginIO.c — driver-manager BeginIO and AbortIO vectors.
 *
 * Three kinds of command are dispatched here:
 *
 *   Inline-reply commands
 *       Fast trackdisk no-ops and queries (MOTOR, SEEK, EJECT,
 *       CHANGENUM, CHANGESTATE, GETDRIVETYPE, NSCMD_DEVICEQUERY, …).
 *       Filled in immediately and replied (or left IOF_QUICK) without
 *       ever touching the unit task.
 *
 *   Held commands
 *       TD_ADDCHANGEINT and TD_REMOVE stash the IORequest on the unit
 *       (one slot each) and are NOT replied.  They're released in
 *       Close() or by a matching TD_REMCHANGEINT; fixed-media
 *       semantics require that they stay pending forever otherwise.
 *
 *   Async I/O commands
 *       CMD_READ/WRITE/UPDATE, their TD_*64 and NSCMD_* variants, and
 *       HD_SCSICMD — all PutMsg'd to unit->io_port and handled by the
 *       unit task so BeginIO never blocks on actual I/O.
 *
 * AbortIO finds a queued IORequest and removes it before it's picked
 * up; once a request is committed to the NVMe SQ it cannot be aborted
 * and the function returns a nonzero error.
 */

#include "nvme_device.h"
#include "nvme_cmds.h"
#include "nvme_stats.h"
#include <devices/newstyle.h>
#include <devices/scsidisk.h>
#include <exec/errors.h>

/* NSD supported command list */
static const UWORD nvme_supported_cmds[] = {
    CMD_READ, CMD_WRITE, CMD_UPDATE, CMD_CLEAR, CMD_FLUSH,
    CMD_START, CMD_STOP,
    TD_MOTOR, TD_SEEK, TD_EJECT, TD_FORMAT,
    TD_GETGEOMETRY, TD_GETNUMTRACKS, TD_GETDRIVETYPE,
    TD_ADDCHANGEINT, TD_REMCHANGEINT, TD_REMOVE,
    TD_CHANGENUM, TD_CHANGESTATE, TD_PROTSTATUS,
    TD_READ64, TD_WRITE64, TD_SEEK64, TD_FORMAT64,
    HD_SCSICMD,
    ETD_READ, ETD_WRITE, ETD_UPDATE, ETD_CLEAR, ETD_MOTOR,
    ETD_SEEK, ETD_FORMAT,
    NSCMD_DEVICEQUERY,
    NSCMD_TD_READ64, NSCMD_TD_WRITE64,
    NSCMD_TD_SEEK64, NSCMD_TD_FORMAT64,
    NSCMD_TD_GETGEOMETRY64,
    NSCMD_ETD_READ64, NSCMD_ETD_WRITE64,
    NSCMD_ETD_SEEK64, NSCMD_ETD_FORMAT64,
    /* Custom stats interface — 0xA006/0xA007 are stubbed for now, the
     * one currently useful command is GETSTATS. */
    NSCMD_NVME_GETSTATS,
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

    DPRINTF(IExec, "[nvme.device:BeginIO] Cmd %u len=%lu off=%lu\n",
            (ULONG)ioreq->io_Command,
            (ULONG)ioreq->io_Length, (ULONG)ioreq->io_Offset);

    switch (ioreq->io_Command) {

    /* ---- Held change-notification commands ---- */

    case TD_ADDCHANGEINT:
        /* NVMe fixed media — hold the request; never fire a change
         * notification.  Without a unit there is nowhere to hold it:
         * reply with an error rather than orphaning the caller. */
        if (!unit) {
            ioreq->io_Error = IOERR_OPENFAIL;
            inline_reply(IExec, ioreq);
            return;
        }
        /* Only one slot: release any previously held request so its
         * owner isn't orphaned by a silent overwrite. */
        if (unit->changeint_req) {
            unit->changeint_req->io_Error = IOERR_ABORTED;
            IExec->ReplyMsg((struct Message *)unit->changeint_req);
        }
        unit->changeint_req = ioreq;
        ioreq->io_Flags &= ~IOF_QUICK;
        return; /* Do NOT reply */

    case TD_REMOVE:
        if (!unit) {
            ioreq->io_Error = IOERR_OPENFAIL;
            inline_reply(IExec, ioreq);
            return;
        }
        if (unit->remove_req) {
            unit->remove_req->io_Error = IOERR_ABORTED;
            IExec->ReplyMsg((struct Message *)unit->remove_req);
        }
        unit->remove_req = ioreq;
        ioreq->io_Flags &= ~IOF_QUICK;
        return; /* Do NOT reply — replied on close/expunge */

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
    case ETD_MOTOR:
        DPRINTF(IExec, "[nvme.device:BeginIO] MOTOR/START/STOP (cmd %u): motor=on\n",
                (ULONG)ioreq->io_Command);
        ioreq->io_Actual = 1; /* motor always on */
        ioreq->io_Error  = 0;
        inline_reply(IExec, ioreq);
        return;

    case TD_SEEK:
    case TD_SEEK64:
    case NSCMD_TD_SEEK64:
    case ETD_SEEK:
        DPRINTF(IExec, "[nvme.device:BeginIO] no-op: SEEK (cmd %u)\n",
                (ULONG)ioreq->io_Command);
        ioreq->io_Error = 0;
        inline_reply(IExec, ioreq);
        return;

    case TD_EJECT:
        DPRINTF(IExec, "[nvme.device:BeginIO] no-op: TD_EJECT\n");
        ioreq->io_Error = 0;
        inline_reply(IExec, ioreq);
        return;

    case CMD_CLEAR:
    case ETD_CLEAR:
        DPRINTF(IExec, "[nvme.device:BeginIO] no-op: CMD_CLEAR (cmd %u)\n",
                (ULONG)ioreq->io_Command);
        ioreq->io_Error = 0;
        inline_reply(IExec, ioreq);
        return;

    case TD_CHANGENUM:
        DPRINTF(IExec, "[nvme.device:BeginIO] TD_CHANGENUM: 0 (fixed media)\n");
        ioreq->io_Actual = 0; /* no disk changes ever */
        ioreq->io_Error  = 0;
        inline_reply(IExec, ioreq);
        return;

    case TD_CHANGESTATE:
        DPRINTF(IExec, "[nvme.device:BeginIO] TD_CHANGESTATE: 0 (disk present)\n");
        ioreq->io_Actual = 0; /* 0 = disk present */
        ioreq->io_Error  = 0;
        inline_reply(IExec, ioreq);
        return;

    case TD_PROTSTATUS:
        DPRINTF(IExec, "[nvme.device:BeginIO] TD_PROTSTATUS: 0 (read-write)\n");
        ioreq->io_Actual = 0; /* 0 = not write-protected */
        ioreq->io_Error  = 0;
        inline_reply(IExec, ioreq);
        return;

    case TD_GETDRIVETYPE:
        DPRINTF(IExec, "[nvme.device:BeginIO] TD_GETDRIVETYPE: DRIVE_NEWSTYLE\n");
        ioreq->io_Actual = DRIVE_NEWSTYLE; /* 0x4E535459 'NSTY' */
        ioreq->io_Error  = 0;
        inline_reply(IExec, ioreq);
        return;

    case TD_GETNUMTRACKS: {
        /* Return cylinder count; zero means "media removed" to mounter */
        ULONG tracks = 32768; /* safe fallback */
        if (unit && unit->total_blocks > 0) {
            ULONG cyl_sectors = 4 * 16; /* heads * sectors_per_track */
            ULONG total32 = (unit->total_blocks > 0xFFFFFFFFULL)
                            ? 0xFFFFFFFF : (ULONG)unit->total_blocks;
            tracks = total32 / cyl_sectors;
            if (tracks == 0) tracks = 1;
        }
        DPRINTF(IExec, "[nvme.device:BeginIO] TD_GETNUMTRACKS: %lu\n", tracks);
        ioreq->io_Actual = tracks;
        ioreq->io_Error  = 0;
        inline_reply(IExec, ioreq);
        return;
    }

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
    case CMD_FLUSH:
    case ETD_READ:
    case ETD_WRITE:
    case ETD_UPDATE:
    case TD_FORMAT:
    case ETD_FORMAT:
    case TD_FORMAT64:
    case NSCMD_TD_FORMAT64:
    case TD_GETGEOMETRY:
    case TD_READ64:
    case TD_WRITE64:
    case NSCMD_TD_READ64:
    case NSCMD_TD_WRITE64:
    case NSCMD_ETD_READ64:
    case NSCMD_ETD_WRITE64:
    case NSCMD_ETD_SEEK64:
    case NSCMD_ETD_FORMAT64:
    case NSCMD_TD_GETGEOMETRY64:
    case HD_SCSICMD:
        if (!unit || !unit->io_port) {
            ioreq->io_Error = IOERR_OPENFAIL;
            inline_reply(IExec, ioreq);
            return;
        }
        ioreq->io_Flags &= ~IOF_QUICK; /* caller must not touch ioreq until ReplyMsg */
        IExec->PutMsg(unit->io_port, (struct Message *)ioreq);
        return;

    case NSCMD_NVME_GETSTATS:
        /* Synchronous stats snapshot — safe to service inline because
         * the snapshot copy is O(1) and doesn't touch any queues. */
        if (!unit) {
            ioreq->io_Error = IOERR_OPENFAIL;
            inline_reply(IExec, ioreq);
            return;
        }
        NVMe_HandleGetStats(libBase, unit, ioreq);
        inline_reply(IExec, ioreq);
        return;

    case NSCMD_TD_ADDSTATCALLBACK:
    case NSCMD_TD_REMSTATCALLBACK:
        /* Callback mechanism is declared for wire-compatibility with
         * usb2; periodic-tick firing is not yet wired.  Consumers
         * relying on live updates should poll NSCMD_NVME_GETSTATS. */
        ioreq->io_Error = IOERR_NOCMD;
        inline_reply(IExec, ioreq);
        return;

    default:
        DPRINTF(IExec, "[nvme.device:BeginIO] UNKNOWN COMMAND: %lu. Returning IOERR_NOCMD.\n",
                (ULONG)ioreq->io_Command);
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
