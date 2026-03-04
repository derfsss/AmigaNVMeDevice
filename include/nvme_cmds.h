#ifndef NVME_CMDS_H
#define NVME_CMDS_H

/*
 * nvme_cmds.h — AmigaOS trackdisk / NSD command set supported by nvme.device
 *
 * This header documents the commands dispatched in BeginIO.c.
 * No NVMe-specific custom commands are defined yet; all I/O goes through
 * the standard trackdisk and NSD interfaces.
 */

#include <devices/trackdisk.h>
#include <devices/newstyle.h>

/* Commands replied inline (no unit task needed):
 *   CMD_START, CMD_STOP, TD_MOTOR        — fixed media, motor always on
 *   TD_SEEK, TD_EJECT, CMD_CLEAR         — no-ops for fixed media
 *   TD_CHANGENUM                         — always 0 (NVMe has no media change)
 *   TD_CHANGESTATE                       — always 0 (disk present)
 *   TD_PROTSTATUS                        — always 0 (not write-protected)
 *   TD_GETDRIVETYPE                      — DRIVE_NEWSTYLE (0x44)
 *   TD_GETNUMTRACKS                      — 0 (not applicable)
 *   NSCMD_DEVICEQUERY                    — NSD capabilities query
 *
 * Commands held (never replied by driver; held until explicitly removed):
 *   TD_ADDCHANGEINT                      — NVMe has no disk change; held forever
 *   TD_REMOVE                            — held forever
 *   TD_REMCHANGEINT                      — replies the held ADDCHANGEINT request
 *
 * Commands queued to unit task:
 *   CMD_READ, CMD_WRITE, CMD_UPDATE      — standard block I/O + flush
 *   TD_READ64, TD_WRITE64                — 64-bit offset variants
 *   NSCMD_TD_READ64, NSCMD_TD_WRITE64   — NSD 64-bit variants
 *   TD_GETGEOMETRY                       — returns DriveGeometry struct
 *   HD_SCSICMD                           — returns IOERR_NOCMD (NVMe != SCSI)
 */

#endif /* NVME_CMDS_H */
