/*
 * scsi_log_sense.c — LOG SENSE (CDB 0x4D).
 *
 * Two pages are answered:
 *
 *   0x00 — Supported Log Pages.  Minimal list: just 0x00 and 0x2F.
 *   0x2F — Informational Exceptions (SPC-4 §7.3.11).  Status byte
 *          reflects the NVMe SMART critical-warning bits: if any of
 *          those bits are set we return an ASC/ASCQ that health tools
 *          treat as a warning.  Otherwise all zeros = "device OK".
 *
 * Other pages return CHECK CONDITION / ILLEGAL REQUEST — tools fall
 * back to their next query.  The shared sense-data helper used here
 * also serves the ATA PASS-THROUGH handler and the top-level switch
 * in unit_task.c, which is why it lives in the scsi_cmds/ module.
 */

#include "nvme_device.h"
#include "nvme_scsi.h"
#include "nvme_debug.h"

#include <devices/scsidisk.h>
#include <exec/errors.h>
#include <string.h>

/* -------------------------------------------------------------------
 * Shared auto-sense filler (used by ata_passthrough and log_sense).
 * ------------------------------------------------------------------ */

void NVMe_SCSI_FillSense(struct SCSICmd *scsi, UBYTE sense_key,
                         UBYTE asc, UBYTE ascq)
{
    if (scsi->scsi_SenseData && scsi->scsi_SenseLength >= 18) {
        UBYTE *s = (UBYTE *)scsi->scsi_SenseData;
        memset(s, 0, 18);
        s[0]  = 0x70;       /* fixed-format current error */
        s[2]  = sense_key;
        s[7]  = 10;         /* additional sense length */
        s[12] = asc;
        s[13] = ascq;
        scsi->scsi_SenseActual = 18;
    }
}

/* -------------------------------------------------------------------
 * LOG SENSE helpers
 * ------------------------------------------------------------------ */

/* Page 0x00 — Supported Log Pages.  Header (4 bytes) + page list. */
static ULONG emit_supported_pages(UBYTE *buf, ULONG alloc)
{
    const UBYTE supported[] = { 0x00, 0x2F };
    UBYTE resp[4 + sizeof(supported)];
    memset(resp, 0, sizeof(resp));
    resp[0] = 0x00;                         /* page code + SPF/DS */
    resp[1] = 0x00;                         /* subpage */
    resp[2] = 0x00;                         /* page length (hi) */
    resp[3] = (UBYTE)sizeof(supported);     /* page length (lo) */
    memcpy(&resp[4], supported, sizeof(supported));

    ULONG n = sizeof(resp);
    if (alloc < n) n = alloc;
    memcpy(buf, resp, n);
    return n;
}

/* Page 0x2F — Informational Exceptions.
 *
 * SPC-4 §7.3.11 parameter layout for the single parameter we emit:
 *   [0-1]  parameter code (0x0000)
 *   [2]    format/link flags (0x03 — LBIN | LP | binary)
 *   [3]    parameter length = 4
 *   [4]    IE ASC  — 0x00 if healthy, 0x5D if NVMe critical-warning set
 *   [5]    IE ASCQ — 0x00 / matching critical detail
 *   [6]    most recent temperature reading (°C) — optional, useful
 *   [7]    most recent trip temperature threshold (0 = none)
 */
static ULONG emit_ie_page(UBYTE *buf, ULONG alloc,
                          struct NVMeController *ctrl)
{
    UBYTE ie_asc  = 0x00;
    UBYTE ie_ascq = 0x00;
    UBYTE temp_c  = 0;

#ifdef ENABLE_SMART
    if (ctrl->smart_cache.valid) {
        struct NVMeSMARTCache *v = &ctrl->smart_cache;
        int t = (int)v->temp_k - 273;
        if (t < 0) t = 0;
        if (t > 255) t = 255;
        temp_c = (UBYTE)t;

        if (v->critical_warning != 0) {
            /* ASC 0x5D = "HARDWARE IMPENDING FAILURE" — the generic
             * value smartctl et al. recognise as a warning state. */
            ie_asc  = 0x5D;
            ie_ascq = 0x10;  /* general hardware failure */
        }
    }
#else
    (void)ctrl;
#endif

    UBYTE param[4 + 4];
    memset(param, 0, sizeof(param));
    param[0] = 0x00;
    param[1] = 0x00;
    param[2] = 0x03;                /* binary format, LP set */
    param[3] = 0x04;                /* parameter length */
    param[4] = ie_asc;
    param[5] = ie_ascq;
    param[6] = temp_c;
    param[7] = 0x00;

    UBYTE header[4];
    memset(header, 0, sizeof(header));
    header[0] = 0x2F;                /* page code */
    header[1] = 0x00;                /* subpage */
    header[2] = 0x00;
    header[3] = (UBYTE)sizeof(param);

    UBYTE resp[sizeof(header) + sizeof(param)];
    memcpy(resp,                     header, sizeof(header));
    memcpy(resp + sizeof(header),    param,  sizeof(param));

    ULONG n = sizeof(resp);
    if (alloc < n) n = alloc;
    memcpy(buf, resp, n);
    return n;
}

/* -------------------------------------------------------------------
 * Top-level CDB handler
 * ------------------------------------------------------------------ */

void NVMe_SCSI_HandleLogSense(struct NVMeBase *devBase,
                              struct NVMeUnit *unit,
                              struct IOStdReq *ioreq)
{
    struct ExecIFace *IExec = devBase->IExec;
    struct SCSICmd   *scsi  = (struct SCSICmd *)ioreq->io_Data;
    UBYTE            *cdb   = scsi->scsi_Command;

    UBYTE page = cdb[2] & 0x3F;              /* page code, low 6 bits */
    UBYTE *buf = (UBYTE *)scsi->scsi_Data;
    ULONG alloc = scsi->scsi_Length;

    DPRINTF(IExec, "[nvme.device:scsi-log] LOG SENSE page=0x%02lx alloc=%lu\n",
            (ULONG)page, alloc);

    if (!buf || alloc == 0) {
        scsi->scsi_Status = 2;
        scsi->scsi_Actual = 0;
        NVMe_SCSI_FillSense(scsi, NVME_SSK_ILLEGAL_REQUEST, 0x24, 0x00);
        ioreq->io_Error = HFERR_BadStatus;
        return;
    }

    ULONG actual = 0;
    switch (page) {
    case 0x00:
        actual = emit_supported_pages(buf, alloc);
        break;
    case 0x2F:
        actual = emit_ie_page(buf, alloc, unit->ctrl);
        break;
    default:
        scsi->scsi_Status = 2;
        scsi->scsi_Actual = 0;
        NVMe_SCSI_FillSense(scsi, NVME_SSK_ILLEGAL_REQUEST, 0x24, 0x00);
        ioreq->io_Error = HFERR_BadStatus;
        return;
    }

    scsi->scsi_Actual  = actual;
    scsi->scsi_Status  = 0;
    ioreq->io_Actual   = actual;
    ioreq->io_Error    = 0;
}
