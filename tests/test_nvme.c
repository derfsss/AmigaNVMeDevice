/*
 * test_nvme.c — Functional test program for nvme.device.
 *
 * Usage from AmigaOS shell:   test_nvme [unit]
 *   unit  — unit number to open (default 0).  With the multi-controller
 *           driver, units are numbered flat across controllers so e.g.
 *           unit 0 is controller 0's first namespace.
 *
 * Tests performed, in order:
 *
 *   1. Identification banner + OpenDevice
 *   2. NSCMD_DEVICEQUERY  — list supported commands + device type
 *   3. TD_GETGEOMETRY     — synthetic CHS geometry
 *   4. NSCMD_TD_GETGEOMETRY64 — full 64-bit geometry
 *   5. HD_SCSICMD INQUIRY — vendor/product/revision strings
 *   6. CMD_READ block 0
 *   7. CMD_WRITE + CMD_UPDATE + CMD_READ  — 512-byte single-block verify
 *   8. 64 KiB bounce-buffer round-trip verify at offset 4 KiB
 *   9. TD_READ64 at a high offset (> 4 GiB into the namespace, if the
 *      namespace is large enough)
 *  10. 6 MiB round-trip verify — exceeds the 2 MiB MDTS cap, so the
 *      driver's chunked (>MDTS) submission path runs (3 chunks)
 *  11. Alignment rejection — block-misaligned length and offset must
 *      fail cleanly with an error, not truncate silently
 *  12. SCSI READ CAPACITY(10) + (16) — consistency with the 64-bit
 *      geometry; on >2 TiB namespaces RC10 must clamp to 0xFFFFFFFF
 *  13. SCSI SYNCHRONIZE CACHE(10) — NVMe Flush translation
 *  14. SCSI MODE SENSE/SELECT page 0x08 — write-cache state read and
 *      toggle (NVMe Set Features 0x06), restored afterwards
 *  15. SCSI LOG SENSE — pages 0x00 (supported) and 0x2F (informational
 *      exceptions)
 *  16. SCSI ATA PASS-THROUGH(16) SMART READ DATA / READ THRESHOLDS —
 *      ATA attribute table synthesized from the NVMe health log
 *  17. SCSI UNMAP — NVMe Dataset Management (TRIM) on 8 scratch blocks
 *  18. (>2 TiB namespaces only) TD_WRITE64/TD_READ64 round-trip at an
 *      offset beyond 2 TiB
 *  19. CloseDevice
 *
 * Built with -lauto so the clib4 CRT is linked and IExec/IDOS are
 * auto-opened.  All I/O uses IExec->DoIO so failures are synchronous.
 */

#include <exec/exec.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/errors.h>
#include <devices/trackdisk.h>
#include <devices/newstyle.h>
#include <devices/scsidisk.h>
#include <libraries/mounter.h>    /* DriveGeometry64 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef BUILD_DATE
#define BUILD_DATE "unknown"
#endif
#ifndef BUILD_TIME
#define BUILD_TIME "unknown"
#endif

#define DEVNAME "nvme.device"

/* NSCMD_TD_GETGEOMETRY64 may not be in every SDK release. */
#ifndef NSCMD_TD_GETGEOMETRY64
#define NSCMD_TD_GETGEOMETRY64  0xA004
#endif

static int g_fail_count = 0;
static int g_pass_count = 0;

static void report(BOOL pass, const char *fmt, ...)
{
    va_list ap;
    if (pass) g_pass_count++; else g_fail_count++;
    printf("%s: ", pass ? "PASS" : "FAIL");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* HD_SCSICMD helper — issue one CDB, return DoIO result.              */
/* On return: *status_out = scsi_Status, *actual_out = scsi_Actual.    */
/* `flags` is SCSIF_READ for data-in, 0 for data-out / no data.        */
/* ------------------------------------------------------------------ */
static LONG do_scsi(struct IOStdReq *ioreq, UBYTE *cdb, ULONG cdb_len,
                    APTR data, ULONG data_len, UBYTE flags,
                    UBYTE *status_out, ULONG *actual_out)
{
    static UBYTE sense[20];
    struct SCSICmd scsi;
    memset(&scsi, 0, sizeof(scsi));
    memset(sense, 0, sizeof(sense));
    scsi.scsi_Data        = (UWORD *)data;
    scsi.scsi_Length      = data_len;
    scsi.scsi_Command     = cdb;
    scsi.scsi_CmdLength   = (UWORD)cdb_len;
    scsi.scsi_SenseData   = sense;
    scsi.scsi_SenseLength = sizeof(sense) - 2;
    scsi.scsi_Flags       = SCSIF_AUTOSENSE | flags;

    ioreq->io_Command = HD_SCSICMD;
    ioreq->io_Data    = &scsi;
    ioreq->io_Length  = sizeof(scsi);
    LONG rc = IExec->DoIO((struct IORequest *)ioreq);

    if (status_out) *status_out = scsi.scsi_Status;
    if (actual_out) *actual_out = scsi.scsi_Actual;
    return rc;
}

int main(int argc, char *argv[])
{
    ULONG unit = 0;
    if (argc > 1)
        unit = (ULONG)strtol(argv[1], NULL, 10);

    printf("test_nvme built %s %s — opening %s unit %lu\n",
           BUILD_DATE, BUILD_TIME, DEVNAME, unit);

    struct MsgPort *port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_DONE);
    if (!port) {
        printf("FAIL: could not create message port\n");
        return 1;
    }

    struct IOStdReq *ioreq = (struct IOStdReq *)
        IExec->AllocSysObjectTags(ASOT_IOREQUEST,
            ASOIOR_Size,      sizeof(struct IOStdReq),
            ASOIOR_ReplyPort, port,
            TAG_DONE);
    if (!ioreq) {
        printf("FAIL: could not create ioreq\n");
        IExec->FreeSysObject(ASOT_PORT, port);
        return 1;
    }

    /* -------------------------------------------------------------- */
    /* 1. OpenDevice                                                   */
    /* -------------------------------------------------------------- */
    LONG err = IExec->OpenDevice(DEVNAME, unit, (struct IORequest *)ioreq, 0);
    if (err != 0) {
        printf("FAIL: OpenDevice returned %ld\n", err);
        IExec->FreeSysObject(ASOT_IOREQUEST, ioreq);
        IExec->FreeSysObject(ASOT_PORT, port);
        return 1;
    }
    report(TRUE, "OpenDevice unit %lu", unit);

    /* -------------------------------------------------------------- */
    /* 2. NSCMD_DEVICEQUERY                                            */
    /* -------------------------------------------------------------- */
    struct NSDeviceQueryResult qr;
    memset(&qr, 0, sizeof(qr));
    ioreq->io_Command = NSCMD_DEVICEQUERY;
    ioreq->io_Data    = &qr;
    ioreq->io_Length  = sizeof(qr);
    if (IExec->DoIO((struct IORequest *)ioreq) == 0) {
        report(TRUE, "NSCMD_DEVICEQUERY — DeviceType=%lu", (ULONG)qr.DeviceType);
        if (qr.SupportedCommands) {
            printf("      Supported:");
            for (UWORD *cmd = qr.SupportedCommands; *cmd; cmd++)
                printf(" %u", *cmd);
            printf("\n");
        }
    } else {
        report(FALSE, "NSCMD_DEVICEQUERY io_Error=%d", ioreq->io_Error);
    }

    /* -------------------------------------------------------------- */
    /* 3. TD_GETGEOMETRY (synthetic CHS)                               */
    /* -------------------------------------------------------------- */
    struct DriveGeometry dg;
    memset(&dg, 0, sizeof(dg));
    ioreq->io_Command = TD_GETGEOMETRY;
    ioreq->io_Data    = &dg;
    ioreq->io_Length  = sizeof(dg);
    uint64 total_bytes_32 = 0;
    if (IExec->DoIO((struct IORequest *)ioreq) == 0) {
        total_bytes_32 = (uint64)dg.dg_TotalSectors * dg.dg_SectorSize;
        report(TRUE, "TD_GETGEOMETRY  sectors=%lu size=%lu"
                     " (~%lu MiB using 32-bit total)",
               dg.dg_TotalSectors, dg.dg_SectorSize,
               (ULONG)(total_bytes_32 / (1024u*1024u)));
    } else {
        report(FALSE, "TD_GETGEOMETRY io_Error=%d", ioreq->io_Error);
    }

    /* -------------------------------------------------------------- */
    /* 4. NSCMD_TD_GETGEOMETRY64 (full 64-bit)                         */
    /* -------------------------------------------------------------- */
    struct DriveGeometry64 dg64;
    memset(&dg64, 0, sizeof(dg64));
    ioreq->io_Command = NSCMD_TD_GETGEOMETRY64;
    ioreq->io_Data    = &dg64;
    ioreq->io_Length  = sizeof(dg64);
    uint64 true_total_bytes = 0;
    uint32 true_sector_size = 512;
    if (IExec->DoIO((struct IORequest *)ioreq) == 0) {
        true_total_bytes = dg64.dg_TotalSectors * dg64.dg_SectorSize;
        true_sector_size = dg64.dg_SectorSize;
        report(TRUE, "NSCMD_TD_GETGEOMETRY64 sectors=(hi:%lu lo:%lu) size=%lu"
                     " → (hi:%lu lo:%lu) bytes",
               (ULONG)(dg64.dg_TotalSectors >> 32),
               (ULONG)(dg64.dg_TotalSectors & 0xFFFFFFFFu),
               (ULONG)dg64.dg_SectorSize,
               (ULONG)(true_total_bytes >> 32),
               (ULONG)(true_total_bytes & 0xFFFFFFFFu));
    } else {
        report(FALSE, "NSCMD_TD_GETGEOMETRY64 io_Error=%d", ioreq->io_Error);
    }

    /* -------------------------------------------------------------- */
    /* 5. HD_SCSICMD INQUIRY                                           */
    /* -------------------------------------------------------------- */
    {
        UBYTE inqbuf[96];
        UBYTE sense[18];
        UBYTE cdb[6] = { 0x12, 0x00, 0x00, 0x00, sizeof(inqbuf), 0x00 };
        struct SCSICmd scsi;
        memset(&scsi, 0, sizeof(scsi));
        memset(inqbuf, 0, sizeof(inqbuf));
        scsi.scsi_Data        = (UWORD *)inqbuf;
        scsi.scsi_Length      = sizeof(inqbuf);
        scsi.scsi_Command     = cdb;
        scsi.scsi_CmdLength   = sizeof(cdb);
        scsi.scsi_SenseData   = sense;
        scsi.scsi_SenseLength = sizeof(sense);
        scsi.scsi_Flags       = SCSIF_AUTOSENSE | SCSIF_READ;

        ioreq->io_Command = HD_SCSICMD;
        ioreq->io_Data    = &scsi;
        ioreq->io_Length  = sizeof(scsi);
        if (IExec->DoIO((struct IORequest *)ioreq) == 0) {
            char vendor[9], product[17], revision[5];
            memcpy(vendor,   inqbuf + 8,  8); vendor[8]    = '\0';
            memcpy(product,  inqbuf + 16, 16); product[16] = '\0';
            memcpy(revision, inqbuf + 32, 4); revision[4]  = '\0';
            report(TRUE, "HD_SCSICMD INQUIRY vendor=\"%s\" product=\"%s\" rev=\"%s\"",
                   vendor, product, revision);
        } else {
            report(FALSE, "HD_SCSICMD INQUIRY io_Error=%d", ioreq->io_Error);
        }
    }

    /* Single-block buffer sized from the REAL sector size reported by
     * geometry — a 4K-native namespace would reject 512-byte transfers
     * as block-misaligned (and rightly so). */
    ULONG blk = true_sector_size;
    UBYTE *buf = IExec->AllocVecTags(blk,
        AVT_Type,      MEMF_SHARED,
        AVT_Alignment, blk,
        AVT_Clear,     0,
        TAG_DONE);
    if (!buf) {
        report(FALSE, "AllocVecTags %lu", blk);
        goto done;
    }

    /* -------------------------------------------------------------- */
    /* 6. CMD_READ block 0                                             */
    /* -------------------------------------------------------------- */
    ioreq->io_Command = CMD_READ;
    ioreq->io_Data    = buf;
    ioreq->io_Length  = blk;
    ioreq->io_Offset  = 0;
    if (IExec->DoIO((struct IORequest *)ioreq) == 0) {
        report(TRUE, "CMD_READ block 0  — bytes: %02X %02X %02X %02X",
               buf[0], buf[1], buf[2], buf[3]);
    } else {
        report(FALSE, "CMD_READ block 0 io_Error=%d", ioreq->io_Error);
    }

    /* -------------------------------------------------------------- */
    /* 7. Single-block write + flush + read-back verify                */
    /* -------------------------------------------------------------- */
    memset(buf, 0xA5, blk);
    ioreq->io_Command = CMD_WRITE;
    ioreq->io_Data    = buf;
    ioreq->io_Length  = blk;
    ioreq->io_Offset  = blk;   /* block 1 — keeps MBR intact */
    if (IExec->DoIO((struct IORequest *)ioreq) == 0) {
        report(TRUE, "CMD_WRITE block 1 pattern 0xA5");

        ioreq->io_Command = CMD_UPDATE;
        ioreq->io_Data    = NULL;
        ioreq->io_Length  = 0;
        ioreq->io_Offset  = 0;
        if (IExec->DoIO((struct IORequest *)ioreq) == 0)
            report(TRUE, "CMD_UPDATE (Flush)");
        else
            printf("WARN: CMD_UPDATE io_Error=%d (may not be supported)\n",
                   ioreq->io_Error);

        memset(buf, 0, blk);
        ioreq->io_Command = CMD_READ;
        ioreq->io_Data    = buf;
        ioreq->io_Length  = blk;
        ioreq->io_Offset  = blk;
        if (IExec->DoIO((struct IORequest *)ioreq) == 0) {
            BOOL ok = TRUE;
            for (ULONG i = 0; i < blk; i++)
                if (buf[i] != 0xA5) { ok = FALSE; break; }
            report(ok, "Block 1 readback verify");
        } else {
            report(FALSE, "Readback CMD_READ io_Error=%d", ioreq->io_Error);
        }
    } else {
        report(FALSE, "CMD_WRITE block 1 io_Error=%d", ioreq->io_Error);
    }

    IExec->FreeVec(buf);

    /* -------------------------------------------------------------- */
    /* 8. 64 KiB bounce-buffer round-trip verify at offset 4 KiB       */
    /* -------------------------------------------------------------- */
    {
        ULONG big_len = 64u * 1024u;
        UBYTE *bigbuf = IExec->AllocVecTags(big_len,
            AVT_Type,      MEMF_SHARED,
            AVT_Alignment, 4096,
            AVT_Clear,     0,
            TAG_DONE);
        if (!bigbuf) {
            report(FALSE, "AllocVecTags 64 KiB");
        } else {
            /* Fill with a pseudo-random pattern keyed by byte index so
             * any misalignment is caught. */
            for (ULONG i = 0; i < big_len; i++)
                bigbuf[i] = (UBYTE)((i * 131u + 7u) & 0xFF);

            ioreq->io_Command = CMD_WRITE;
            ioreq->io_Data    = bigbuf;
            ioreq->io_Length  = big_len;
            ioreq->io_Offset  = 4096;
            if (IExec->DoIO((struct IORequest *)ioreq) == 0) {
                ioreq->io_Command = CMD_UPDATE;
                ioreq->io_Data    = NULL;
                ioreq->io_Length  = 0;
                ioreq->io_Offset  = 0;
                (void)IExec->DoIO((struct IORequest *)ioreq);

                memset(bigbuf, 0, big_len);
                ioreq->io_Command = CMD_READ;
                ioreq->io_Data    = bigbuf;
                ioreq->io_Length  = big_len;
                ioreq->io_Offset  = 4096;
                if (IExec->DoIO((struct IORequest *)ioreq) == 0) {
                    BOOL ok = TRUE;
                    ULONG bad_at = 0;
                    for (ULONG i = 0; i < big_len; i++) {
                        UBYTE expected = (UBYTE)((i * 131u + 7u) & 0xFF);
                        if (bigbuf[i] != expected) {
                            ok = FALSE;
                            bad_at = i;
                            break;
                        }
                    }
                    if (ok)
                        report(TRUE, "64 KiB write/read round-trip verify");
                    else
                        report(FALSE, "64 KiB verify mismatch at offset %lu"
                                      " (got 0x%02X)",
                               bad_at, bigbuf[bad_at]);
                } else {
                    report(FALSE, "64 KiB CMD_READ io_Error=%d", ioreq->io_Error);
                }
            } else {
                report(FALSE, "64 KiB CMD_WRITE io_Error=%d", ioreq->io_Error);
            }
            IExec->FreeVec(bigbuf);
        }
    }

    /* -------------------------------------------------------------- */
    /* 9. TD_READ64 at high offset (>4 GiB)                            */
    /* -------------------------------------------------------------- */
    /* Needs the namespace to extend past 4 GiB so the byte offset's
     * high 32 bits are non-zero — that's what makes this a true 64-bit
     * offset test.  (An earlier 5-GiB threshold silently skipped this
     * on the standard 5 × 10^9-byte test image.) */
    if (true_total_bytes > 0x100000000ULL + (uint64)true_sector_size) {
        /* Pick an offset near the end of the device, aligned on a
         * sector boundary.  Bytes 512 just before total give us a
         * sector that exists and can be read safely. */
        uint64 hi_offset = (true_total_bytes & ~((uint64)true_sector_size - 1))
                         - (uint64)true_sector_size;
        UBYTE *hbuf = IExec->AllocVecTags(true_sector_size,
            AVT_Type,      MEMF_SHARED,
            AVT_Alignment, true_sector_size,
            AVT_Clear,     0,
            TAG_DONE);
        if (!hbuf) {
            report(FALSE, "AllocVecTags for TD_READ64");
        } else {
            ioreq->io_Command = TD_READ64;
            ioreq->io_Data    = hbuf;
            ioreq->io_Length  = true_sector_size;
            ioreq->io_Offset  = (ULONG)(hi_offset & 0xFFFFFFFFu);
            ioreq->io_Actual  = (ULONG)(hi_offset >> 32);
            if (IExec->DoIO((struct IORequest *)ioreq) == 0) {
                report(TRUE, "TD_READ64 @ offset (hi:%lu lo:%lu)  last sector",
                       (ULONG)(hi_offset >> 32),
                       (ULONG)(hi_offset & 0xFFFFFFFFu));
            } else {
                report(FALSE, "TD_READ64 io_Error=%d", ioreq->io_Error);
            }
            IExec->FreeVec(hbuf);
        }
    } else {
        printf("SKIP: TD_READ64 high-offset (namespace does not extend past 4 GiB)\n");
    }

    /* -------------------------------------------------------------- */
    /* 10. 6 MiB round-trip — forces the chunked (>MDTS) path          */
    /* -------------------------------------------------------------- */
    {
        ULONG huge_len = 6u * 1024u * 1024u;   /* 3 chunks at 2 MiB MDTS */
        ULONG huge_off = 1024u * 1024u;        /* clear of earlier tests */
        UBYTE *hugebuf = IExec->AllocVecTags(huge_len,
            AVT_Type,      MEMF_SHARED,
            AVT_Alignment, 4096,
            AVT_Clear,     0,
            TAG_DONE);
        if (!hugebuf) {
            report(FALSE, "AllocVecTags 6 MiB");
        } else {
            /* Index-keyed pattern with a stride coprime to the chunk
             * size, so a swapped / repeated / skipped chunk shows up
             * as a mismatch at the first wrong byte. */
            for (ULONG i = 0; i < huge_len; i++)
                hugebuf[i] = (UBYTE)((i * 197u + i / 65536u + 13u) & 0xFF);

            ioreq->io_Command = CMD_WRITE;
            ioreq->io_Data    = hugebuf;
            ioreq->io_Length  = huge_len;
            ioreq->io_Offset  = huge_off;
            if (IExec->DoIO((struct IORequest *)ioreq) == 0) {
                BOOL len_ok = (ioreq->io_Actual == huge_len);

                ioreq->io_Command = CMD_UPDATE;
                ioreq->io_Data    = NULL;
                ioreq->io_Length  = 0;
                ioreq->io_Offset  = 0;
                (void)IExec->DoIO((struct IORequest *)ioreq);

                memset(hugebuf, 0, huge_len);
                ioreq->io_Command = CMD_READ;
                ioreq->io_Data    = hugebuf;
                ioreq->io_Length  = huge_len;
                ioreq->io_Offset  = huge_off;
                if (IExec->DoIO((struct IORequest *)ioreq) == 0) {
                    len_ok = len_ok && (ioreq->io_Actual == huge_len);
                    BOOL ok = TRUE;
                    ULONG bad_at = 0;
                    for (ULONG i = 0; i < huge_len; i++) {
                        UBYTE expected =
                            (UBYTE)((i * 197u + i / 65536u + 13u) & 0xFF);
                        if (hugebuf[i] != expected) {
                            ok = FALSE;
                            bad_at = i;
                            break;
                        }
                    }
                    if (ok && len_ok)
                        report(TRUE, "6 MiB chunked (>MDTS) round-trip verify");
                    else if (!ok)
                        report(FALSE, "6 MiB verify mismatch at offset %lu"
                                      " (got 0x%02X)",
                               bad_at, hugebuf[bad_at]);
                    else
                        report(FALSE, "6 MiB io_Actual=%lu expected %lu",
                               ioreq->io_Actual, huge_len);
                } else {
                    report(FALSE, "6 MiB CMD_READ io_Error=%d", ioreq->io_Error);
                }
            } else {
                report(FALSE, "6 MiB CMD_WRITE io_Error=%d", ioreq->io_Error);
            }
            IExec->FreeVec(hugebuf);
        }
    }

    /* -------------------------------------------------------------- */
    /* 11. Alignment rejection — misaligned length / offset must fail  */
    /* -------------------------------------------------------------- */
    {
        UBYTE *abuf = IExec->AllocVecTags(true_sector_size * 2,
            AVT_Type,      MEMF_SHARED,
            AVT_Alignment, true_sector_size,
            AVT_Clear,     0,
            TAG_DONE);
        if (!abuf) {
            report(FALSE, "AllocVecTags for alignment tests");
        } else {
            /* Length that is not a multiple of the block size. */
            ioreq->io_Command = CMD_READ;
            ioreq->io_Data    = abuf;
            ioreq->io_Length  = true_sector_size + 7;
            ioreq->io_Offset  = 0;
            LONG rc = IExec->DoIO((struct IORequest *)ioreq);
            report(rc != 0 && ioreq->io_Error != 0,
                   "Misaligned length rejected (io_Error=%d)", ioreq->io_Error);

            /* Offset that is not on a block boundary. */
            ioreq->io_Command = CMD_READ;
            ioreq->io_Data    = abuf;
            ioreq->io_Length  = true_sector_size;
            ioreq->io_Offset  = true_sector_size + 1;
            rc = IExec->DoIO((struct IORequest *)ioreq);
            report(rc != 0 && ioreq->io_Error != 0,
                   "Misaligned offset rejected (io_Error=%d)", ioreq->io_Error);

            IExec->FreeVec(abuf);
        }
    }

    /* -------------------------------------------------------------- */
    /* 12. SCSI READ CAPACITY(10) + (16) vs 64-bit geometry            */
    /* -------------------------------------------------------------- */
    {
        UBYTE st;
        ULONG act;

        UBYTE rc10[8];
        UBYTE cdb10[10];
        memset(cdb10, 0, sizeof(cdb10));
        cdb10[0] = 0x25;
        memset(rc10, 0, sizeof(rc10));
        LONG rc = do_scsi(ioreq, cdb10, 10, rc10, sizeof(rc10),
                          SCSIF_READ, &st, &act);
        if (rc == 0 && st == 0 && act == 8) {
            ULONG last10 = ((ULONG)rc10[0] << 24) | ((ULONG)rc10[1] << 16)
                         | ((ULONG)rc10[2] << 8)  |  (ULONG)rc10[3];
            ULONG bs10   = ((ULONG)rc10[4] << 24) | ((ULONG)rc10[5] << 16)
                         | ((ULONG)rc10[6] << 8)  |  (ULONG)rc10[7];
            uint64 total = dg64.dg_TotalSectors;
            BOOL ok;
            if (total > 0xFFFFFFFFULL)
                ok = (last10 == 0xFFFFFFFFu);   /* >2 TiB must clamp */
            else
                ok = (last10 == (ULONG)(total - 1));
            ok = ok && (bs10 == true_sector_size);
            report(ok, "SCSI READ CAPACITY(10) last_lba=0x%08lx bs=%lu%s",
                   last10, bs10,
                   (total > 0xFFFFFFFFULL) ? " (clamped, >2 TiB)" : "");
        } else {
            report(FALSE, "SCSI READ CAPACITY(10) rc=%ld status=%u", rc, st);
        }

        UBYTE rc16[32];
        UBYTE cdb16[16];
        memset(cdb16, 0, sizeof(cdb16));
        cdb16[0] = 0x9E;
        cdb16[1] = 0x10;            /* service action: READ CAPACITY */
        cdb16[13] = 32;             /* allocation length */
        memset(rc16, 0, sizeof(rc16));
        rc = do_scsi(ioreq, cdb16, 16, rc16, sizeof(rc16),
                     SCSIF_READ, &st, &act);
        if (rc == 0 && st == 0 && act == 32) {
            uint64 last16 = 0;
            for (int i = 0; i < 8; i++)
                last16 = (last16 << 8) | rc16[i];
            ULONG bs16 = ((ULONG)rc16[8] << 24) | ((ULONG)rc16[9] << 16)
                       | ((ULONG)rc16[10] << 8) |  (ULONG)rc16[11];
            BOOL ok = (last16 == dg64.dg_TotalSectors - 1)
                   && (bs16 == true_sector_size);
            report(ok, "SCSI READ CAPACITY(16) last_lba=(hi:%lu lo:%lu) bs=%lu",
                   (ULONG)(last16 >> 32), (ULONG)(last16 & 0xFFFFFFFFu), bs16);
        } else {
            report(FALSE, "SCSI READ CAPACITY(16) rc=%ld status=%u", rc, st);
        }
    }

    /* -------------------------------------------------------------- */
    /* 13. SCSI SYNCHRONIZE CACHE(10) — NVMe Flush translation         */
    /* -------------------------------------------------------------- */
    {
        UBYTE st;
        UBYTE cdb[10];
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x35;
        LONG rc = do_scsi(ioreq, cdb, 10, NULL, 0, 0, &st, NULL);
        report(rc == 0 && st == 0,
               "SCSI SYNCHRONIZE CACHE(10) rc=%ld status=%u", rc, st);
    }

    /* -------------------------------------------------------------- */
    /* 14. SCSI MODE SENSE/SELECT page 0x08 — write-cache toggle       */
    /* -------------------------------------------------------------- */
    {
        UBYTE st;
        ULONG act;
        UBYTE sense_cdb[6] = { 0x1A, 0, 0x08, 0, 32, 0 };
        UBYTE page[32];

        memset(page, 0, sizeof(page));
        LONG rc = do_scsi(ioreq, sense_cdb, 6, page, sizeof(page),
                          SCSIF_READ, &st, &act);
        if (rc == 0 && st == 0 && act >= 4 + 3 &&
            (page[4] & 0x3F) == 0x08) {
            BOOL wce_initial = (page[6] & 0x04) != 0;
            report(TRUE, "SCSI MODE SENSE(6) page 0x08 — WCE=%d", wce_initial);

            /* Toggle WCE via MODE SELECT(6): 4-byte header + 20-byte
             * caching page. */
            UBYTE param[24];
            memset(param, 0, sizeof(param));
            param[4] = 0x08;        /* page code */
            param[5] = 0x12;        /* page length */
            param[6] = wce_initial ? 0x00 : 0x04;
            UBYTE select_cdb[6] = { 0x15, 0x10, 0, 0, sizeof(param), 0 };
            rc = do_scsi(ioreq, select_cdb, 6, param, sizeof(param),
                         0, &st, NULL);
            BOOL select_ok = (rc == 0 && st == 0);

            /* Read back — if the controller has a volatile write cache
             * the state must have flipped; if not, the driver accepts
             * the select as a no-op and the state stays put. */
            memset(page, 0, sizeof(page));
            rc = do_scsi(ioreq, sense_cdb, 6, page, sizeof(page),
                         SCSIF_READ, &st, &act);
            BOOL wce_after = (page[6] & 0x04) != 0;
            report(select_ok && rc == 0 && st == 0,
                   "SCSI MODE SELECT(6) WCE %d -> %d%s",
                   wce_initial, wce_after,
                   (wce_after == wce_initial) ? " (no VWC — accepted as no-op)"
                                              : " (toggled)");

            /* Restore the initial state. */
            param[6] = wce_initial ? 0x04 : 0x00;
            (void)do_scsi(ioreq, select_cdb, 6, param, sizeof(param),
                          0, &st, NULL);
        } else {
            report(FALSE, "SCSI MODE SENSE(6) page 0x08 rc=%ld status=%u"
                          " actual=%lu", rc, st, act);
        }
    }

    /* -------------------------------------------------------------- */
    /* 15. SCSI LOG SENSE — pages 0x00 and 0x2F                        */
    /* -------------------------------------------------------------- */
    {
        UBYTE st;
        ULONG act;
        UBYTE buf0[16];
        UBYTE cdb[10];
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x4D;
        cdb[2] = 0x00;              /* page 0x00 — supported pages */
        cdb[8] = sizeof(buf0);
        memset(buf0, 0, sizeof(buf0));
        LONG rc = do_scsi(ioreq, cdb, 10, buf0, sizeof(buf0),
                          SCSIF_READ, &st, &act);
        BOOL lists_2f = FALSE;
        for (ULONG i = 4; i < act && i < sizeof(buf0); i++)
            if (buf0[i] == 0x2F) lists_2f = TRUE;
        report(rc == 0 && st == 0 && lists_2f,
               "SCSI LOG SENSE page 0x00 (lists 0x2F: %s)",
               lists_2f ? "yes" : "no");

        UBYTE buf2f[16];
        cdb[2] = 0x2F;
        cdb[8] = sizeof(buf2f);
        memset(buf2f, 0, sizeof(buf2f));
        rc = do_scsi(ioreq, cdb, 10, buf2f, sizeof(buf2f),
                     SCSIF_READ, &st, &act);
        report(rc == 0 && st == 0 && act >= 12 && (buf2f[0] & 0x3F) == 0x2F,
               "SCSI LOG SENSE page 0x2F (IE ASC=0x%02x temp=%u C)",
               buf2f[8], buf2f[10]);
    }

    /* -------------------------------------------------------------- */
    /* 16. SCSI ATA PASS-THROUGH(16) — SMART READ DATA / THRESHOLDS    */
    /* -------------------------------------------------------------- */
    {
        UBYTE st;
        ULONG act;
        UBYTE *smart = IExec->AllocVecTags(512, AVT_Type, MEMF_SHARED,
                                           AVT_Clear, 0, TAG_DONE);
        if (!smart) {
            report(FALSE, "AllocVecTags for SMART buffer");
        } else {
            UBYTE cdb[16];
            memset(cdb, 0, sizeof(cdb));
            cdb[0]  = 0x85;         /* ATA PASS-THROUGH (16) */
            cdb[4]  = 0xD0;         /* features: SMART READ DATA */
            cdb[14] = 0xB0;         /* ATA command: SMART */
            LONG rc = do_scsi(ioreq, cdb, 16, smart, 512,
                              SCSIF_READ, &st, &act);
            /* Attribute entries: 12 bytes each from offset 2; a valid
             * table has at least one non-zero attribute ID. */
            ULONG attrs = 0;
            UBYTE first_id = 0;
            for (ULONG off = 2; off + 12 <= 362; off += 12) {
                if (smart[off] != 0) {
                    if (!attrs) first_id = smart[off];
                    attrs++;
                }
            }
            report(rc == 0 && st == 0 && act == 512 && attrs > 0,
                   "SCSI ATA PASS-THROUGH SMART READ DATA — %lu attrs"
                   " (first id %u)", attrs, first_id);

            cdb[4] = 0xD1;          /* SMART READ THRESHOLDS */
            memset(smart, 0, 512);
            rc = do_scsi(ioreq, cdb, 16, smart, 512,
                         SCSIF_READ, &st, &act);
            report(rc == 0 && st == 0 && act == 512 && smart[2] != 0,
                   "SCSI ATA PASS-THROUGH SMART READ THRESHOLDS");
            IExec->FreeVec(smart);
        }
    }

    /* -------------------------------------------------------------- */
    /* 17. SCSI UNMAP — NVMe Dataset Management (TRIM)                 */
    /* -------------------------------------------------------------- */
    {
        UBYTE st;
        /* Write a pattern to 8 scratch blocks at 8 MiB, TRIM them, and
         * confirm both the UNMAP and a subsequent read succeed.  The
         * post-TRIM content is deliberately not asserted — the spec
         * leaves deallocated-read values to the device. */
        ULONG  scratch_len = 8 * true_sector_size;
        uint64 scratch_off = 8u * 1024u * 1024u;
        UBYTE *sbuf = IExec->AllocVecTags(scratch_len,
            AVT_Type, MEMF_SHARED, AVT_Alignment, 4096,
            AVT_Clear, 0, TAG_DONE);
        if (!sbuf) {
            report(FALSE, "AllocVecTags for UNMAP scratch");
        } else {
            memset(sbuf, 0x5A, scratch_len);
            ioreq->io_Command = CMD_WRITE;
            ioreq->io_Data    = sbuf;
            ioreq->io_Length  = scratch_len;
            ioreq->io_Offset  = (ULONG)scratch_off;
            LONG rc = IExec->DoIO((struct IORequest *)ioreq);
            if (rc != 0) {
                report(FALSE, "UNMAP scratch CMD_WRITE io_Error=%d",
                       ioreq->io_Error);
            } else {
                uint64 lba = scratch_off / true_sector_size;
                UBYTE param[8 + 16];
                memset(param, 0, sizeof(param));
                param[1] = sizeof(param) - 2;   /* UNMAP data length */
                param[3] = 16;                  /* block descriptor bytes */
                for (int i = 0; i < 8; i++)
                    param[8 + i] = (UBYTE)(lba >> (56 - i * 8));
                param[19] = 8;                  /* block count */
                UBYTE cdb[10];
                memset(cdb, 0, sizeof(cdb));
                cdb[0] = 0x42;
                cdb[8] = sizeof(param);         /* parameter list length */
                rc = do_scsi(ioreq, cdb, 10, param, sizeof(param),
                             0, &st, NULL);
                report(rc == 0 && st == 0,
                       "SCSI UNMAP (TRIM) 8 blocks @ LBA %lu rc=%ld status=%u",
                       (ULONG)lba, rc, st);

                memset(sbuf, 0xEE, scratch_len);
                ioreq->io_Command = CMD_READ;
                ioreq->io_Data    = sbuf;
                ioreq->io_Length  = scratch_len;
                ioreq->io_Offset  = (ULONG)scratch_off;
                rc = IExec->DoIO((struct IORequest *)ioreq);
                report(rc == 0, "Post-TRIM read of deallocated range"
                       " (first byte 0x%02X)", sbuf[0]);
            }
            IExec->FreeVec(sbuf);
        }
    }

    /* -------------------------------------------------------------- */
    /* 18. (>2 TiB only) TD_WRITE64/READ64 round-trip past 2 TiB       */
    /* -------------------------------------------------------------- */
    if (true_total_bytes > 0x20000000000ULL + (uint64)true_sector_size) {
        uint64 far_off = 0x20000000000ULL;       /* 2 TiB, block-aligned */
        UBYTE *fbuf = IExec->AllocVecTags(true_sector_size,
            AVT_Type, MEMF_SHARED, AVT_Alignment, true_sector_size,
            AVT_Clear, 0, TAG_DONE);
        if (!fbuf) {
            report(FALSE, "AllocVecTags for >2 TiB test");
        } else {
            for (ULONG i = 0; i < true_sector_size; i++)
                fbuf[i] = (UBYTE)((i * 89u + 3u) & 0xFF);
            ioreq->io_Command = TD_WRITE64;
            ioreq->io_Data    = fbuf;
            ioreq->io_Length  = true_sector_size;
            ioreq->io_Offset  = (ULONG)(far_off & 0xFFFFFFFFu);
            ioreq->io_Actual  = (ULONG)(far_off >> 32);
            if (IExec->DoIO((struct IORequest *)ioreq) == 0) {
                memset(fbuf, 0, true_sector_size);
                ioreq->io_Command = TD_READ64;
                ioreq->io_Data    = fbuf;
                ioreq->io_Length  = true_sector_size;
                ioreq->io_Offset  = (ULONG)(far_off & 0xFFFFFFFFu);
                ioreq->io_Actual  = (ULONG)(far_off >> 32);
                if (IExec->DoIO((struct IORequest *)ioreq) == 0) {
                    BOOL ok = TRUE;
                    for (ULONG i = 0; i < true_sector_size; i++)
                        if (fbuf[i] != (UBYTE)((i * 89u + 3u) & 0xFF)) {
                            ok = FALSE; break;
                        }
                    report(ok, "TD_WRITE64/READ64 round-trip at 2 TiB");
                } else {
                    report(FALSE, ">2 TiB TD_READ64 io_Error=%d",
                           ioreq->io_Error);
                }
            } else {
                report(FALSE, ">2 TiB TD_WRITE64 io_Error=%d",
                       ioreq->io_Error);
            }
            IExec->FreeVec(fbuf);
        }
    } else {
        printf("SKIP: >2 TiB round-trip (namespace too small)\n");
    }

done:
    IExec->CloseDevice((struct IORequest *)ioreq);
    report(TRUE, "CloseDevice");

    IExec->FreeSysObject(ASOT_IOREQUEST, ioreq);
    IExec->FreeSysObject(ASOT_PORT, port);

    printf("----- test_nvme: %d passed, %d failed -----\n",
           g_pass_count, g_fail_count);
    return (g_fail_count == 0) ? 0 : 1;
}
