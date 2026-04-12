/*
 * nvme_stats.c — CLI monitor for nvme.device's live statistics.
 *
 * Usage:
 *   nvme_stats              one-shot snapshot of unit 0
 *   nvme_stats <unit>       one-shot snapshot of the named unit
 *   nvme_stats -w <unit>    watch mode, refresh every 1 s
 *   nvme_stats -w <unit> <secs>  custom refresh interval
 *   nvme_stats -s           summary line for every unit on the bus
 *
 * Reads the stats block via NSCMD_NVME_GETSTATS (0xA100) and formats
 * it for humans — byte counters in MiB/GiB, latencies in microseconds
 * (converted from the reported EClock tick rate), a one-line SMART
 * health summary.  Exit code 0 on any successful print.
 *
 * Built with -lauto; clib4 CRT provides IExec/IDOS.  Banner format
 * matches test_nvme.c so paired logs are easy to line up.
 */

#include "nvme.h"          /* NVME_MAX_GLOBAL_UNITS */
#include "nvme_stats.h"

#include <exec/exec.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/errors.h>
#include <devices/newstyle.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef BUILD_DATE
#define BUILD_DATE "unknown"
#endif
#ifndef BUILD_TIME
#define BUILD_TIME "unknown"
#endif

#define DEVNAME         "nvme.device"
#define MAX_UNITS       NVME_MAX_GLOBAL_UNITS

/* Combine hi/lo 32-bit pair into a uint64 — the wire format splits
 * them so AmigaOS DebugPrintF and printf can handle 32-bit arguments
 * cleanly even though the underlying value is 64-bit. */
static inline uint64 combine64(uint32 hi, uint32 lo)
{
    return ((uint64)hi << 32) | (uint64)lo;
}

/* Format a byte count in a human-readable way.  Output buffer must be
 * at least 32 bytes.  We prefer GiB if >= 1 GiB, else MiB if >= 1 MiB,
 * else KiB, else raw bytes. */
static void format_bytes(char *out, size_t n, uint64 bytes)
{
    if (bytes >= ((uint64)1 << 30)) {
        uint64 gib100 = (bytes * 100ULL) >> 30;
        snprintf(out, n, "%lu.%02lu GiB",
                 (ULONG)(gib100 / 100), (ULONG)(gib100 % 100));
    } else if (bytes >= ((uint64)1 << 20)) {
        uint64 mib100 = (bytes * 100ULL) >> 20;
        snprintf(out, n, "%lu.%02lu MiB",
                 (ULONG)(mib100 / 100), (ULONG)(mib100 % 100));
    } else if (bytes >= 1024ULL) {
        uint64 kib100 = (bytes * 100ULL) >> 10;
        snprintf(out, n, "%lu.%02lu KiB",
                 (ULONG)(kib100 / 100), (ULONG)(kib100 % 100));
    } else {
        snprintf(out, n, "%lu B", (ULONG)bytes);
    }
}

/* Convert ticks into microseconds.  freq is ticks per second.
 * Careful with overflow: do the multiply first only if ticks is
 * small, otherwise divide first (accepting a bit of precision loss). */
static uint64 ticks_to_us(uint64 ticks, uint32 freq)
{
    if (freq == 0) return 0;
    const uint64 million = 1000000ULL;
    if (ticks < ((uint64)~0ULL / million))
        return (ticks * million) / (uint64)freq;
    return (ticks / (uint64)freq) * million;
}

/* Human-readable SMART critical flag byte.  NVMe 1.4 §5.14.1.2 fig 208. */
static const char *smart_critical_str(uint8 bits, char *buf, size_t n)
{
    if (bits == 0) { snprintf(buf, n, "OK"); return buf; }
    char *p = buf;
    size_t left = n;
    int first = 1;
    #define FLAG(MASK, S)                                          \
        do { if (bits & (MASK)) {                                  \
                int wrote = snprintf(p, left, "%s%s", first?"":",", (S)); \
                if (wrote < 0) break;                              \
                p += wrote; if ((size_t)wrote >= left) wrote = left-1; \
                left -= wrote; first = 0;                          \
             } } while (0)
    FLAG(0x01, "SPARE_LOW");
    FLAG(0x02, "TEMP_EXCURSION");
    FLAG(0x04, "RELIAB_DEGRADED");
    FLAG(0x08, "READONLY");
    FLAG(0x10, "VOLATILE_BACKUP_FAIL");
    FLAG(0x20, "PMR_UNRELIABLE");
    #undef FLAG
    return buf;
}

/* Fetch one snapshot into a caller-supplied struct.  Returns 0 on
 * success, non-zero io_Error on failure (negative for out-of-band
 * failures).  ioreq must be prepared by the caller. */
static LONG fetch_stats(struct IOStdReq *ioreq, struct NVMeStats *out)
{
    memset(out, 0, sizeof(*out));
    ioreq->io_Command = NSCMD_NVME_GETSTATS;
    ioreq->io_Data    = out;
    ioreq->io_Length  = sizeof(*out);
    IExec->DoIO((struct IORequest *)ioreq);
    return ioreq->io_Error;
}

static void print_one_snapshot(const struct NVMeStats *s)
{
    char readB[32], writtenB[32], critBuf[64];
    format_bytes(readB,    sizeof(readB),    combine64(s->ns_read_bytes_hi,  s->ns_read_bytes_lo));
    format_bytes(writtenB, sizeof(writtenB), combine64(s->ns_write_bytes_hi, s->ns_write_bytes_lo));

    uint64 reads   = combine64(s->ns_read_cmds_hi,   s->ns_read_cmds_lo);
    uint64 writes  = combine64(s->ns_write_cmds_hi,  s->ns_write_cmds_lo);
    uint64 flushes = combine64(s->ns_flush_cmds_hi,  s->ns_flush_cmds_lo);
    uint64 cmds    = reads + writes + flushes;

    uint64 total_ticks = combine64(s->ns_total_io_ticks_hi, s->ns_total_io_ticks_lo);
    uint64 max_ticks   = combine64(s->ns_max_io_ticks_hi,   s->ns_max_io_ticks_lo);
    uint32 freq        = s->ns_eclock_freq;

    uint64 total_us = ticks_to_us(total_ticks, freq);
    uint64 mean_us  = cmds ? (total_us / cmds) : 0;
    uint64 max_us   = ticks_to_us(max_ticks, freq);

    uint64 lba_count = combine64(s->ns_lba_count_hi, s->ns_lba_count_lo);
    uint64 cap_bytes = lba_count * (uint64)s->ns_lba_size;
    char   capB[32]; format_bytes(capB, sizeof(capB), cap_bytes);

    printf("=== %s unit %lu (controller %lu) ===\n",
           DEVNAME, (ULONG)s->ns_unit, (ULONG)s->ns_controller);
    printf("  Model    : %s\n", s->ns_model[0]  ? s->ns_model  : "(unknown)");
    printf("  Serial   : %s\n", s->ns_serial[0] ? s->ns_serial : "(unknown)");
    printf("  Firmware : %s\n", s->ns_fw_rev[0] ? s->ns_fw_rev : "(unknown)");
    printf("  NSID     : %lu   Block size: %lu B   Capacity: %s\n",
           (ULONG)s->ns_namespace_id, (ULONG)s->ns_lba_size, capB);

    printf("\n  I/O (lifetime of this driver instance):\n");
    printf("    reads   : %lu cmds, %s\n", (ULONG)reads,   readB);
    printf("    writes  : %lu cmds, %s\n", (ULONG)writes,  writtenB);
    printf("    flushes : %lu cmds\n",    (ULONG)flushes);
    printf("    errors  : timeout=%lu status=%lu abort=%lu dma=%lu\n",
           (ULONG)s->ns_err_timeout, (ULONG)s->ns_err_status,
           (ULONG)s->ns_err_abort,   (ULONG)s->ns_err_dma);

    printf("\n  Queue / path:\n");
    printf("    inflight  : current=%lu peak=%lu\n",
           (ULONG)s->ns_inflight_current, (ULONG)s->ns_inflight_peak);
    printf("    paths     : bounce=%lu direct=%lu prp_list=%lu\n",
           (ULONG)s->ns_bounce_hits, (ULONG)s->ns_direct_dma_hits,
           (ULONG)s->ns_prp_list_hits);
    printf("    mdts_split=%lu   unit_busy=%lu\n",
           (ULONG)s->ns_mdts_splits, (ULONG)s->ns_unitbusy_hits);

    printf("\n  Latency (host-measured, submit→harvest):\n");
    printf("    mean=%lu us   max=%lu us   (eclock freq %lu Hz)\n",
           (ULONG)mean_us, (ULONG)max_us, (ULONG)freq);

    printf("\n  ISR (aggregate across controllers):\n");
    printf("    total=%lu claimed=%lu not-ours=%lu\n",
           (ULONG)s->ns_isr_count, (ULONG)s->ns_isr_claimed,
           (ULONG)s->ns_isr_not_ours);

    if (s->ns_smart_valid) {
        uint64 data_read    = combine64(s->ns_smart_data_read_hi,    s->ns_smart_data_read_lo);
        uint64 data_written = combine64(s->ns_smart_data_written_hi, s->ns_smart_data_written_lo);
        uint64 host_reads   = combine64(s->ns_smart_host_reads_hi,   s->ns_smart_host_reads_lo);
        uint64 host_writes  = combine64(s->ns_smart_host_writes_hi,  s->ns_smart_host_writes_lo);
        uint64 pwr_cycles   = combine64(s->ns_smart_power_cycles_hi, s->ns_smart_power_cycles_lo);
        uint64 pwr_hours    = combine64(s->ns_smart_power_on_hrs_hi, s->ns_smart_power_on_hrs_lo);
        uint64 unsafe_sh    = combine64(s->ns_smart_unsafe_shutdn_hi,s->ns_smart_unsafe_shutdn_lo);
        uint64 media_err    = combine64(s->ns_smart_media_errors_hi, s->ns_smart_media_errors_lo);

        /* SMART totals are in 1000×512-byte units per spec. */
        char readT[32], writtenT[32];
        format_bytes(readT,    sizeof(readT),    data_read    * 1000ULL * 512ULL);
        format_bytes(writtenT, sizeof(writtenT), data_written * 1000ULL * 512ULL);

        int temp_c = (int)s->ns_smart_temp_k - 273;
        printf("\n  SMART:\n");
        printf("    critical  : %s (0x%02lX)\n",
               smart_critical_str(s->ns_smart_critical, critBuf, sizeof(critBuf)),
               (ULONG)s->ns_smart_critical);
        printf("    temp      : %d C (%lu K)\n",
               temp_c, (ULONG)s->ns_smart_temp_k);
        printf("    spare     : %lu%% (threshold %lu%%)\n",
               (ULONG)s->ns_smart_spare_pct, (ULONG)s->ns_smart_spare_thr);
        printf("    life used : %lu%%\n", (ULONG)s->ns_smart_life_used);
        printf("    lifetime  : %s read, %s written\n", readT, writtenT);
        printf("    host cmds : %lu reads, %lu writes\n",
               (ULONG)host_reads, (ULONG)host_writes);
        printf("    power     : %lu cycles, %lu on-hours, %lu unsafe-shut\n",
               (ULONG)pwr_cycles, (ULONG)pwr_hours, (ULONG)unsafe_sh);
        printf("    media errs: %lu\n", (ULONG)media_err);
    } else {
        printf("\n  SMART: (unavailable)\n");
    }
    printf("\n");
}

/* Compact one-line summary for "-s" mode. */
static void print_summary_line(const struct NVMeStats *s)
{
    uint64 reads  = combine64(s->ns_read_cmds_hi,  s->ns_read_cmds_lo);
    uint64 writes = combine64(s->ns_write_cmds_hi, s->ns_write_cmds_lo);
    uint64 rb     = combine64(s->ns_read_bytes_hi, s->ns_read_bytes_lo);
    uint64 wb     = combine64(s->ns_write_bytes_hi,s->ns_write_bytes_lo);
    char rbS[24], wbS[24];
    format_bytes(rbS, sizeof(rbS), rb);
    format_bytes(wbS, sizeof(wbS), wb);
    printf("unit %2lu  ctrl %lu  NS %lu  R %lu cmds (%s)  W %lu cmds (%s)  inflight %lu/%lu\n",
           (ULONG)s->ns_unit, (ULONG)s->ns_controller,
           (ULONG)s->ns_namespace_id,
           (ULONG)reads, rbS, (ULONG)writes, wbS,
           (ULONG)s->ns_inflight_current, (ULONG)s->ns_inflight_peak);
}

/* Open a unit, fetch one snapshot.  Returns 0 on success. */
static LONG snapshot_one_unit(ULONG unit, struct NVMeStats *out, BOOL quiet)
{
    struct MsgPort *port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_DONE);
    if (!port) return -1;

    struct IOStdReq *ioreq = (struct IOStdReq *)
        IExec->AllocSysObjectTags(ASOT_IOREQUEST,
            ASOIOR_Size,      sizeof(struct IOStdReq),
            ASOIOR_ReplyPort, port,
            TAG_DONE);
    if (!ioreq) {
        IExec->FreeSysObject(ASOT_PORT, port);
        return -1;
    }

    LONG err = IExec->OpenDevice(DEVNAME, unit, (struct IORequest *)ioreq, 0);
    if (err != 0) {
        if (!quiet)
            printf("OpenDevice(%s, %lu) failed: %ld\n", DEVNAME, unit, err);
        IExec->FreeSysObject(ASOT_IOREQUEST, ioreq);
        IExec->FreeSysObject(ASOT_PORT, port);
        return err;
    }

    LONG rc = fetch_stats(ioreq, out);
    if (rc != 0 && !quiet)
        printf("NSCMD_NVME_GETSTATS on unit %lu: io_Error=%ld\n", unit, rc);

    IExec->CloseDevice((struct IORequest *)ioreq);
    IExec->FreeSysObject(ASOT_IOREQUEST, ioreq);
    IExec->FreeSysObject(ASOT_PORT, port);
    return rc;
}

int main(int argc, char *argv[])
{
    printf("nvme_stats  (built %s %s)\n\n", BUILD_DATE, BUILD_TIME);

    BOOL  watch_mode   = FALSE;
    BOOL  summary_mode = FALSE;
    ULONG unit         = 0;
    ULONG watch_secs   = 1;

    int argi = 1;
    while (argi < argc && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "-w") == 0)      { watch_mode   = TRUE; argi++; }
        else if (strcmp(argv[argi], "-s") == 0) { summary_mode = TRUE; argi++; }
        else if (strcmp(argv[argi], "-h") == 0 || strcmp(argv[argi], "--help") == 0) {
            printf("Usage: nvme_stats [-s | -w <unit> [secs] | <unit>]\n"
                   "  -s            summary for every unit (default)\n"
                   "  -w u [s]      watch mode: refresh unit u every s seconds (1 s default)\n"
                   "  <unit>        one-shot snapshot of a single unit\n");
            return 0;
        } else {
            printf("unknown flag %s — try -h\n", argv[argi]);
            return 1;
        }
    }
    if (argi < argc) unit       = (ULONG)strtol(argv[argi++], NULL, 10);
    if (argi < argc) watch_secs = (ULONG)strtol(argv[argi++], NULL, 10);
    if (watch_secs == 0) watch_secs = 1;

    struct NVMeStats s;

    if (summary_mode) {
        printf("Summary of all %s units (up to %lu):\n", DEVNAME, (ULONG)MAX_UNITS);
        ULONG found = 0;
        for (ULONG u = 0; u < MAX_UNITS; u++) {
            if (snapshot_one_unit(u, &s, TRUE) == 0) {
                print_summary_line(&s);
                found++;
            }
        }
        printf("----- %lu unit(s) found -----\n", found);
        return 0;
    }

    if (watch_mode) {
        printf("Watching %s unit %lu every %lu s — Ctrl-C to stop\n\n",
               DEVNAME, unit, watch_secs);
        while (1) {
            if (IExec->SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
                printf("Ctrl-C — exiting\n");
                return 0;
            }
            if (snapshot_one_unit(unit, &s, FALSE) != 0)
                return 1;
            print_one_snapshot(&s);
            IDOS->Delay(watch_secs * 50UL);   /* 50 ticks = 1 second */
        }
    }

    if (snapshot_one_unit(unit, &s, FALSE) != 0)
        return 1;
    print_one_snapshot(&s);
    return 0;
}
