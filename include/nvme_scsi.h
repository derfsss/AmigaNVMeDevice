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

/* Shared auto-sense builder. */
void NVMe_SCSI_FillSense(struct SCSICmd *scsiCmd, UBYTE sense_key,
                         UBYTE asc, UBYTE ascq);

/* Sense keys referenced from scsi_cmds/ and unit_task.c. */
#define NVME_SSK_ILLEGAL_REQUEST  0x05
#define NVME_SSK_NO_SENSE         0x00

#endif /* NVME_SCSI_H */
