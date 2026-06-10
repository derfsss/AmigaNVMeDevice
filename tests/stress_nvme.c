/*
 * stress_nvme.c — concurrency stress test for nvme.device.
 *
 * Usage from AmigaOS shell:   stress_nvme [unit] [rounds]
 *   unit    — unit number to open (default 0)
 *   rounds  — write/read burst rounds (default 4)
 *
 * Everything test_nvme does is synchronous DoIO — queue depth 1.  This
 * tool drives the driver the way a busy filesystem does:
 *
 *   1. NREQS (24) parallel SendIO writes — more requests than the
 *      driver's 16 inflight slots, so the pipeline saturates and the
 *      driver must apply back-pressure rather than failing
 *   2. NREQS parallel SendIO reads of the same blocks + pattern verify
 *   3. Mixed read/write interleave round
 *   4. AbortIO on queued requests — aborted requests must complete
 *      with IOERR_ABORTED or success (if already committed), never
 *      hang, and the data path must stay intact afterwards
 *
 * Every request uses its own IOStdReq (ASOIOR_Duplicate of the opened
 * master) and its own buffer / disk region, so completions can land in
 * any order.
 *
 * Exit code 0 if every check passed.
 */

#include <exec/exec.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/errors.h>
#include <devices/trackdisk.h>
#include <devices/newstyle.h>
#include <libraries/mounter.h>
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

#define DEVNAME   "nvme.device"
#define NREQS     24            /* > driver inflight slots (16) */
#define REQ_BYTES (16u * 1024u)
#define BASE_OFF  (16u * 1024u * 1024u)   /* clear of test_nvme scratch */
#define REGION    (1024u * 1024u)         /* per-request disk region */

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

static void fill_pattern(UBYTE *buf, ULONG len, ULONG seed)
{
    for (ULONG i = 0; i < len; i++)
        buf[i] = (UBYTE)((i * 31u + seed * 7u + 11u) & 0xFF);
}

static LONG check_pattern(const UBYTE *buf, ULONG len, ULONG seed)
{
    for (ULONG i = 0; i < len; i++)
        if (buf[i] != (UBYTE)((i * 31u + seed * 7u + 11u) & 0xFF))
            return (LONG)i;
    return -1;
}

int main(int argc, char *argv[])
{
    ULONG unit   = 0;
    ULONG rounds = 4;
    if (argc > 1) unit   = (ULONG)strtol(argv[1], NULL, 10);
    if (argc > 2) rounds = (ULONG)strtol(argv[2], NULL, 10);

    printf("stress_nvme built %s %s — unit %lu, %lu rounds, %u requests"
           " x %lu KiB\n", BUILD_DATE, BUILD_TIME, unit, rounds,
           NREQS, (ULONG)(REQ_BYTES / 1024u));

    struct MsgPort *port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_DONE);
    if (!port) { printf("FAIL: no message port\n"); return 1; }

    struct IOStdReq *master = (struct IOStdReq *)
        IExec->AllocSysObjectTags(ASOT_IOREQUEST,
            ASOIOR_Size,      sizeof(struct IOStdReq),
            ASOIOR_ReplyPort, port,
            TAG_DONE);
    if (!master) { printf("FAIL: no master ioreq\n"); return 1; }

    if (IExec->OpenDevice(DEVNAME, unit, (struct IORequest *)master, 0) != 0) {
        printf("FAIL: OpenDevice unit %lu\n", unit);
        return 1;
    }
    report(TRUE, "OpenDevice unit %lu", unit);

    /* Per-request resources.  One shared reply port keeps WaitIO/GetMsg
     * semantics simple; requests are matched by pointer. */
    struct IOStdReq *req[NREQS];
    UBYTE           *buf[NREQS];
    BOOL             ok_setup = TRUE;
    memset(req, 0, sizeof(req));
    memset(buf, 0, sizeof(buf));
    for (int i = 0; i < NREQS; i++) {
        req[i] = (struct IOStdReq *)IExec->AllocSysObjectTags(ASOT_IOREQUEST,
            ASOIOR_Size,      sizeof(struct IOStdReq),
            ASOIOR_ReplyPort, port,
            ASOIOR_Duplicate, master,
            TAG_DONE);
        buf[i] = IExec->AllocVecTags(REQ_BYTES,
            AVT_Type,      MEMF_SHARED,
            AVT_Alignment, 4096,
            AVT_Clear,     0, TAG_DONE);
        if (!req[i] || !buf[i]) ok_setup = FALSE;
    }
    report(ok_setup, "Allocated %u duplicated IORequests + buffers", NREQS);
    if (!ok_setup) goto cleanup;

    /* ------------------------------------------------------------ */
    /* Rounds of parallel write bursts then parallel read+verify     */
    /* ------------------------------------------------------------ */
    for (ULONG r = 0; r < rounds; r++) {
        ULONG werr = 0, busy = 0;

        for (int i = 0; i < NREQS; i++) {
            fill_pattern(buf[i], REQ_BYTES, r * NREQS + i);
            req[i]->io_Command = CMD_WRITE;
            req[i]->io_Data    = buf[i];
            req[i]->io_Length  = REQ_BYTES;
            req[i]->io_Offset  = BASE_OFF + (ULONG)i * REGION
                               + (r % 4) * REQ_BYTES;
            IExec->SendIO((struct IORequest *)req[i]);
        }
        for (int i = 0; i < NREQS; i++) {
            IExec->WaitIO((struct IORequest *)req[i]);
            if (req[i]->io_Error == IOERR_UNITBUSY) busy++;
            else if (req[i]->io_Error != 0) werr++;
        }
        report(werr == 0 && busy == 0,
               "Round %lu: %u parallel writes (errors=%lu unitbusy=%lu)",
               r, NREQS, werr, busy);

        ULONG rerr = 0, vbad = 0;
        busy = 0;
        for (int i = 0; i < NREQS; i++) {
            memset(buf[i], 0, REQ_BYTES);
            req[i]->io_Command = CMD_READ;
            req[i]->io_Data    = buf[i];
            req[i]->io_Length  = REQ_BYTES;
            req[i]->io_Offset  = BASE_OFF + (ULONG)i * REGION
                               + (r % 4) * REQ_BYTES;
            IExec->SendIO((struct IORequest *)req[i]);
        }
        for (int i = 0; i < NREQS; i++) {
            IExec->WaitIO((struct IORequest *)req[i]);
            if (req[i]->io_Error == IOERR_UNITBUSY) busy++;
            else if (req[i]->io_Error != 0) rerr++;
            else if (check_pattern(buf[i], REQ_BYTES, r * NREQS + i) >= 0)
                vbad++;
        }
        report(rerr == 0 && busy == 0 && vbad == 0,
               "Round %lu: %u parallel reads (errors=%lu unitbusy=%lu"
               " corrupt=%lu)", r, NREQS, rerr, busy, vbad);
    }

    /* ------------------------------------------------------------ */
    /* Mixed interleave: even = write, odd = read of round-0 data    */
    /* ------------------------------------------------------------ */
    {
        ULONG err = 0, busy = 0, vbad = 0;
        for (int i = 0; i < NREQS; i++) {
            if (i & 1) {
                memset(buf[i], 0, REQ_BYTES);
                req[i]->io_Command = CMD_READ;
                req[i]->io_Offset  = BASE_OFF + (ULONG)i * REGION;
            } else {
                fill_pattern(buf[i], REQ_BYTES, 1000u + i);
                req[i]->io_Command = CMD_WRITE;
                req[i]->io_Offset  = BASE_OFF + (ULONG)i * REGION
                                   + 512u * 1024u;
            }
            req[i]->io_Data   = buf[i];
            req[i]->io_Length = REQ_BYTES;
            IExec->SendIO((struct IORequest *)req[i]);
        }
        for (int i = 0; i < NREQS; i++) {
            IExec->WaitIO((struct IORequest *)req[i]);
            if (req[i]->io_Error == IOERR_UNITBUSY) busy++;
            else if (req[i]->io_Error != 0) err++;
            else if ((i & 1) &&
                     check_pattern(buf[i], REQ_BYTES, 0 * NREQS + i) >= 0)
                vbad++;
        }
        report(err == 0 && busy == 0 && vbad == 0,
               "Mixed read/write interleave (errors=%lu unitbusy=%lu"
               " corrupt=%lu)", err, busy, vbad);
    }

    /* ------------------------------------------------------------ */
    /* AbortIO: queue a burst, abort the back half                   */
    /* ------------------------------------------------------------ */
    {
        ULONG aborted = 0, completed = 0, other = 0;
        for (int i = 0; i < NREQS; i++) {
            memset(buf[i], 0, REQ_BYTES);
            req[i]->io_Command = CMD_READ;
            req[i]->io_Data    = buf[i];
            req[i]->io_Length  = REQ_BYTES;
            req[i]->io_Offset  = BASE_OFF + (ULONG)i * REGION;
            IExec->SendIO((struct IORequest *)req[i]);
        }
        /* Abort the back half — some are still queued on the unit's
         * message port, some may already be inflight (not abortable). */
        for (int i = NREQS / 2; i < NREQS; i++)
            IExec->AbortIO((struct IORequest *)req[i]);

        for (int i = 0; i < NREQS; i++) {
            IExec->WaitIO((struct IORequest *)req[i]);
            if (req[i]->io_Error == IOERR_ABORTED)      aborted++;
            else if (req[i]->io_Error == 0)             completed++;
            else                                        other++;
        }
        report(other == 0 && aborted + completed == NREQS,
               "AbortIO burst: %lu aborted, %lu completed, %lu other"
               " errors", aborted, completed, other);

        /* The unit must still work after the abort storm. */
        memset(buf[0], 0, REQ_BYTES);
        req[0]->io_Command = CMD_READ;
        req[0]->io_Data    = buf[0];
        req[0]->io_Length  = REQ_BYTES;
        req[0]->io_Offset  = BASE_OFF;
        LONG rc = IExec->DoIO((struct IORequest *)req[0]);
        report(rc == 0 &&
               check_pattern(buf[0], REQ_BYTES, 0) < 0,
               "Post-abort read + verify");
    }

cleanup:
    for (int i = 0; i < NREQS; i++) {
        if (buf[i]) IExec->FreeVec(buf[i]);
        if (req[i]) IExec->FreeSysObject(ASOT_IOREQUEST, req[i]);
    }
    IExec->CloseDevice((struct IORequest *)master);
    report(TRUE, "CloseDevice");
    IExec->FreeSysObject(ASOT_IOREQUEST, master);
    IExec->FreeSysObject(ASOT_PORT, port);

    printf("----- stress_nvme: %d passed, %d failed -----\n",
           g_pass_count, g_fail_count);
    return (g_fail_count == 0) ? 0 : 1;
}
