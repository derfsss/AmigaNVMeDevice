/*
 * scsi_unmap.c — SCSI UNMAP (CDB 0x42) translated to NVMe Dataset
 * Management with the Deallocate attribute (TRIM).
 *
 *   SPC-4  §6.14      UNMAP command
 *   SBC-4  §5.28      UNMAP parameter data (16-byte block descriptors)
 *   NVMe 1.4 §6.7     Dataset Management command
 *
 * The library consumer (e.g. blockdev.library's Trim() / TrimRanges())
 * hands us a parameter list whose header carries a count of 16-byte
 * block descriptors, each {LBA:8, blocks:4, reserved:4}.  We translate
 * those into NVMe's 16-byte range-descriptor format
 * {attributes:4, nlb:4, slba:8}, fill the unit's pinned DSM buffer,
 * and issue a single Dataset Management command (opcode 0x09) with
 * AD=1.  We block on the I/O queue via NVMeIO_SubmitAndWait until the
 * controller replies, then translate status into SCSI scsi_Status.
 *
 * Prerequisites checked on every call:
 *   - Controller advertised ONCS.DSM (Identify byte 520 bit 2).
 *   - Unit successfully allocated dsm_buf at UnitTask_Start.
 *   - The CDB's ANCHOR bit (byte 1 bit 0) is clear — we don't support
 *     anchoring (it's a WRITE SAME-style precondition, not applicable
 *     to TRIM).
 *
 * Range-count bound: NVMe caps at NVME_DSM_MAX_RANGES = 256 per command.
 * The caller's list may be shorter; if it's longer we truncate and
 * report BLKDEV_ERROR_NOT_SUPPORTED-equivalent (ILLEGAL REQUEST).
 */

#include "nvme_device.h"
#include "nvme_scsi.h"
#include "nvme_io.h"
#include <devices/scsidisk.h>
#include <dos/dos.h>
#include <string.h>

/* Write a 32-bit little-endian word into the DMA buffer via stwbrx,
 * matching the NVMe spec's on-the-wire byte order.  Local helper — we
 * deliberately don't pull the one from nvme_io.c (file scope). */
static inline void dsm_w32_le(void *addr, ULONG val)
{
    __asm__ volatile ("stwbrx %0, 0, %1" : : "r"(val), "r"(addr) : "memory");
}

/* Read a 64-bit big-endian LBA from an SBC-4 UNMAP block descriptor. */
static inline uint64 be64_read(const UBYTE *p)
{
    return ((uint64)p[0] << 56) | ((uint64)p[1] << 48)
         | ((uint64)p[2] << 40) | ((uint64)p[3] << 32)
         | ((uint64)p[4] << 24) | ((uint64)p[5] << 16)
         | ((uint64)p[6] << 8)  | ((uint64)p[7]);
}

/* Read a 32-bit big-endian block count from an UNMAP block descriptor. */
static inline ULONG be32_read(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16)
         | ((ULONG)p[2] << 8)  | ((ULONG)p[3]);
}

/* Read 16-bit big-endian from two consecutive bytes (header fields). */
static inline UWORD be16_read(const UBYTE *p)
{
    return (UWORD)(((UWORD)p[0] << 8) | (UWORD)p[1]);
}

void NVMe_SCSI_HandleUnmap(struct NVMeBase *devBase,
                           struct NVMeUnit *unit,
                           struct IOStdReq *ioreq)
{
    struct ExecIFace *IExec   = devBase->IExec;
    struct SCSICmd   *scsi    = (struct SCSICmd *)ioreq->io_Data;
    struct NVMeController *ctrl = unit->ctrl;

    (void)IExec;

    /* Feature gate: controller must advertise DSM. */
    if (!ctrl->onc_dsm) {
        scsi->scsi_Status = 2;
        NVMe_SCSI_FillSense(scsi, NVME_SSK_ILLEGAL_REQUEST, 0x20, 0x00);
        ioreq->io_Error = HFERR_BadStatus;
        return;
    }

    /* Feature gate: unit must have its DSM range-descriptor buffer. */
    if (!unit->dsm_buf || unit->dsm_phys == 0) {
        scsi->scsi_Status = 2;
        NVMe_SCSI_FillSense(scsi, NVME_SSK_ILLEGAL_REQUEST, 0x20, 0x00);
        ioreq->io_Error = HFERR_BadStatus;
        return;
    }

    /* SPC-4 UNMAP CDB layout (10 bytes):
     *   [0] opcode = 0x42
     *   [1] bit 0 = ANCHOR (not supported here)
     *   [2-5] reserved
     *   [6] group number (ignored)
     *   [7-8] parameter list length (big-endian)
     *   [9] control */
    UBYTE *cdb = (UBYTE *)scsi->scsi_Command;
    if (cdb[1] & 0x01u) {
        scsi->scsi_Status = 2;
        NVMe_SCSI_FillSense(scsi, NVME_SSK_ILLEGAL_REQUEST, 0x24, 0x00);
        ioreq->io_Error = HFERR_BadStatus;
        return;
    }

    ULONG param_len = be16_read(cdb + 7);
    /* Zero-length UNMAP is legal and is a no-op per SPC-4 §6.14.1. */
    if (param_len == 0) {
        scsi->scsi_Status    = 0;
        scsi->scsi_Actual    = 0;
        ioreq->io_Actual     = 0;
        ioreq->io_Error      = 0;
        return;
    }

    if (!scsi->scsi_Data || scsi->scsi_Length < 8) {
        /* Parameter data absent or too short to even hold the header. */
        scsi->scsi_Status = 2;
        NVMe_SCSI_FillSense(scsi, NVME_SSK_ILLEGAL_REQUEST, 0x1A, 0x00);
        ioreq->io_Error = HFERR_BadStatus;
        return;
    }

    UBYTE *param = (UBYTE *)scsi->scsi_Data;
    /* Parameter data header:
     *   [0-1] UNMAP data length (n-2) — total header+descriptors bytes
     *   [2-3] UNMAP block descriptor data length (n)
     *   [4-7] reserved
     * followed by n/16 block descriptors. */
    ULONG bd_len = be16_read(param + 2);
    ULONG n_desc = bd_len / 16u;

    if (n_desc == 0) {
        scsi->scsi_Status    = 0;
        scsi->scsi_Actual    = 0;
        ioreq->io_Actual     = 0;
        ioreq->io_Error      = 0;
        return;
    }

    if (n_desc > NVME_DSM_MAX_RANGES) {
        /* NVMe limit.  We could split into multiple DSM commands, but
         * SPC allows the device to refuse; report ILLEGAL REQUEST /
         * INVALID FIELD IN PARAMETER LIST. */
        scsi->scsi_Status = 2;
        NVMe_SCSI_FillSense(scsi, NVME_SSK_ILLEGAL_REQUEST, 0x26, 0x00);
        ioreq->io_Error = HFERR_BadStatus;
        return;
    }

    /* Sanity — header declares n_desc descriptors but the caller's
     * data buffer must be large enough to hold 8 + n_desc*16 bytes. */
    if (scsi->scsi_Length < 8u + n_desc * 16u) {
        scsi->scsi_Status = 2;
        NVMe_SCSI_FillSense(scsi, NVME_SSK_ILLEGAL_REQUEST, 0x1A, 0x00);
        ioreq->io_Error = HFERR_BadStatus;
        return;
    }

    /* Translate SCSI UNMAP block descriptors → NVMe DSM range descriptors.
     * SCSI layout (16 bytes): LBA[0..7] big-endian, blocks[8..11] BE,
     *                         reserved[12..15].
     * NVMe layout (16 bytes): cattr[0..3] LE (0), nlb[4..7] LE,
     *                         slba[8..15] LE.
     * We write LE directly with stwbrx to account for PPC big-endian host. */
    UBYTE *src  = param + 8;
    UBYTE *dst  = (UBYTE *)unit->dsm_buf;

    for (ULONG i = 0; i < n_desc; i++) {
        uint64 slba    = be64_read(src + 0);
        ULONG  blocks  = be32_read(src + 8);

        dsm_w32_le(dst + 0,  0u);                      /* cattr = 0 */
        dsm_w32_le(dst + 4,  blocks);                  /* nlb */
        dsm_w32_le(dst + 8,  (ULONG)(slba & 0xFFFFFFFFu)); /* slba lo */
        dsm_w32_le(dst + 12, (ULONG)(slba >> 32));     /* slba hi */

        src += 16;
        dst += 16;
    }

    /* Build the DSM SQE.  PRP1 points at the pinned range-descriptor
     * page; PRP2 is zero because the data fits in one page (256 * 16
     * bytes = 4 KiB).  CDW10 NR = n_desc - 1, CDW11 attribute = AD. */
    struct nvme_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.cdw0    = NVME_CMD_DSM;                 /* cid embedded by SubmitAndWait */
    sqe.nsid    = unit->nsid;
    sqe.prp1_lo = unit->dsm_phys;
    sqe.prp1_hi = 0;
    sqe.cdw10   = (ULONG)(n_desc - 1) & 0xFFu;
    sqe.cdw11   = NVME_DSM_ATTR_AD;

    UWORD nvme_status = NVMeIO_SubmitAndWait(devBase, unit, ioreq, &sqe);
    if (nvme_status == 0) {
        scsi->scsi_Status = 0;   /* GOOD */
        scsi->scsi_Actual = 0;
        ioreq->io_Actual  = 0;
        ioreq->io_Error   = 0;
    } else {
        scsi->scsi_Status = 2;   /* CHECK CONDITION */
        NVMe_SCSI_FillSense(scsi, NVME_SSK_MEDIUM_ERROR, 0x00, 0x00);
        ioreq->io_Actual  = 0;
        if (ioreq->io_Error == 0)
            ioreq->io_Error = HFERR_BadStatus;
    }
}
