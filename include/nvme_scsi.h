#ifndef NVME_SCSI_H
#define NVME_SCSI_H

/*
 * nvme_scsi.h — prototypes for the per-opcode HD_SCSICMD synthesis
 * helpers that live in src/scsi_cmds/.
 *
 * The driver fields HD_SCSICMD via `handle_scsi_cmd()` in unit_task.c,
 * which dispatches to these helpers for the opcodes an end-user tool
 * (AmigaDiskBench, SMARTCtl, HDToolbox, Media Toolbox) is likely to
 * probe.  Each helper fills scsi_Data / scsi_Status / io_Error in
 * place; it returns nothing.
 */

#include <devices/scsidisk.h>

#include "nvme_device.h"

/* ATA PASS-THROUGH (16-byte CDB opcode 0x85; 12-byte 0xA1).
 * Used by SMART tools to send ATA SMART (0xB0) commands through a
 * SCSI transport.  We translate the per-controller NVMe SMART cache
 * (populated by NVMe_RefreshSMART behind ENABLE_SMART) into the
 * ATA/ATAPI-8 SMART Read Data layout so AmigaDiskBench's SMART tab
 * lights up with real NVMe telemetry. */
void NVMe_SCSI_HandleATAPassthrough(struct NVMeBase *devBase,
                                    struct NVMeUnit *unit,
                                    struct IOStdReq *ioreq);

/* LOG SENSE (CDB opcode 0x4D).
 * We answer page 0x00 (supported pages) and 0x2F (Informational
 * Exceptions) so health-reporting tools see a positive response.
 * Page 0x2F status flips to a warning if the NVMe SMART critical-
 * warning byte is non-zero. */
void NVMe_SCSI_HandleLogSense(struct NVMeBase *devBase,
                              struct NVMeUnit *unit,
                              struct IOStdReq *ioreq);

/* UNMAP (CDB opcode 0x42).
 * Translates the SCSI UNMAP parameter list (a header + up to 64 block
 * descriptors, each {LBA, blocks}) into an NVMe Dataset Management
 * command with the Deallocate attribute set — the NVMe equivalent of
 * TRIM.  Blocks the unit task on the I/O queue via NVMeIO_SubmitAndWait
 * until the controller replies, then fills scsi_Status.
 *
 * Refuses with CHECK CONDITION / ILLEGAL REQUEST if the controller's
 * Identify Controller ONCS did not advertise DSM support, or if the
 * unit's per-unit DSM range-descriptor buffer failed to allocate at
 * startup. */
void NVMe_SCSI_HandleUnmap(struct NVMeBase *devBase,
                           struct NVMeUnit *unit,
                           struct IOStdReq *ioreq);

/* MODE SENSE (CDB 0x1A / 0x5A) and MODE SELECT (CDB 0x15 / 0x55) —
 * page 0x08 (Caching Mode Page).  The only field we care about is the
 * WCE (Write Cache Enable) bit, which maps to NVMe Feature 0x06
 * (Volatile Write Cache).  Other mode pages are not advertised;
 * callers asking for them get an empty response of the right shape. */
void NVMe_SCSI_HandleModeSense(struct NVMeBase *devBase,
                               struct NVMeUnit *unit,
                               struct IOStdReq *ioreq);
void NVMe_SCSI_HandleModeSelect(struct NVMeBase *devBase,
                                struct NVMeUnit *unit,
                                struct IOStdReq *ioreq);

/* Shared auto-sense builder. */
void NVMe_SCSI_FillSense(struct SCSICmd *scsiCmd, UBYTE sense_key,
                         UBYTE asc, UBYTE ascq);

/* Sense keys referenced from scsi_cmds/ and unit_task.c. */
#define NVME_SSK_NO_SENSE         0x00
#define NVME_SSK_MEDIUM_ERROR     0x03
#define NVME_SSK_ILLEGAL_REQUEST  0x05

#endif /* NVME_SCSI_H */
