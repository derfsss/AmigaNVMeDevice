/*
 * scsi_mode.c — MODE SENSE(6/10) and MODE SELECT(6/10) for the single
 *               mode page we meaningfully translate: 0x08 (Caching).
 *
 *   SBC-4  §6.4.5    Caching Mode Page
 *   NVMe 1.4 §5.21.1.6   Feature 0x06 — Volatile Write Cache
 *
 * SCSI-aware consumers (filesystem tools, block-device utility
 * libraries) toggle the device's volatile write cache through this
 * path.  The one bit that matters is WCE (Write Cache Enable, byte 2
 * bit 2 of the Caching page); all other bytes in the page are
 * reported as zero and accepted as zero on write.
 *
 * NVMe has an admin Set Features command with feature identifier 0x06
 * (Volatile Write Cache), cdw11 bit 0 = 1 to enable.  We cache the
 * last-written value on the controller so that MODE SENSE's answer
 * reflects the state we last set, without issuing a Get Features on
 * every probe.  The VWC-present bit (ctrl->vwc_present) was sampled
 * from Identify Controller byte 525 at init.
 */

#include "nvme_device.h"
#include "nvme_scsi.h"
#include "nvme_admin.h"
#include <devices/scsidisk.h>
#include <dos/dos.h>
#include <string.h>

/* ---------------------------------------------------------------- */
/* Build a Caching-page response into `dst`.  Returns 20 (page size). */
/* Page format per SBC-4 §6.4.5: byte 0 = page code (0x08),           */
/*                                byte 1 = page length (18, 0x12).    */
/*                                bytes 2-19 = parameters; only WCE   */
/*                                (byte 2 bit 2) is meaningful here.  */
/* ---------------------------------------------------------------- */
static ULONG fill_caching_page(UBYTE *dst, BOOL wce)
{
    memset(dst, 0, 20);
    dst[0] = 0x08;          /* page code */
    dst[1] = 0x12;          /* page length = 18 (bytes after this) */
    /* Byte 2 bits: [IC=0][ABPF=0][CAP=0][DISC=0][SIZE=0][WCE][MF=0][RCD=0] */
    dst[2] = wce ? 0x04u : 0x00u;
    /* Bytes 3-19 left zero — we don't track readahead / demand retention
     * parameters here; NVMe has no analogous knobs exposed this way. */
    return 20;
}

/* ---------------------------------------------------------------- */
/* MODE SENSE                                                        */
/*                                                                    */
/* Accepts both the 6-byte (CDB 0x1A) and 10-byte (CDB 0x5A) variants.*/
/* The variants differ only in CDB layout and the header size of the  */
/* response (4 vs 8 bytes).  All other shape is shared.              */
/*                                                                    */
/* CDB(6):  opcode 0x1A | dbd,reserved | pcf:pageCode | subPage |     */
/*          alloc_len | control                                       */
/* CDB(10): opcode 0x5A | dbd,reserved | pcf:pageCode | subPage |     */
/*          reserved*3 | alloc_len:2 | control                        */
/*                                                                    */
/* Supported pages:                                                   */
/*   0x3F — return all supported pages (just 0x08 for us)            */
/*   0x08 — Caching                                                   */
/* Any other page -> CHECK CONDITION / ILLEGAL REQUEST / INVALID     */
/* FIELD IN CDB.                                                      */
/* ---------------------------------------------------------------- */
void NVMe_SCSI_HandleModeSense(struct NVMeBase *devBase,
                               struct NVMeUnit *unit,
                               struct IOStdReq *ioreq)
{
    struct ExecIFace *IExec = devBase->IExec;
    struct SCSICmd   *scsi  = (struct SCSICmd *)ioreq->io_Data;
    UBYTE *cdb = (UBYTE *)scsi->scsi_Command;

    (void)IExec;

    UBYTE opcode = cdb[0];
    BOOL  is10   = (opcode == 0x5A);
    UBYTE page_code  = cdb[2] & 0x3Fu;
    ULONG alloc_len  = is10 ? (((ULONG)cdb[7] << 8) | cdb[8])
                            : (ULONG)cdb[4];

    if (!scsi->scsi_Data || scsi->scsi_Length < alloc_len) {
        scsi->scsi_Status = 2;
        ioreq->io_Error = HFERR_BadStatus;
        return;
    }

    UBYTE *out = (UBYTE *)scsi->scsi_Data;
    ULONG header_len = is10 ? 8u : 4u;

    if (alloc_len < header_len) {
        /* Caller didn't even leave room for the header.  Report a
         * truncated zero response rather than failing — permits
         * probes with a tiny allocation length. */
        memset(out, 0, alloc_len);
        scsi->scsi_Actual = alloc_len;
        ioreq->io_Actual  = alloc_len;
        ioreq->io_Error   = 0;
        return;
    }

    /* Clear header — fields that matter are written explicitly below. */
    memset(out, 0, header_len);

    ULONG body_off  = header_len;
    ULONG body_room = (alloc_len > header_len) ? (alloc_len - header_len) : 0;
    ULONG body_used = 0;

    BOOL  wce = unit->ctrl->vwc_enabled;

    if (page_code == 0x08 || page_code == 0x3F) {
        if (body_room >= 20u) {
            body_used = fill_caching_page(out + body_off, wce);
        } else {
            /* Not enough room for the full page — emit as much as we
             * can (SPC permits truncation).  The header's mode data
             * length will still reference the full length the client
             * asked for, per spec. */
            UBYTE tmp[20];
            fill_caching_page(tmp, wce);
            IExec->CopyMem(tmp, out + body_off, body_room);
            body_used = body_room;
        }
    } else {
        /* Unknown page — reject. */
        scsi->scsi_Status = 2;
        NVMe_SCSI_FillSense(scsi, NVME_SSK_ILLEGAL_REQUEST, 0x24, 0x00);
        ioreq->io_Error = HFERR_BadStatus;
        return;
    }

    /* Fill the mode-parameter header's data length.  The field is
     * "remaining byte count after this field" so it excludes itself
     * but includes everything that follows.  We report the amount we
     * actually wrote to keep the response self-consistent. */
    ULONG total_bytes = header_len + body_used;
    if (is10) {
        ULONG mdl = total_bytes - 2u; /* mode data length (bytes 0-1) */
        out[0] = (UBYTE)(mdl >> 8);
        out[1] = (UBYTE)(mdl);
        /* out[2] = medium type = 0 (direct access, per default). */
        /* out[3] = device-specific parameter = 0. */
        /* out[4-5] reserved; out[6-7] block descriptor length = 0. */
    } else {
        UBYTE mdl = (UBYTE)((total_bytes - 1u) & 0xFFu);
        out[0] = mdl;
        /* out[1] = medium type, out[2] = device-specific, out[3] = BD length. */
    }

    scsi->scsi_Status = 0;
    scsi->scsi_Actual = total_bytes;
    ioreq->io_Actual  = total_bytes;
    ioreq->io_Error   = 0;
}

/* ---------------------------------------------------------------- */
/* MODE SELECT                                                       */
/*                                                                    */
/* Accepts MODE SELECT(6) (CDB 0x15) and MODE SELECT(10) (0x55).     */
/* The parameter list carries a header and (optionally) mode pages.  */
/* We locate page 0x08 by walking the pages after the header, extract*/
/* the WCE bit, and — if it differs from the controller's last known */
/* state — issue Set Features 0x06 with the new value.               */
/*                                                                    */
/* If VWC is not present on this controller (vwc_present == FALSE)   */
/* we accept the request but take no action (toggle is a no-op).     */
/* ---------------------------------------------------------------- */
void NVMe_SCSI_HandleModeSelect(struct NVMeBase *devBase,
                                struct NVMeUnit *unit,
                                struct IOStdReq *ioreq)
{
    struct ExecIFace *IExec = devBase->IExec;
    struct SCSICmd   *scsi  = (struct SCSICmd *)ioreq->io_Data;
    struct NVMeController *ctrl = unit->ctrl;
    UBYTE *cdb = (UBYTE *)scsi->scsi_Command;

    (void)IExec;

    UBYTE opcode = cdb[0];
    BOOL  is10   = (opcode == 0x55);
    ULONG param_len = is10 ? (((ULONG)cdb[7] << 8) | cdb[8])
                           : (ULONG)cdb[4];

    if (param_len == 0) {
        /* Zero-length MODE SELECT is a no-op. */
        scsi->scsi_Status = 0;
        scsi->scsi_Actual = 0;
        ioreq->io_Actual  = 0;
        ioreq->io_Error   = 0;
        return;
    }

    if (!scsi->scsi_Data || scsi->scsi_Length < param_len) {
        scsi->scsi_Status = 2;
        NVMe_SCSI_FillSense(scsi, NVME_SSK_ILLEGAL_REQUEST, 0x1A, 0x00);
        ioreq->io_Error = HFERR_BadStatus;
        return;
    }

    UBYTE *in = (UBYTE *)scsi->scsi_Data;
    ULONG header_len = is10 ? 8u : 4u;

    if (param_len < header_len) {
        scsi->scsi_Status = 2;
        NVMe_SCSI_FillSense(scsi, NVME_SSK_ILLEGAL_REQUEST, 0x1A, 0x00);
        ioreq->io_Error = HFERR_BadStatus;
        return;
    }

    /* Block-descriptor-length field (after header):
     *   MODE SELECT(6):  byte 3, 8-bit value.
     *   MODE SELECT(10): bytes 6-7, 16-bit big-endian.
     * The block descriptors, if any, follow the header; we skip them
     * without processing (they describe medium geometry which we
     * don't honour — sector size comes from NVMe Identify). */
    ULONG bd_len = is10 ? (((ULONG)in[6] << 8) | in[7]) : (ULONG)in[3];
    if (header_len + bd_len > param_len) {
        scsi->scsi_Status = 2;
        NVMe_SCSI_FillSense(scsi, NVME_SSK_ILLEGAL_REQUEST, 0x26, 0x00);
        ioreq->io_Error = HFERR_BadStatus;
        return;
    }

    /* Walk mode pages starting after the header + block descriptors. */
    ULONG off = header_len + bd_len;
    BOOL  wce_new      = ctrl->vwc_enabled;  /* default: no change */
    BOOL  saw_caching  = FALSE;

    while (off + 2u <= param_len) {
        UBYTE pc  = in[off] & 0x3Fu;
        UBYTE plen = in[off + 1];
        /* Page header is 2 bytes; total page size = 2 + plen. */
        if (off + 2u + plen > param_len) {
            scsi->scsi_Status = 2;
            NVMe_SCSI_FillSense(scsi, NVME_SSK_ILLEGAL_REQUEST, 0x26, 0x01);
            ioreq->io_Error = HFERR_BadStatus;
            return;
        }

        if (pc == 0x08 && plen >= 1) {
            saw_caching = TRUE;
            wce_new = (in[off + 2] & 0x04u) ? TRUE : FALSE;
        }
        /* Other pages silently accepted — we don't track them. */
        off += 2u + plen;
    }

    /* If the client actually sent a caching page and the WCE state
     * would change, push it to the controller via Set Features 0x06. */
    if (saw_caching && ctrl->vwc_present && wce_new != ctrl->vwc_enabled) {
        UWORD st = NVMe_SetFeature(ctrl, NVME_FEATURE_VOLATILE_WRITE_CACHE,
                                   wce_new ? 1u : 0u, 0);
        if (st != NVME_STATUS_SUCCESS) {
            scsi->scsi_Status = 2;
            NVMe_SCSI_FillSense(scsi, NVME_SSK_MEDIUM_ERROR, 0x00, 0x00);
            ioreq->io_Error = HFERR_BadStatus;
            return;
        }
        ctrl->vwc_enabled = wce_new;
    }

    scsi->scsi_Status = 0;
    scsi->scsi_Actual = param_len;
    ioreq->io_Actual  = param_len;
    ioreq->io_Error   = 0;
}
