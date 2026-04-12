/*
 * nvme_status.c — NVMe CQE status → AmigaOS io_Error translator.
 *
 * The mapping here is the single source of truth; every code path that
 * completes an IORequest from an NVMe CQE should call NVMe_StatusToIOErr
 * rather than picking its own io_Error value.  This keeps user-facing
 * error semantics consistent and makes future refinements (e.g. special
 * handling of LBA-out-of-range) a one-line change.
 *
 * Status code references below are NVMe 1.4 §4.6.1.2.1 tables.
 */

#include "nvme_status.h"

#include <devices/scsidisk.h>   /* HFERR_BadStatus */
#include <exec/errors.h>        /* IOERR_* */

/* Extract SCT (status code type) and SC (status code) from the raw
 * 16-bit CQE status word.  We deliberately accept either "phase bit in
 * bit 0" or "phase bit already cleared" forms; the shift strips bit 0
 * either way. */
static inline UBYTE sct_of(UWORD s) { return (UBYTE)((s >> 9) & 0x7); }
static inline UBYTE sc_of (UWORD s) { return (UBYTE)((s >> 1) & 0xFF); }

LONG NVMe_StatusToIOErr(UWORD status_word)
{
    UBYTE sct = sct_of(status_word);
    UBYTE sc  = sc_of (status_word);

    /* Success is the common case — always SCT=0, SC=0. */
    if (sct == 0 && sc == 0)
        return 0;

    switch (sct) {

    /* Generic Command Status (§4.6.1.2.1 Figure 126) */
    case 0:
        switch (sc) {
        case 0x07:  /* Command Abort Requested */
        case 0x08:  /* Command Aborted due to SQ Deletion */
        case 0x09:  /* Command Aborted due to Failed Fused Command */
        case 0x0A:  /* Command Aborted due to Missing Fused Command */
        case 0x05:  /* Commands Aborted due to Power Loss Notification */
            return IOERR_ABORTED;

        case 0x80:  /* LBA Out Of Range */
        case 0x81:  /* Capacity Exceeded */
        case 0x82:  /* Namespace Not Ready */
        case 0x83:  /* Reservation Conflict */
            return IOERR_BADADDRESS;

        case 0x01:  /* Invalid Command Opcode */
        case 0x02:  /* Invalid Field in Command */
        case 0x0B:  /* Invalid Namespace or Format */
            return IOERR_NOCMD;

        case 0x04:  /* Data Transfer Error */
        case 0x06:  /* Internal Error */
        default:
            return HFERR_BadStatus;
        }

    /* Command Specific Status (§4.6.1.2.1 Figure 127) */
    case 1:
        return HFERR_BadStatus;

    /* Media and Data Integrity Errors (§4.6.1.2.1 Figure 128) */
    case 2:
        /* Unrecovered read / write fault, end-to-end errors, etc. —
         * any of these mean the LBA's data is untrustworthy. */
        return IOERR_BADADDRESS;

    /* Path Related Status (§4.6.1.2.1) */
    case 3:
        return HFERR_BadStatus;

    /* Vendor Specific */
    case 7:
        return HFERR_BadStatus;

    default:
        return HFERR_BadStatus;
    }
}

const char *NVMe_StatusDescribe(UWORD status_word)
{
    UBYTE sct = sct_of(status_word);
    UBYTE sc  = sc_of (status_word);

    if (sct == 0 && sc == 0)
        return "Success";

    if (sct == 0) {
        switch (sc) {
        case 0x01: return "Invalid Opcode";
        case 0x02: return "Invalid Field";
        case 0x03: return "Command ID Conflict";
        case 0x04: return "Data Transfer Error";
        case 0x05: return "Power-Loss Abort";
        case 0x06: return "Internal Error";
        case 0x07: return "Abort Requested";
        case 0x08: return "Abort (SQ Deleted)";
        case 0x09: return "Abort (Failed Fused)";
        case 0x0A: return "Abort (Missing Fused)";
        case 0x0B: return "Invalid NS/Format";
        case 0x0C: return "Command Sequence Error";
        case 0x80: return "LBA Out of Range";
        case 0x81: return "Capacity Exceeded";
        case 0x82: return "Namespace Not Ready";
        case 0x83: return "Reservation Conflict";
        default:   return "Generic Error";
        }
    }
    if (sct == 1) return "Command-Specific Error";
    if (sct == 2) return "Media/Data-Integrity Error";
    if (sct == 3) return "Path-Related Error";
    if (sct == 7) return "Vendor-Specific Error";
    return "Unknown";
}
