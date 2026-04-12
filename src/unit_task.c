/*
 * unit_task.c — per-unit async I/O task, SCSI-command synthesis, and
 * the dispatch of every "real" IORequest that BeginIO cannot finish
 * inline.
 *
 * The task owns:
 *   - unit->io_port         — message port BeginIO PutMsg's to
 *   - unit->inflight[]      — NVME_MAX_INFLIGHT command slots
 *   - bounce_bufs[]/_phys[] — pre-pinned DMA-mapped bounce buffers
 *   - prp_list_pages[]      — one pre-allocated PRP list page per slot
 *
 * Startup uses a one-shot handshake (NVMeTaskStartMsg passed through
 * the new task's tc_UserData) so UnitTask_Start blocks until the task
 * has claimed its resources.  Shutdown raises SIGBREAKF_CTRL_C and
 * busy-waits for the task to clear unit->task.
 *
 * HD_SCSICMD is handled here because NVMe is not SCSI but several
 * Workbench tools (Media Toolbox, HDToolbox, SFSCheck) still expect
 * SCSI responses.  We synthesise INQUIRY (standard + VPD 0x00/0x80/
 * 0x83), READ CAPACITY 10, TEST UNIT READY; anything else returns
 * CHECK CONDITION with ILLEGAL REQUEST sense.
 */

#include "unit_task.h"
#include "nvme_io.h"
#include "nvme_device.h"
#include "nvme_cmds.h"
#include "nvme_scsi.h"
#include <exec/exec.h>
#include <exec/memory.h>
#include <devices/trackdisk.h>
#include <devices/scsidisk.h>
#include <devices/newstyle.h>
#include <libraries/mounter.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Start message — passed via tc_UserData for handshake               */
/* ------------------------------------------------------------------ */

struct NVMeTaskStartMsg {
    struct NVMeBase *devBase;
    struct NVMeUnit *unit;
    struct Task     *parent_task;
    ULONG            ready_mask; /* signal mask to send to parent when ready */
    LONG             ready_bit;  /* signal bit allocated by parent */
};

/* ------------------------------------------------------------------ */
/* Unit task dispatch — handle a single IOStdReq from the message port */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* HD_SCSICMD: synthesize SCSI responses for Media Toolbox et al.     */
/* NVMe is not SCSI, but many AmigaOS tools expect HD_SCSICMD.        */
/* ------------------------------------------------------------------ */

/* Legacy shim — the actual sense filler lives in scsi_cmds/scsi_log_sense.c
 * (shared with the ATA PASS-THROUGH handler). */
static inline void fill_auto_sense(struct SCSICmd *scsiCmd, UBYTE sense_key,
                                   UBYTE asc, UBYTE ascq)
{
    NVMe_SCSI_FillSense(scsiCmd, sense_key, asc, ascq);
}

#define SCSI_SENSE_ILLEGAL_REQUEST NVME_SSK_ILLEGAL_REQUEST

static void handle_scsi_cmd(struct NVMeBase *devBase, struct NVMeUnit *unit,
                            struct IOStdReq *ioreq)
{
    struct ExecIFace *IExec = devBase->IExec;

    if (!ioreq->io_Data || ioreq->io_Length < sizeof(struct SCSICmd)) {
        ioreq->io_Error = IOERR_BADLENGTH;
        return;
    }

    struct SCSICmd *scsiCmd = (struct SCSICmd *)ioreq->io_Data;
    if (!scsiCmd->scsi_Command || scsiCmd->scsi_CmdLength == 0) {
        ioreq->io_Error = HFERR_BadStatus;
        return;
    }

    scsiCmd->scsi_Status     = 0;
    scsiCmd->scsi_CmdActual  = scsiCmd->scsi_CmdLength;
    scsiCmd->scsi_SenseActual = 0;

    UBYTE opcode = scsiCmd->scsi_Command[0];

    DPRINTF(IExec, "[nvme.device:scsi] opcode=0x%02lx len=%lu\n",
            (ULONG)opcode, (ULONG)scsiCmd->scsi_Length);

    switch (opcode) {

    case 0x00: /* TEST UNIT READY */
        scsiCmd->scsi_Status = 0; /* GOOD */
        scsiCmd->scsi_Actual = 0;
        ioreq->io_Error = 0;
        return;

    case 0x12: { /* INQUIRY */
        UBYTE *cdb = scsiCmd->scsi_Command;
        UBYTE evpd = cdb[1] & 1;
        UBYTE page = cdb[2];
        UBYTE *buf = (UBYTE *)scsiCmd->scsi_Data;
        ULONG alloc_len = scsiCmd->scsi_Length;

        if (evpd) {
            /* VPD pages */
            switch (page) {
            case 0x00: { /* Supported VPD pages */
                UBYTE resp[7];
                memset(resp, 0, sizeof(resp));
                resp[0] = 0x00; /* peripheral qualifier + device type (direct access) */
                resp[1] = 0x00; /* page code */
                resp[3] = 3;    /* page length */
                resp[4] = 0x00; /* page 0x00 */
                resp[5] = 0x80; /* page 0x80 */
                resp[6] = 0x83; /* page 0x83 */
                ULONG copy = alloc_len < 7 ? alloc_len : 7;
                memcpy(buf, resp, copy);
                scsiCmd->scsi_Actual = copy;
                ioreq->io_Actual = copy;
                ioreq->io_Error = 0;
                return;
            }
            case 0x80: { /* Unit Serial Number */
                UBYTE resp[24];
                memset(resp, 0, sizeof(resp));
                resp[0] = 0x00; /* device type */
                resp[1] = 0x80; /* page code */
                /* Serial: "NVME-NS<nsid>" */
                char serial[16];
                int slen = 0;
                serial[slen++] = 'N'; serial[slen++] = 'V';
                serial[slen++] = 'M'; serial[slen++] = 'E';
                serial[slen++] = '-'; serial[slen++] = 'N';
                serial[slen++] = 'S';
                serial[slen++] = '0' + (char)(unit->nsid % 10);
                resp[3] = (UBYTE)slen;
                memcpy(&resp[4], serial, slen);
                ULONG total = 4 + slen;
                ULONG copy = alloc_len < total ? alloc_len : total;
                memcpy(buf, resp, copy);
                scsiCmd->scsi_Actual = copy;
                ioreq->io_Actual = copy;
                ioreq->io_Error = 0;
                return;
            }
            case 0x83: { /* Device Identification */
                char id[] = "QEMU    NVMe            NS0";
                id[sizeof(id)-2] = '0' + (char)(unit->nsid % 10);
                ULONG idlen = (ULONG)(sizeof(id) - 1);
                UBYTE resp[48];
                memset(resp, 0, sizeof(resp));
                resp[0] = 0x00;
                resp[1] = 0x83;
                ULONG desig_len = 4 + idlen;
                resp[3] = (UBYTE)desig_len;
                resp[4] = 0x02; /* ASCII */
                resp[5] = 0x01; /* T10 vendor ID */
                resp[6] = 0x00;
                resp[7] = (UBYTE)idlen;
                memcpy(&resp[8], id, idlen);
                ULONG total = 4 + desig_len;
                ULONG copy = alloc_len < total ? alloc_len : total;
                memcpy(buf, resp, copy);
                scsiCmd->scsi_Actual = copy;
                ioreq->io_Actual = copy;
                ioreq->io_Error = 0;
                return;
            }
            default:
                scsiCmd->scsi_Status = 2; /* CHECK CONDITION */
                fill_auto_sense(scsiCmd, SCSI_SENSE_ILLEGAL_REQUEST, 0x24, 0x00);
                ioreq->io_Error = HFERR_BadStatus;
                return;
            }
        }

        /* Standard INQUIRY — synthesize from NVMe identify data */
        UBYTE resp[36];
        memset(resp, 0, sizeof(resp));
        resp[0] = 0x00;  /* peripheral: direct access block device */
        resp[1] = 0x00;  /* not removable */
        resp[2] = 0x05;  /* SPC-3 */
        resp[3] = 0x02;  /* response data format */
        resp[4] = 31;    /* additional length */
        /* Vendor (bytes 8-15) */
        memcpy(&resp[8],  "QEMU    ", 8);
        /* Product (bytes 16-31) */
        memcpy(&resp[16], "NVMe Disk       ", 16);
        /* Revision (bytes 32-35) */
        memcpy(&resp[32], "1.0 ", 4);

        ULONG copy = alloc_len < 36 ? alloc_len : 36;
        memcpy(buf, resp, copy);
        scsiCmd->scsi_Actual = copy;
        ioreq->io_Actual = copy;
        ioreq->io_Error = 0;
        return;
    }

    case 0x4D:  /* LOG SENSE */
        NVMe_SCSI_HandleLogSense(devBase, unit, ioreq);
        return;

    case 0x85:  /* ATA PASS-THROUGH (16) */
    case 0xA1:  /* ATA PASS-THROUGH (12) */
        NVMe_SCSI_HandleATAPassthrough(devBase, unit, ioreq);
        return;

    case 0x25: { /* READ CAPACITY (10) */
        if (scsiCmd->scsi_Length < 8) {
            scsiCmd->scsi_Status = 2;
            ioreq->io_Error = HFERR_BadStatus;
            return;
        }
        UBYTE *buf = (UBYTE *)scsiCmd->scsi_Data;
        /* Last LBA (big-endian uint32).
         * Per SCSI spec, return 0xFFFFFFFF for disks > 2TB to signal
         * that READ CAPACITY(16) should be used instead. */
        ULONG last_lba = (unit->total_blocks > 0xFFFFFFFFULL)
                         ? 0xFFFFFFFF : (ULONG)(unit->total_blocks - 1);
        buf[0] = (UBYTE)(last_lba >> 24);
        buf[1] = (UBYTE)(last_lba >> 16);
        buf[2] = (UBYTE)(last_lba >> 8);
        buf[3] = (UBYTE)(last_lba);
        /* Block size (big-endian uint32) */
        buf[4] = (UBYTE)(unit->block_size >> 24);
        buf[5] = (UBYTE)(unit->block_size >> 16);
        buf[6] = (UBYTE)(unit->block_size >> 8);
        buf[7] = (UBYTE)(unit->block_size);
        scsiCmd->scsi_Actual = 8;
        ioreq->io_Actual = 8;
        ioreq->io_Error = 0;
        return;
    }

    default:
        /* Unsupported SCSI opcode — CHECK CONDITION with auto-sense */
        DPRINTF(IExec, "[nvme.device:scsi] Unsupported opcode 0x%02lx\n",
                (ULONG)opcode);
        scsiCmd->scsi_Status = 2; /* CHECK CONDITION */
        fill_auto_sense(scsiCmd, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
        ioreq->io_Error = HFERR_BadStatus;
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Read/Write dispatch with MDTS chunking                              */
/*                                                                      */
/* If the transfer exceeds the controller's max_transfer_bytes (MDTS), */
/* split into multiple NVMe commands. Each chunk is submitted async     */
/* and then poll-harvested before sending the next. The original        */
/* IOStdReq is replied once all chunks complete (or on first error).    */
/* ------------------------------------------------------------------ */

static void dispatch_rw(struct NVMeBase *devBase, struct NVMeUnit *unit,
                        struct IOStdReq *ioreq, BOOL is_write,
                        struct ExecIFace *IExec)
{
    ULONG total_len = ioreq->io_Length;
    ULONG max_chunk = unit->ctrl->max_transfer_bytes;

    /* Fast path: transfer fits in one NVMe command.  Use the no-ring
     * primitive so the event loop can batch a burst of submissions
     * behind a single doorbell write. */
    if (total_len <= max_chunk) {
        LONG rc = NVMeIO_SubmitNoRing(devBase, unit, ioreq, is_write);
        if (rc == 0) return; /* async — Harvest will ReplyMsg */
        if (rc == -1) {
            ioreq->io_Error = IOERR_UNITBUSY;
            unit->stats.unitbusy_hits++;
        }
        IExec->ReplyMsg((struct Message *)ioreq);
        return;
    }

    /* Transfer exceeds MDTS — will need chunking. */
    unit->stats.mdts_splits++;

    /* Chunked path: split into max_chunk-sized pieces.
     * We temporarily modify the IOStdReq fields for each chunk,
     * then restore them before replying. */
    ULONG orig_offset = ioreq->io_Offset;
    ULONG orig_actual = ioreq->io_Actual; /* high 32 of 64-bit offset, or 0 */
    APTR  orig_data   = ioreq->io_Data;
    ULONG orig_length = ioreq->io_Length;

    ULONG done = 0;
    ioreq->io_Error = 0;

    DPRINTF(IExec, "[nvme.device:dispatch] Chunking %s: %lu bytes into %lu-byte chunks\n",
            is_write ? "WRITE" : "READ", total_len, max_chunk);

    while (done < total_len) {
        ULONG chunk = total_len - done;
        if (chunk > max_chunk)
            chunk = max_chunk;

        /* Compute 64-bit byte offset for this chunk */
        uint64 base_off = ((uint64)orig_actual << 32) | (uint64)orig_offset;
        uint64 chunk_off = base_off + done;

        /* Set up IOStdReq for this chunk */
        ioreq->io_Offset = (ULONG)(chunk_off & 0xFFFFFFFF);
        ioreq->io_Actual = (ULONG)(chunk_off >> 32);
        ioreq->io_Data   = (APTR)((UBYTE *)orig_data + done);
        ioreq->io_Length = chunk;

        LONG rc = NVMeIO_Submit(devBase, unit, ioreq, is_write);
        if (rc != 0) {
            /* Submit failed — no inflight to harvest */
            if (rc == -1) {
                ioreq->io_Error = IOERR_UNITBUSY;
                unit->stats.unitbusy_hits++;
            }
            break;
        }

        /* Poll-harvest until this chunk completes.
         * Submit sets inflight[slot].ioreq = ioreq;
         * Harvest clears it and calls ReplyMsg.
         * But we DON'T want ReplyMsg yet — we want to continue chunking.
         *
         * Trick: temporarily NULL the mn_ReplyPort so ReplyMsg is a no-op,
         * then restore it after. Actually, simpler: we'll check if
         * inflight slot is free (ioreq cleared) as our completion signal.
         * The issue is Harvest calls ReplyMsg... we need to prevent that.
         *
         * Better approach: find which slot has our ioreq and poll until
         * it's cleared. We intercept the reply by using a flag. */

        /* Find which slot has this ioreq */
        LONG slot = -1;
        for (int i = 0; i < NVME_MAX_INFLIGHT; i++) {
            if (unit->inflight[i].ioreq == ioreq) {
                slot = i;
                break;
            }
        }

        if (slot < 0) {
            /* Should not happen — Submit returned 0 */
            ioreq->io_Error = IOERR_ABORTED;
            break;
        }

        /* Redirect reply to our own port so ReplyMsg doesn't go back
         * to the caller yet. We'll consume it here. */
        struct MsgPort *real_port = ioreq->io_Message.mn_ReplyPort;
        ioreq->io_Message.mn_ReplyPort = unit->io_port;

        /* Spin-poll until Harvest consumes this slot.
         * Unmask INTMC each iteration in case the ISR masked INTMS
         * during the poll — without this, subsequent chunks may
         * never complete if the ISR swallowed the interrupt. */
        for (LONG poll = 0; poll < 5000000; poll++) {
            NVMeIO_Harvest(devBase, unit);
            if (unit->inflight[slot].ioreq == NULL)
                break;
            if (unit->ctrl->irq_installed)
                nvme_w32(unit->ctrl->iobase + NVME_REG_INTMC, 1);
        }

        /* Consume the replied message from our own port */
        if (unit->inflight[slot].ioreq == NULL) {
            IExec->GetMsg(unit->io_port); /* discard — it's our ioreq */
        }

        /* Restore the reply port */
        ioreq->io_Message.mn_ReplyPort = real_port;

        if (unit->inflight[slot].ioreq != NULL) {
            /* Timeout — command stuck */
            unit->inflight[slot].ioreq = NULL;
            ioreq->io_Error = IOERR_ABORTED;
            break;
        }

        if (ioreq->io_Error != 0)
            break; /* NVMe error on this chunk */

        done += chunk;
    }

    /* Restore original IOStdReq fields */
    ioreq->io_Offset = orig_offset;
    ioreq->io_Actual = (ioreq->io_Error == 0) ? orig_length : 0;
    ioreq->io_Data   = orig_data;
    ioreq->io_Length = orig_length;

    IExec->ReplyMsg((struct Message *)ioreq);
}

/* ------------------------------------------------------------------ */
/* Unit task dispatch — handle a single IOStdReq from the message port */
/* ------------------------------------------------------------------ */

static void dispatch_ioreq(struct NVMeBase *devBase, struct NVMeUnit *unit,
                             struct IOStdReq *ioreq)
{
    struct ExecIFace *IExec = devBase->IExec;

    DPRINTF(IExec, "[nvme.device:dispatch] cmd=%u offset=%lu len=%lu data=0x%08lx\n",
            (ULONG)ioreq->io_Command, (ULONG)ioreq->io_Offset,
            (ULONG)ioreq->io_Length, (ULONG)ioreq->io_Data);

    switch (ioreq->io_Command) {

    case CMD_READ:
    case TD_READ64:
    case NSCMD_TD_READ64:
    case NSCMD_ETD_READ64:
    case ETD_READ: {
        dispatch_rw(devBase, unit, ioreq, FALSE, IExec);
        return;
    }

    case CMD_WRITE:
    case TD_WRITE64:
    case NSCMD_TD_WRITE64:
    case NSCMD_ETD_WRITE64:
    case ETD_WRITE:
    case TD_FORMAT:
    case ETD_FORMAT:
    case TD_FORMAT64:
    case NSCMD_TD_FORMAT64:
    case NSCMD_ETD_FORMAT64: {
        dispatch_rw(devBase, unit, ioreq, TRUE, IExec);
        return;
    }

    case CMD_UPDATE:
    case CMD_FLUSH:
    case ETD_UPDATE: {
        LONG rc = NVMeIO_Flush(devBase, unit, ioreq);
        if (rc == 0) return;
        if (rc == -1) {
            ioreq->io_Error = IOERR_UNITBUSY;
            unit->stats.unitbusy_hits++;
        }
        IExec->ReplyMsg((struct Message *)ioreq);
        return;
    }

    case TD_GETGEOMETRY: {
        struct DriveGeometry *dg = (struct DriveGeometry *)ioreq->io_Data;
        if (dg && ioreq->io_Length >= sizeof(struct DriveGeometry)) {
            ULONG sectors_per_track = 16;
            ULONG heads = 4;
            ULONG cyl_sectors = heads * sectors_per_track;
            ULONG total32 = (unit->total_blocks > 0xFFFFFFFFULL)
                            ? 0xFFFFFFFF : (ULONG)unit->total_blocks;
            ULONG cylinders = total32 / cyl_sectors;
            if (cylinders == 0) cylinders = 1;

            dg->dg_SectorSize   = unit->block_size;
            dg->dg_TotalSectors = total32;
            dg->dg_Cylinders    = cylinders;
            dg->dg_CylSectors   = cyl_sectors;
            dg->dg_Heads        = heads;
            dg->dg_TrackSectors = sectors_per_track;
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

    case NSCMD_TD_GETGEOMETRY64: {
        struct DriveGeometry64 *dg64 = (struct DriveGeometry64 *)ioreq->io_Data;
        if (dg64 && ioreq->io_Length >= sizeof(struct DriveGeometry64)) {
            dg64->dg_SectorSize   = unit->block_size;
            dg64->dg_Reserved1    = 0;
            dg64->dg_TotalSectors = unit->total_blocks;
            dg64->dg_BufMemTags   = NULL;
            dg64->dg_DeviceType   = DG_DIRECT_ACCESS;
            dg64->dg_Flags        = 0;
            dg64->dg_Reserved2    = 0;
            ioreq->io_Actual      = sizeof(struct DriveGeometry64);
            ioreq->io_Error       = 0;

            DPRINTF(IExec, "[nvme.device:dispatch] GETGEOMETRY64: SectorSize=%lu TotalSectors=%lu\n",
                    dg64->dg_SectorSize, (ULONG)dg64->dg_TotalSectors);
        } else {
            ioreq->io_Error = IOERR_BADLENGTH;
        }
        IExec->ReplyMsg((struct Message *)ioreq);
        return;
    }

    case NSCMD_ETD_SEEK64:
        /* NSD no-op for fixed media */
        ioreq->io_Error = 0;
        IExec->ReplyMsg((struct Message *)ioreq);
        return;

    case HD_SCSICMD:
        handle_scsi_cmd(devBase, unit, ioreq);
        IExec->ReplyMsg((struct Message *)ioreq);
        return;

    default:
        DPRINTF(IExec, "[nvme.device:dispatch] UNSUPPORTED cmd=%u — IOERR_NOCMD\n",
                (ULONG)ioreq->io_Command);
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
    /* Retrieve IExec from the system base (valid in any context) */
    struct ExecIFace *IExec =
        (struct ExecIFace *)((struct ExecBase *)*((ULONG *)4))->MainInterface;

    struct Task *self = IExec->FindTask(NULL);

    /* Retrieve start message passed via tc_UserData */
    struct NVMeTaskStartMsg *startMsg =
        (struct NVMeTaskStartMsg *)self->tc_UserData;

    struct NVMeBase *devBase   = startMsg->devBase;
    struct NVMeUnit *unit      = startMsg->unit;
    struct Task     *parent    = startMsg->parent_task;
    ULONG            ready_mask = startMsg->ready_mask;

    /* Allocate message port */
    unit->io_port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_DONE);
    if (!unit->io_port) {
        DPRINTF(IExec, "[nvme.device:unit_task] Failed to create message port\n");
        unit->task = NULL;
        IExec->Signal(parent, ready_mask);
        return;
    }
    NVME_LEAK_INC(nvme_leak_port);
    unit->io_port_mask = 1UL << unit->io_port->mp_SigBit;

    /* Allocate persistent ISR signal bit */
    LONG sig = IExec->AllocSignal(-1);
    if (sig < 0) {
        DPRINTF(IExec, "[nvme.device:unit_task] Failed to allocate signal bit\n");
        IExec->FreeSysObject(ASOT_PORT, unit->io_port);
        NVME_LEAK_DEC(nvme_leak_port);
        unit->io_port = NULL;
        unit->task = NULL;
        IExec->Signal(parent, ready_mask);
        return;
    }
    NVME_LEAK_INC(nvme_leak_signal);
    unit->io_signal_mask = 1UL << sig;
    unit->io_wait_task   = self; /* ISR will signal self */

    /* Signal parent that we're ready */
    IExec->Signal(parent, ready_mask);

    /* Main event loop — hybrid ISR + yield-poll, also the sole path
     * when running in polling-mode.
     *
     * QEMU TCG (single-threaded emulation) does not reliably deliver
     * NVMe INTx interrupts: the bottom-half that posts CQEs may not
     * run until the guest CPU yields.  A pure Wait()-based design
     * deadlocks because Wait() only returns when a signal is set, but
     * QEMU can't fire the ISR until the guest yields — circular
     * dependency.
     *
     * Solution: when I/O is inflight, use SetSignal to poll for
     * signals and yield via Forbid()/Permit() between harvest
     * attempts.  This lets QEMU's main loop run (processing BHs and
     * asserting INTx) while keeping the task responsive to new
     * messages and any signal the ISR may raise.  When no I/O is
     * inflight, block in Wait() to avoid burning CPU.
     *
     * The same loop is also the polling-mode fallback: when no IRQ
     * was installed (unit->ctrl->polling_mode == TRUE), the ISR-signal
     * bit never fires, and the yield-poll is the sole driver of CQ
     * consumption.  The io_signal_mask is left in the wait mask
     * either way — it's harmless in polling-mode (never raised) and
     * necessary in IRQ-mode. */
    ULONG wait_mask = unit->io_port_mask | unit->io_signal_mask | SIGBREAKF_CTRL_C;

    while (!unit->task_shutdown) {
        ULONG signals;

        /* Check if any inflight I/O is pending */
        BOOL has_inflight = FALSE;
        for (int i = 0; i < NVME_MAX_INFLIGHT; i++) {
            if (unit->inflight[i].ioreq != NULL) { has_inflight = TRUE; break; }
        }

        if (has_inflight) {
            /* Yield-poll: check for signals without blocking, yield to
             * let QEMU process NVMe completions, then harvest. */
            signals = IExec->SetSignal(0, wait_mask);
            if (signals & wait_mask) {
                /* Clear the signals we're handling */
                IExec->SetSignal(0, signals & wait_mask);
            }
            if (!(signals & wait_mask)) {
                /* No signals yet — yield CPU so QEMU can process BHs,
                 * then try harvesting directly. */
                IExec->Forbid();
                IExec->Permit();
                NVMeIO_Harvest(devBase, unit);
                /* Unmask INTMC in case ISR masked during yield */
                if (unit->ctrl->irq_installed)
                    nvme_w32(unit->ctrl->iobase + NVME_REG_INTMC, 1);
                continue; /* re-check inflight and signals */
            }
        } else {
            /* No inflight I/O — safe to block until next event */
            signals = IExec->Wait(wait_mask);
        }

        if (signals & SIGBREAKF_CTRL_C)
            break;

        DPRINTF(IExec, "[nvme.device:task%lu] Woke: signals=0x%08lx (%s%s%s)\n",
                unit->unit_num, signals,
                (signals & unit->io_signal_mask) ? "ISR " : "",
                (signals & unit->io_port_mask)   ? "MSG " : "",
                (signals & SIGBREAKF_CTRL_C)     ? "CTRL_C " : "");

        /* Always harvest completions first — handles ISR wakeup */
        NVMeIO_Harvest(devBase, unit);

        /* Dispatch pending I/O requests.  dispatch_ioreq may submit an
         * NVMe SQE via the no-ring primitive in nvme_io.c; each path
         * that ends in NVMeIO_SubmitNoRing bumps the shadow SQ tail but
         * leaves the doorbell alone.  We snapshot io_sq_tail before the
         * drain and, after draining, ring the doorbell exactly once if
         * any SQEs were queued.  This collapses N doorbell writes into
         * one for a burst of N messages.
         *
         * Callers that need an immediate doorbell write (Flush, any
         * path that uses the convenience NVMeIO_Submit wrapper) are
         * unaffected — they ring themselves inline and we just observe
         * the same tail here, turning the second ring into a no-op
         * write of the same value. */
        if (signals & unit->io_port_mask) {
            UWORD tail_before = unit->io_sq_tail;
            struct IOStdReq *req;
            while ((req = (struct IOStdReq *)IExec->GetMsg(unit->io_port)) != NULL) {
                dispatch_ioreq(devBase, unit, req);
            }
            if (unit->io_sq_tail != tail_before)
                NVMeIO_RingSQ(unit);
            /* Harvest again after submitting to catch fast completions */
            NVMeIO_Harvest(devBase, unit);
        }

        /* No post-submit spin-poll here on purpose.  An experiment in
         * v1.63/v1.64 added a bounded Harvest spin after the doorbell
         * ring to catch immediate completions; on QEMU TCG — which
         * runs guest CPU and device emulation on the same thread —
         * the spin starved QEMU's main loop exactly when it needed to
         * progress the NVMe DMA, producing 11–13 % read regressions.
         * The outer loop's Forbid/Permit-based yield-poll already
         * handles fast completions correctly and gives QEMU the CPU
         * it needs.  See Session 10 in docs/history.md. */

        /* Ensure NVMe interrupts are unmasked before looping.
         * Writing INTMC when INTMS is already clear is harmless (NVMe spec). */
        if (unit->ctrl->irq_installed)
            nvme_w32(unit->ctrl->iobase + NVME_REG_INTMC, 1);
    }

    DLOG(IExec, "[nvme.device:task%lu] exit: draining port\n", unit->unit_num);

    /* Drain any pending I/O with abort */
    struct IOStdReq *req;
    while ((req = (struct IOStdReq *)IExec->GetMsg(unit->io_port)) != NULL) {
        req->io_Error = IOERR_ABORTED;
        IExec->ReplyMsg((struct Message *)req);
    }

    DLOG(IExec, "[nvme.device:task%lu] exit: freeing port+signal\n", unit->unit_num);

    IExec->FreeSignal(sig);
    NVME_LEAK_DEC(nvme_leak_signal);
    IExec->FreeSysObject(ASOT_PORT, unit->io_port);
    NVME_LEAK_DEC(nvme_leak_port);
    unit->io_port        = NULL;
    unit->io_wait_task   = NULL;
    unit->io_signal_mask = 0;

    DLOG(IExec, "[nvme.device:task%lu] exit: clearing unit->task (terminating)\n",
         unit->unit_num);

    /* Snapshot the ack destination before clearing unit->task — the
     * parent may free those fields as soon as it unblocks from Wait. */
    struct Task *ack_task = unit->shutdown_ack_task;
    ULONG        ack_mask = unit->shutdown_ack_mask;

    /* Publish task=NULL first.  When the parent unblocks it checks
     * unit->task and expects it to already be NULL. */
    unit->task = NULL;

    /* Fallback-spin path has NULL ack fields — parent spins on
     * unit->task, which we just cleared, so it'll exit shortly. */
    if (ack_task && ack_mask)
        IExec->Signal(ack_task, ack_mask);
}

/* ------------------------------------------------------------------ */
/* UnitTask_Start / UnitTask_Shutdown                                  */
/* ------------------------------------------------------------------ */

BOOL UnitTask_Start(struct NVMeBase *devBase, struct NVMeUnit *unit)
{
    struct ExecIFace    *IExec     = devBase->IExec;
    struct UtilityIFace *IUtility  = devBase->IUtility;
    ULONG                page_size = unit->ctrl->page_size;

    /* Allocate pre-pinned bounce buffers and PRP list pages.
     *
     * We deliberately keep the DMA mapping LIVE for the lifetime of
     * the unit task — StartDMA at task startup, no EndDMA until
     * shutdown.  This was v1.47's pattern and it is known-good on
     * QEMU Pegasos2.  The alternative (EndDMA with DMAF_NoModify +
     * explicit CacheClearE in nvme_io.c) as used by virtioscsi.device
     * turned out to cause I/O hangs on the first CMD_READ — the exact
     * cache-attribute state after DMAF_NoModify is not sufficiently
     * predictable across the bridge + MMU interaction on this
     * platform to risk it. */
    for (int i = 0; i < NVME_MAX_INFLIGHT; i++) {
        unit->bounce_bufs[i] = IExec->AllocVecTags(NVME_BOUNCE_SIZE,
            AVT_Type,      MEMF_SHARED,
            AVT_Alignment, page_size,
            AVT_Clear,     0, TAG_DONE);
        if (!unit->bounce_bufs[i]) goto fail;
        NVME_LEAK_INC(nvme_leak_vec);

        ULONG entries = IExec->StartDMA(unit->bounce_bufs[i], NVME_BOUNCE_SIZE, DMA_ReadFromRAM);
        if (entries == 0) {
            /* StartDMA reported no entries — treat as no-dma-started;
             * NULL the buffer pointer so the fail path doesn't try to
             * EndDMA a non-mapped buffer. */
            IExec->FreeVec(unit->bounce_bufs[i]);
            NVME_LEAK_DEC(nvme_leak_vec);
            unit->bounce_bufs[i] = NULL;
            goto fail;
        }
        NVME_LEAK_INC(nvme_leak_dma);
        struct DMAEntry *dma = IExec->AllocSysObjectTags(ASOT_DMAENTRY,
            ASODMAE_NumEntries, entries, TAG_DONE);
        if (!dma) {
            IExec->EndDMA(unit->bounce_bufs[i], NVME_BOUNCE_SIZE, DMA_ReadFromRAM);
            NVME_LEAK_DEC(nvme_leak_dma);
            IExec->FreeVec(unit->bounce_bufs[i]);
            NVME_LEAK_DEC(nvme_leak_vec);
            unit->bounce_bufs[i] = NULL;
            goto fail;
        }
        NVME_LEAK_INC(nvme_leak_dmaentry);
        IExec->GetDMAList(unit->bounce_bufs[i], NVME_BOUNCE_SIZE, DMA_ReadFromRAM, dma);
        unit->bounce_phys[i] = (ULONG)dma[0].PhysicalAddress;
        IExec->FreeSysObject(ASOT_DMAENTRY, dma);
        NVME_LEAK_DEC(nvme_leak_dmaentry);

        /* Intentionally NO EndDMA here — the buffer stays DMA-pinned
         * (and hence cache-inhibited) until UnitTask_Shutdown.  Both
         * the CPU and the NVMe controller see the same bytes without
         * any explicit cache flushing. */

        unit->prp_list_pages[i] = IExec->AllocVecTags(page_size,
            AVT_Type,      MEMF_SHARED,
            AVT_Alignment, page_size,
            AVT_Clear,     0, TAG_DONE);
        if (!unit->prp_list_pages[i]) goto fail;
        NVME_LEAK_INC(nvme_leak_vec);

        ULONG prp_entries = IExec->StartDMA(unit->prp_list_pages[i], page_size, DMA_ReadFromRAM);
        if (prp_entries == 0) {
            IExec->FreeVec(unit->prp_list_pages[i]);
            NVME_LEAK_DEC(nvme_leak_vec);
            unit->prp_list_pages[i] = NULL;
            goto fail;
        }
        NVME_LEAK_INC(nvme_leak_dma);
        struct DMAEntry *prp_dma = IExec->AllocSysObjectTags(ASOT_DMAENTRY,
            ASODMAE_NumEntries, prp_entries, TAG_DONE);
        if (!prp_dma) {
            IExec->EndDMA(unit->prp_list_pages[i], page_size, DMA_ReadFromRAM);
            NVME_LEAK_DEC(nvme_leak_dma);
            IExec->FreeVec(unit->prp_list_pages[i]);
            NVME_LEAK_DEC(nvme_leak_vec);
            unit->prp_list_pages[i] = NULL;
            goto fail;
        }
        NVME_LEAK_INC(nvme_leak_dmaentry);
        IExec->GetDMAList(unit->prp_list_pages[i], page_size, DMA_ReadFromRAM, prp_dma);
        unit->prp_list_phys[i] = (ULONG)prp_dma[0].PhysicalAddress;
        IExec->FreeSysObject(ASOT_DMAENTRY, prp_dma);
        NVME_LEAK_DEC(nvme_leak_dmaentry);

        /* Same policy as the bounce buffer — leave the PRP list page
         * DMA-pinned (hence cache-inhibited) for the unit task's
         * lifetime.  `stwbrx` writes in Submit already bypass the
         * cache, but keeping the page CI also means the device never
         * reads stale data on its PRP-list dereference. */
    }

    /* Pre-allocate one DMAEntry array per inflight slot so the direct-
     * DMA path in NVMeIO_Submit does not need an AllocSysObjectTags
     * call on every I/O.  Capacity must cover the worst-case number of
     * physical fragments a single NVMe command can span: that is
     * max_transfer_bytes / page_size pages of data plus one extra to
     * cover an unaligned start.  We never expect to exceed this, but
     * NVMeIO_Submit retains a per-I/O AllocSysObject fallback just in
     * case.
     *
     * Failure to allocate the pool is NON-fatal — we leave
     * dma_entry_pool[i] = NULL and the Submit path will fall back to
     * per-I/O AllocSysObject for every direct-DMA request on that
     * slot.  Correctness is preserved; only the perf win is lost. */
    {
        ULONG max_xfer       = unit->ctrl->max_transfer_bytes;
        ULONG worst_frags    = (max_xfer + page_size - 1) / page_size + 1u;
        unit->dma_entry_pool_capacity = worst_frags;

        for (int i = 0; i < NVME_MAX_INFLIGHT; i++) {
            unit->dma_entry_pool[i] = IExec->AllocSysObjectTags(ASOT_DMAENTRY,
                ASODMAE_NumEntries, worst_frags, TAG_DONE);
            if (unit->dma_entry_pool[i]) {
                NVME_LEAK_INC(nvme_leak_dmaentry);
            } else {
                /* Log once — subsequent nulls are noise.  Not a fatal
                 * error, just a performance degradation. */
                if (i == 0) {
                    DPRINTF(IExec, "[nvme.device:unit_task] dma_entry_pool alloc"
                            " failed (worst_frags=%lu) — falling back to per-I/O\n",
                            worst_frags);
                }
            }
        }

    }

    /* Allocate a ready signal bit in the current (parent) task */
    LONG ready_bit = IExec->AllocSignal(-1);
    if (ready_bit < 0) goto fail;

    /* Build the start message on the stack — task entry reads before we return */
    struct NVMeTaskStartMsg startMsg;
    startMsg.devBase     = devBase;
    startMsg.unit        = unit;
    startMsg.parent_task = IExec->FindTask(NULL);
    startMsg.ready_bit   = ready_bit;
    startMsg.ready_mask  = 1UL << ready_bit;

    unit->task_shutdown = FALSE;

    /* Build a per-unit task name */
    char task_name[32];
    IUtility->SNPrintf(task_name, sizeof(task_name), "nvme.device unit %lu", unit->unit_num);

    /* Create the unit task under Forbid so we can set tc_UserData before it runs */
    struct Task *t;
    IExec->Forbid();
    t = IExec->CreateTaskTags(task_name, 0, unit_task_entry, 16384, TAG_DONE);
    if (t) {
        t->tc_UserData = &startMsg;
        unit->task = t;
    }
    IExec->Permit();

    if (!t) {
        IExec->FreeSignal(ready_bit);
        goto fail;
    }

    /* Wait for task to signal us when ready (or on failure) */
    IExec->Wait(startMsg.ready_mask);
    IExec->FreeSignal(ready_bit);

    if (!unit->io_port) {
        /* Task failed to initialise */
        unit->task = NULL;
        return FALSE;
    }

    return TRUE;

fail:
    /* Partial-init cleanup.  Tear down any DMA pins we already acquired
     * before returning FALSE.  Indexing by the per-slot pointers is
     * exact — StartDMA is never issued until the AllocVec succeeds, so
     * a non-NULL buffer pointer implies a live DMA mapping. */
    for (int i = 0; i < NVME_MAX_INFLIGHT; i++) {
        if (unit->bounce_bufs[i]) {
            IExec->EndDMA(unit->bounce_bufs[i], NVME_BOUNCE_SIZE, DMA_ReadFromRAM);
            NVME_LEAK_DEC(nvme_leak_dma);
            IExec->FreeVec(unit->bounce_bufs[i]);
            NVME_LEAK_DEC(nvme_leak_vec);
            unit->bounce_bufs[i] = NULL;
        }
        if (unit->prp_list_pages[i]) {
            IExec->EndDMA(unit->prp_list_pages[i], page_size, DMA_ReadFromRAM);
            NVME_LEAK_DEC(nvme_leak_dma);
            IExec->FreeVec(unit->prp_list_pages[i]);
            NVME_LEAK_DEC(nvme_leak_vec);
            unit->prp_list_pages[i] = NULL;
        }
        if (unit->dma_entry_pool[i]) {
            IExec->FreeSysObject(ASOT_DMAENTRY, unit->dma_entry_pool[i]);
            NVME_LEAK_DEC(nvme_leak_dmaentry);
            unit->dma_entry_pool[i] = NULL;
        }
    }
    unit->dma_entry_pool_capacity = 0;
    return FALSE;
}

void UnitTask_Shutdown(struct NVMeBase *devBase, struct NVMeUnit *unit)
{
    struct ExecIFace *IExec = devBase->IExec;

    DLOG(IExec, "[nvme.device:shutdown] enter unit=%lu task=%p\n",
         unit->unit_num, unit->task);

    if (!unit->task) {
        DLOG(IExec, "[nvme.device:shutdown] already stopped — early return\n");
        return;
    }

    /* Signal handshake.  We can't just busy-wait on unit->task because
     * the unit task runs at the same priority as us — Forbid/Permit is
     * a nest-count barrier, NOT a yield, so the unit task would never
     * get scheduled.  Instead we block in Wait() while the unit task
     * signals us back before it clears unit->task. */
    LONG ack_bit = IExec->AllocSignal(-1);
    if (ack_bit < 0) {
        DLOG(IExec, "[nvme.device:shutdown] AllocSignal failed —"
                    " falling back to spin\n");
        /* Last-resort fallback: the spin will still waste CPU but at
         * least won't leak a signal bit on the caller. */
        unit->shutdown_ack_task = NULL;
        unit->shutdown_ack_mask = 0;
        unit->task_shutdown = TRUE;
        IExec->Signal(unit->task, SIGBREAKF_CTRL_C);
        while (unit->task != NULL) {
            IExec->Forbid();
            IExec->Permit();
        }
        return;
    }

    unit->shutdown_ack_task = IExec->FindTask(NULL);
    unit->shutdown_ack_mask = 1UL << ack_bit;

    /* Memory-barrier ordering: publish ack fields BEFORE setting
     * task_shutdown, so the task never sees shutdown without the ack
     * target.  Forbid/Permit pair is the cheapest portable barrier. */
    IExec->Forbid();
    unit->task_shutdown = TRUE;
    IExec->Permit();

    IExec->Signal(unit->task, SIGBREAKF_CTRL_C);
    DLOG(IExec, "[nvme.device:shutdown] signalled CTRL_C; Wait()ing for ack\n");

    IExec->Wait(unit->shutdown_ack_mask);
    IExec->FreeSignal(ack_bit);
    unit->shutdown_ack_task = NULL;
    unit->shutdown_ack_mask = 0;

    DLOG(IExec, "[nvme.device:shutdown] ack received — task=%p\n", unit->task);

    /* Release the persistent DMA pins first, then the backing memory. */
    ULONG page_size = unit->ctrl->page_size;
    for (int i = 0; i < NVME_MAX_INFLIGHT; i++) {
        if (unit->bounce_bufs[i]) {
            IExec->EndDMA(unit->bounce_bufs[i], NVME_BOUNCE_SIZE, DMA_ReadFromRAM);
            NVME_LEAK_DEC(nvme_leak_dma);
            IExec->FreeVec(unit->bounce_bufs[i]);
            NVME_LEAK_DEC(nvme_leak_vec);
            unit->bounce_bufs[i] = NULL;
        }
        if (unit->prp_list_pages[i]) {
            IExec->EndDMA(unit->prp_list_pages[i], page_size, DMA_ReadFromRAM);
            NVME_LEAK_DEC(nvme_leak_dma);
            IExec->FreeVec(unit->prp_list_pages[i]);
            NVME_LEAK_DEC(nvme_leak_vec);
            unit->prp_list_pages[i] = NULL;
        }
        if (unit->dma_entry_pool[i]) {
            IExec->FreeSysObject(ASOT_DMAENTRY, unit->dma_entry_pool[i]);
            NVME_LEAK_DEC(nvme_leak_dmaentry);
            unit->dma_entry_pool[i] = NULL;
        }
    }
    unit->dma_entry_pool_capacity = 0;
}
