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
 *  12. CloseDevice
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

done:
    IExec->CloseDevice((struct IORequest *)ioreq);
    report(TRUE, "CloseDevice");

    IExec->FreeSysObject(ASOT_IOREQUEST, ioreq);
    IExec->FreeSysObject(ASOT_PORT, port);

    printf("----- test_nvme: %d passed, %d failed -----\n",
           g_pass_count, g_fail_count);
    return (g_fail_count == 0) ? 0 : 1;
}
