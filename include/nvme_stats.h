#ifndef NVME_STATS_H
#define NVME_STATS_H

/*
 * nvme_stats.h — live statistics surface for nvme.device.
 *
 * The driver collects per-unit + per-controller counters in the hot
 * paths (Submit, Harvest, ISR).  Consumers read a snapshot via the
 * custom command NSCMD_NVME_GETSTATS or by registering a callback
 * IORequest with NSCMD_TD_ADDSTATCALLBACK.  Both paths fill a
 * struct NVMeStats in io_Data.
 *
 * Forward-compatibility: each snapshot is tagged with
 * ns_version + ns_size.  Consumers pass io_Length = the struct size
 * they know about; the driver writes min(io_Length, sizeof(struct
 * NVMeStats)) bytes.  New fields always go at the end.
 *
 * Latency is reported in native AmigaOS 4.1 FE EClock ticks
 * (struct EClockVal, 64-bit) rather than microseconds — conversion
 * costs a divide per read, so we hand the consumer `ns_eclock_freq`
 * (ticks per second) and let them decide when to convert.
 *
 * All counters are read outside the hot path under a short Forbid()
 * critical section; the snapshot copy is a small memcpy, so reading
 * stats does not introduce measurable jitter into I/O latency.
 */

#include <exec/types.h>
#include <exec/execbase.h>

/* PPC Time Base Register read.  A 64-bit monotonic tick counter with
 * frequency equal to ExecBase->ex_EClockFrequency.  Avoids the cost
 * of going through timer.device's ITimer->ReadEClock on the hot path
 * and — crucially — avoids pulling the timer interface into the ISR
 * context. */
static inline uint64 nvme_read_tbr(void)
{
    uint32 hi, lo, hi2;
    do {
        __asm__ volatile ("mftbu %0" : "=r"(hi));
        __asm__ volatile ("mftb  %0" : "=r"(lo));
        __asm__ volatile ("mftbu %0" : "=r"(hi2));
    } while (hi != hi2);
    return ((uint64)hi << 32) | (uint64)lo;
}

static inline uint32 nvme_eclock_freq(void)
{
    struct ExecBase *SysBase = *(struct ExecBase **)4;
    return SysBase->ex_EClockFrequency;
}

#define NVME_STATS_VERSION   1

/* SMART refresh cadence — seconds between automatic Get Log Page 0x02
 * fetches.  A stats consumer (GETSTATS) that arrives within this
 * window sees the cached snapshot.  Beyond it, the handler issues a
 * fresh admin command synchronously. */
#define NVME_SMART_REFRESH_SECS    30

/* In-driver SMART cache — native 64-bit fields for easy read, then
 * split into hi/lo halves for the wire NVMeStats struct. */
struct NVMeSMARTCache {
    BOOL   valid;
    uint64 last_refresh_tbr;        /* 0 = never populated */

    uint8  critical_warning;
    uint16 temp_k;                  /* composite temperature, Kelvin */
    uint8  spare_pct;
    uint8  spare_threshold;
    uint8  life_used_pct;

    uint64 data_read_units;         /* 1000 × 512-byte increments per spec */
    uint64 data_written_units;
    uint64 host_reads;
    uint64 host_writes;
    uint64 power_cycles;
    uint64 power_on_hours;
    uint64 unsafe_shutdowns;
    uint64 media_errors;
};

/* Custom I/O commands (command numbers chosen to stay out of the
 * NSCMD_ETD / NSCMD_TD / CMD_* ranges used by trackdisk).
 *
 * 0xA006 / 0xA007 match the usb2 stat callback commands so a monitor
 * tool can switch between USB and NVMe targets with minimal changes. */
#define NSCMD_TD_ADDSTATCALLBACK   0xA006
#define NSCMD_TD_REMSTATCALLBACK   0xA007

/* Synchronous one-shot fetch.  Our own allocation, distinct from usb2
 * so future collisions are avoided. */
#define NSCMD_NVME_GETSTATS        0xA100

struct NVMeStats {
    /* ---- version + identity ---- */
    uint32 ns_version;              /* == NVME_STATS_VERSION                */
    uint32 ns_size;                 /* sizeof(struct NVMeStats)             */
    uint32 ns_controller;           /* index into NVMeBase.controllers[]    */
    uint32 ns_unit;                 /* flat unit number                     */

    /* ---- I/O counters ---- */
    uint32 ns_read_cmds_hi, ns_read_cmds_lo;     /* 64-bit read count    */
    uint32 ns_read_bytes_hi, ns_read_bytes_lo;   /* 64-bit bytes read    */
    uint32 ns_write_cmds_hi, ns_write_cmds_lo;   /* 64-bit write count   */
    uint32 ns_write_bytes_hi, ns_write_bytes_lo; /* 64-bit bytes written */
    uint32 ns_flush_cmds_hi, ns_flush_cmds_lo;   /* 64-bit flush count   */

    /* ---- Error buckets ---- */
    uint32 ns_err_timeout;
    uint32 ns_err_status;           /* non-zero NVMe CQE SC                 */
    uint32 ns_err_abort;
    uint32 ns_err_dma;

    /* ---- Queue / path stats ---- */
    uint32 ns_inflight_current;
    uint32 ns_inflight_peak;
    uint32 ns_unitbusy_hits;        /* "all slots full" rejections */
    uint32 ns_mdts_splits;          /* transfers that had to chunk */
    uint32 ns_bounce_hits;          /* transfers that took the bounce path */
    uint32 ns_direct_dma_hits;      /* transfers that took the direct path */
    uint32 ns_prp_list_hits;        /* transfers that used the PRP list page */

    /* ---- Latency (ticks) ---- */
    uint32 ns_total_io_ticks_hi, ns_total_io_ticks_lo;
    uint32 ns_max_io_ticks_hi,   ns_max_io_ticks_lo;
    uint32 ns_eclock_freq;          /* ticks per second from ReadEClock    */

    /* ---- ISR counters (aggregate across controllers) ---- */
    uint32 ns_isr_count;
    uint32 ns_isr_claimed;
    uint32 ns_isr_not_ours;

    /* ---- SMART — populated only if ENABLE_SMART is defined ---- */
    uint8  ns_smart_valid;
    uint8  ns_smart_critical;
    uint16 ns_smart_temp_k;         /* composite temperature in Kelvin   */
    uint8  ns_smart_spare_pct;
    uint8  ns_smart_spare_thr;
    uint8  ns_smart_life_used;      /* wear % used */
    uint8  ns_smart_pad;
    uint32 ns_smart_data_read_hi,    ns_smart_data_read_lo;
    uint32 ns_smart_data_written_hi, ns_smart_data_written_lo;
    uint32 ns_smart_host_reads_hi,   ns_smart_host_reads_lo;
    uint32 ns_smart_host_writes_hi,  ns_smart_host_writes_lo;
    uint32 ns_smart_power_cycles_hi, ns_smart_power_cycles_lo;
    uint32 ns_smart_power_on_hrs_hi, ns_smart_power_on_hrs_lo;
    uint32 ns_smart_unsafe_shutdn_hi,ns_smart_unsafe_shutdn_lo;
    uint32 ns_smart_media_errors_hi, ns_smart_media_errors_lo;

    /* ---- Identify-derived static info ---- */
    char   ns_model[41];            /* from Identify Controller MN */
    char   ns_serial[21];           /* from Identify Controller SN */
    char   ns_fw_rev[9];            /* from Identify Controller FR */
    uint32 ns_namespace_id;
    uint32 ns_lba_count_hi, ns_lba_count_lo;  /* 64-bit LBA total */
    uint32 ns_lba_size;
    uint32 ns_pad;
};

/* In-driver per-unit accumulators.  Kept separate from the wire
 * struct above so we can cheaply increment without encoding the
 * version tag each time.  One of these lives inside each NVMeUnit. */
struct NVMeUnitStats {
    /* I/O counters */
    uint64 reads, writes, flushes;
    uint64 read_bytes, write_bytes;

    /* Errors (per-unit) */
    uint32 err_timeout;
    uint32 err_status;
    uint32 err_abort;
    uint32 err_dma;

    /* Queue / path */
    uint32 inflight_current;
    uint32 inflight_peak;
    uint32 unitbusy_hits;
    uint32 mdts_splits;
    uint32 bounce_hits;
    uint32 direct_dma_hits;
    uint32 prp_list_hits;

    /* Latency accumulators (EClock ticks) */
    uint64 total_io_ticks;
    uint64 max_io_ticks;
};

struct NVMeBase;
struct NVMeUnit;
struct IOStdReq;
struct ExecIFace;

/* Command handlers exposed to BeginIO / unit_task. */
void NVMe_HandleGetStats(struct NVMeBase *devBase, struct NVMeUnit *unit,
                         struct IOStdReq *ioreq);

/* Per-unit accumulator helpers (called from the hot paths).  Kept
 * inlineable — just struct field writes with no locking; each unit's
 * counters are touched only by its own task (+ISR updates are to the
 * global aggregate, not per-unit).  */

#endif /* NVME_STATS_H */
