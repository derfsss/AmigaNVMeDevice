/*
 * nvme_stats.c — snapshot assembly and NSCMD_NVME_GETSTATS handler.
 *
 * A consumer calls DoIO() with:
 *      io_Command = NSCMD_NVME_GETSTATS;
 *      io_Data    = pointer to caller's NVMeStats (or a prefix of it);
 *      io_Length  = sizeof(caller's NVMeStats).
 *
 * We write min(io_Length, sizeof(NVMeStats)) bytes and set io_Actual
 * to the number of bytes populated.  The version + size tags at the
 * top of the struct let future consumers detect which fields are
 * populated without having to upgrade the driver first.
 *
 * Snapshot is taken under Forbid/Permit — critical section is small
 * (plain struct copies, no I/O, no locks), and we avoid the
 * possibility of the unit task writing half-updated counters mid-read.
 */

#include "nvme_stats.h"
#include "nvme_device.h"
#include "nvme_debug.h"
#include "nvme_admin.h"

#include <exec/exec.h>
#include <exec/errors.h>
#include <string.h>

extern volatile ULONG nvme_isr_count;
extern volatile ULONG nvme_isr_claimed;
extern volatile ULONG nvme_isr_not_ours;

/* Helper: split a 64-bit uint64 into hi/lo 32-bit halves for the
 * wire struct.  This avoids %llu in DebugPrintF and keeps the wire
 * format trivially portable. */
static inline void split64(uint64 v, uint32 *hi, uint32 *lo)
{
    *hi = (uint32)(v >> 32);
    *lo = (uint32)(v & 0xFFFFFFFFu);
}

/* Build a complete snapshot into `out` for the given unit.
 * Safe to call from task context (BeginIO or unit task). */
static void nvme_build_snapshot(struct NVMeBase *devBase,
                                struct NVMeUnit *unit,
                                struct NVMeStats *out)
{
    struct NVMeController *ctrl = unit->ctrl;
    struct ExecIFace      *IExec = devBase->IExec;

    memset(out, 0, sizeof(*out));

    out->ns_version    = NVME_STATS_VERSION;
    out->ns_size       = sizeof(*out);
    out->ns_controller = ctrl->ctrl_idx;
    out->ns_unit       = unit->unit_num;

    /* Brief critical section so counter updates from Submit/Harvest
     * running on the unit task don't tear a 64-bit read in half. */
    IExec->Forbid();

    const struct NVMeUnitStats *s = &unit->stats;

    split64(s->reads,      &out->ns_read_cmds_hi,   &out->ns_read_cmds_lo);
    split64(s->writes,     &out->ns_write_cmds_hi,  &out->ns_write_cmds_lo);
    split64(s->flushes,    &out->ns_flush_cmds_hi,  &out->ns_flush_cmds_lo);
    split64(s->read_bytes, &out->ns_read_bytes_hi,  &out->ns_read_bytes_lo);
    split64(s->write_bytes,&out->ns_write_bytes_hi, &out->ns_write_bytes_lo);

    out->ns_err_timeout      = s->err_timeout;
    out->ns_err_status       = s->err_status;
    out->ns_err_abort        = s->err_abort;
    out->ns_err_dma          = s->err_dma;

    out->ns_inflight_current = s->inflight_current;
    out->ns_inflight_peak    = s->inflight_peak;
    out->ns_unitbusy_hits    = s->unitbusy_hits;
    out->ns_mdts_splits      = s->mdts_splits;
    out->ns_bounce_hits      = s->bounce_hits;
    out->ns_direct_dma_hits  = s->direct_dma_hits;
    out->ns_prp_list_hits    = s->prp_list_hits;

    split64(s->total_io_ticks, &out->ns_total_io_ticks_hi, &out->ns_total_io_ticks_lo);
    split64(s->max_io_ticks,   &out->ns_max_io_ticks_hi,   &out->ns_max_io_ticks_lo);

    IExec->Permit();

    /* Hand the consumer the tick rate so they can convert ticks→µs. */
    out->ns_eclock_freq = nvme_eclock_freq();

    /* ISR aggregates (volatile globals; an atomic ULONG read is fine). */
    out->ns_isr_count    = nvme_isr_count;
    out->ns_isr_claimed  = nvme_isr_claimed;
    out->ns_isr_not_ours = nvme_isr_not_ours;

    /* Identify-derived static info. */
    memcpy(out->ns_model,  ctrl->model,  sizeof(out->ns_model));
    memcpy(out->ns_serial, ctrl->serial, sizeof(out->ns_serial));
    memcpy(out->ns_fw_rev, ctrl->fw_rev, sizeof(out->ns_fw_rev));

    out->ns_namespace_id = unit->nsid;
    out->ns_lba_size     = unit->block_size;
    split64(unit->total_blocks, &out->ns_lba_count_hi, &out->ns_lba_count_lo);

#ifdef ENABLE_SMART
    /* Refresh the SMART cache lazily if it's never been populated or
     * has gone stale.  The Get Log Page admin command takes a few
     * milliseconds under QEMU and blocks only this caller. */
    {
        struct NVMeSMARTCache *sc = &ctrl->smart_cache;
        uint32 freq   = nvme_eclock_freq();
        uint64 now    = nvme_read_tbr();
        uint64 age    = (sc->last_refresh_tbr && now >= sc->last_refresh_tbr)
                      ? (now - sc->last_refresh_tbr) : ~(uint64)0;
        uint64 window = (uint64)freq * NVME_SMART_REFRESH_SECS;

        if (!sc->valid || age > window) {
            /* Best-effort refresh; leave ns_smart_valid=0 on failure. */
            (void)NVMe_RefreshSMART(ctrl);
        }

        if (sc->valid) {
            out->ns_smart_valid     = 1;
            out->ns_smart_critical  = sc->critical_warning;
            out->ns_smart_temp_k    = sc->temp_k;
            out->ns_smart_spare_pct = sc->spare_pct;
            out->ns_smart_spare_thr = sc->spare_threshold;
            out->ns_smart_life_used = sc->life_used_pct;
            split64(sc->data_read_units,
                    &out->ns_smart_data_read_hi,    &out->ns_smart_data_read_lo);
            split64(sc->data_written_units,
                    &out->ns_smart_data_written_hi, &out->ns_smart_data_written_lo);
            split64(sc->host_reads,
                    &out->ns_smart_host_reads_hi,   &out->ns_smart_host_reads_lo);
            split64(sc->host_writes,
                    &out->ns_smart_host_writes_hi,  &out->ns_smart_host_writes_lo);
            split64(sc->power_cycles,
                    &out->ns_smart_power_cycles_hi, &out->ns_smart_power_cycles_lo);
            split64(sc->power_on_hours,
                    &out->ns_smart_power_on_hrs_hi, &out->ns_smart_power_on_hrs_lo);
            split64(sc->unsafe_shutdowns,
                    &out->ns_smart_unsafe_shutdn_hi,&out->ns_smart_unsafe_shutdn_lo);
            split64(sc->media_errors,
                    &out->ns_smart_media_errors_hi, &out->ns_smart_media_errors_lo);
        }
    }
#endif
}

void NVMe_HandleGetStats(struct NVMeBase *devBase, struct NVMeUnit *unit,
                         struct IOStdReq *ioreq)
{
    if (!ioreq->io_Data || ioreq->io_Length < sizeof(uint32) * 2) {
        ioreq->io_Error  = IOERR_BADLENGTH;
        ioreq->io_Actual = 0;
        return;
    }

    struct NVMeStats full;
    nvme_build_snapshot(devBase, unit, &full);

    ULONG want = ioreq->io_Length;
    ULONG have = sizeof(full);
    ULONG copy = (want < have) ? want : have;
    memcpy(ioreq->io_Data, &full, copy);

    ioreq->io_Actual = copy;
    ioreq->io_Error  = 0;
}
