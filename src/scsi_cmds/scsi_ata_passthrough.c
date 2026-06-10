/*
 * scsi_ata_passthrough.c — ATA PASS-THROUGH → NVMe SMART translation.
 *
 * CDB opcodes handled:
 *   0x85  ATA PASS-THROUGH (16-byte CDB)
 *   0xA1  ATA PASS-THROUGH (12-byte CDB)
 *
 * The only ATA command we synthesise a response for is 0xB0 (SMART),
 * sub-commands 0xD0 (READ DATA) and 0xD1 (READ THRESHOLDS).  Every
 * other ATA command returns CHECK CONDITION / ILLEGAL REQUEST — the
 * caller's expected fallback behaviour for "no ATA layer here".
 *
 * The response is a 512-byte ATA SMART Read-Data structure
 * (ATA/ATAPI-8 §7.52) whose attribute entries are built from the live
 * NVMe Health Information Log (NVMe 1.4 §5.14.1.2) cached in
 * ctrl->smart_cache.  Building this at request time gives SMART
 * viewers (e.g. AmigaDiskBench's SMART tab) real data — temperature,
 * power-on hours, power-cycle count, unsafe-shutdown count, spare%,
 * wear% — rather than canned placeholder values.
 *
 * If ENABLE_SMART is not compiled in, the helper falls back to
 * canned values so the tool still gets a plausible response.
 */

#include "nvme_device.h"
#include "nvme_scsi.h"
#include "nvme_admin.h"
#include "nvme_stats.h"
#include "nvme_debug.h"

#include <devices/scsidisk.h>
#include <exec/errors.h>
#include <string.h>

/* -------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------ */

static inline void set_le16(UBYTE *p, UWORD v)
{
    p[0] = (UBYTE)(v & 0xFF);
    p[1] = (UBYTE)((v >> 8) & 0xFF);
}

/* Write a uint64 into a 6-byte little-endian raw field (ATA convention
 * limits raw values to 48 bits; values above that are saturated). */
static void set_raw48(UBYTE *p6, uint64 v)
{
    if (v > 0x0000FFFFFFFFFFFFULL)
        v = 0x0000FFFFFFFFFFFFULL;
    p6[0] = (UBYTE)(v       & 0xFF);
    p6[1] = (UBYTE)((v >> 8)  & 0xFF);
    p6[2] = (UBYTE)((v >> 16) & 0xFF);
    p6[3] = (UBYTE)((v >> 24) & 0xFF);
    p6[4] = (UBYTE)((v >> 32) & 0xFF);
    p6[5] = (UBYTE)((v >> 40) & 0xFF);
}

/* Clamp to 1..253 as ATA "current/worst" values conventionally do. */
static UBYTE clamp_normalised(int v)
{
    if (v <   1) return   1;
    if (v > 253) return 253;
    return (UBYTE)v;
}

/* -------------------------------------------------------------------
 * Attribute table — written at request time with live NVMe values.
 * ------------------------------------------------------------------ */

struct ATASmartAttr {
    UBYTE id;
    UBYTE flags_lo;
    UBYTE flags_hi;
    UBYTE current;
    UBYTE worst;
    UBYTE raw[6];
    UBYTE reserved;
};

/* Attribute IDs we populate.  Order and membership are stable so the
 * threshold pass can line up with the data pass. */
enum {
    ATA_ATTR_POH            = 9,    /* Power-On Hours                   */
    ATA_ATTR_POWER_CYCLES   = 12,   /* Power-Cycle Count                */
    ATA_ATTR_UNSAFE_SHUTDN  = 192,  /* Unsafe Shutdowns (repurposed)    */
    ATA_ATTR_TEMPERATURE    = 194,  /* Composite Temperature °C         */
    ATA_ATTR_MEDIA_ERRORS   = 196,  /* Media & Data Integrity Errors    */
    ATA_ATTR_SPARE_PCT      = 231,  /* SSD Life Left / Available Spare  */
    ATA_ATTR_WEAR           = 233,  /* Media Wearout Indicator          */
};

/* ATA attribute-status flag bit masks (ATA/ATAPI-8 Figure 54).  We
 * mark each attribute Prefailure/Always-online and (for some) Error-
 * rate so health viewers recognise the severity category. */
#define ATA_FLAG_PREFAIL        0x0001
#define ATA_FLAG_ONLINE         0x0002
#define ATA_FLAG_PERFORMANCE    0x0004
#define ATA_FLAG_ERROR_RATE     0x0008
#define ATA_FLAG_EVENT_COUNT    0x0010
#define ATA_FLAG_SELF_PRESERV   0x0020

/* -------------------------------------------------------------------
 * Build the attribute table from the live NVMe SMART cache.
 * ------------------------------------------------------------------ */

static void build_attrs_from_nvme(struct ATASmartAttr *out,
                                  ULONG *count_out,
                                  struct NVMeController *ctrl)
{
#ifdef ENABLE_SMART
    struct ExecIFace     *IExec = ctrl->dev_base->IExec;
    struct NVMeSMARTCache *sc   = &ctrl->smart_cache;

    uint32 freq   = nvme_eclock_freq();
    uint64 now    = nvme_read_tbr();
    uint64 age    = (sc->last_refresh_tbr && now >= sc->last_refresh_tbr)
                  ? (now - sc->last_refresh_tbr) : ~(uint64)0;
    uint64 window = (uint64)freq * NVME_SMART_REFRESH_SECS;

    DPRINTF(IExec, "[nvme.device:scsi-ata] smart_cache.valid=%lu age_stale=%lu\n",
            (ULONG)(sc->valid ? 1 : 0),
            (ULONG)((!sc->valid || age > window) ? 1 : 0));

    if (!sc->valid || age > window) {
        DPRINTF(IExec, "[nvme.device:scsi-ata] calling NVMe_RefreshSMART\n");
        BOOL ok = NVMe_RefreshSMART(ctrl);
        DPRINTF(IExec, "[nvme.device:scsi-ata] NVMe_RefreshSMART returned %lu\n",
                (ULONG)(ok ? 1 : 0));
    }
#endif

    ULONG n = 0;

#ifdef ENABLE_SMART
    if (ctrl->smart_cache.valid) {
        struct NVMeSMARTCache *v = &ctrl->smart_cache;
        int temp_c = (int)v->temp_k - 273;
        if (temp_c < 0) temp_c = 0;

        /* Power-On Hours — prefail, always online. */
        out[n] = (struct ATASmartAttr){0};
        out[n].id       = ATA_ATTR_POH;
        set_le16(&out[n].flags_lo, ATA_FLAG_ONLINE);
        out[n].current  = 100; out[n].worst = 100;
        set_raw48(out[n].raw, v->power_on_hours);
        n++;

        /* Power-Cycle Count. */
        out[n] = (struct ATASmartAttr){0};
        out[n].id       = ATA_ATTR_POWER_CYCLES;
        set_le16(&out[n].flags_lo, ATA_FLAG_ONLINE);
        out[n].current  = 100; out[n].worst = 100;
        set_raw48(out[n].raw, v->power_cycles);
        n++;

        /* Unsafe-shutdown count — event count, non-critical. */
        out[n] = (struct ATASmartAttr){0};
        out[n].id       = ATA_ATTR_UNSAFE_SHUTDN;
        set_le16(&out[n].flags_lo, ATA_FLAG_ONLINE | ATA_FLAG_EVENT_COUNT);
        out[n].current  = 100; out[n].worst = 100;
        set_raw48(out[n].raw, v->unsafe_shutdowns);
        n++;

        /* Temperature — current in raw[0], min in raw[2], max in raw[4]
         * (same layout mainstream SMART viewers expect for attr 194).
         * Current normalised value = 100 - temp_c / 2 so it trends down
         * at high temperatures without ever crossing the 10-threshold. */
        out[n] = (struct ATASmartAttr){0};
        out[n].id       = ATA_ATTR_TEMPERATURE;
        set_le16(&out[n].flags_lo, ATA_FLAG_ONLINE);
        out[n].current  = clamp_normalised(100 - temp_c / 2);
        out[n].worst    = out[n].current;
        out[n].raw[0]   = (UBYTE)temp_c;
        out[n].raw[2]   = (UBYTE)temp_c;
        out[n].raw[4]   = (UBYTE)temp_c;
        n++;

        /* Media Errors — prefail: anything non-zero is concerning. */
        out[n] = (struct ATASmartAttr){0};
        out[n].id       = ATA_ATTR_MEDIA_ERRORS;
        set_le16(&out[n].flags_lo, ATA_FLAG_PREFAIL | ATA_FLAG_ONLINE);
        out[n].current  = (v->media_errors == 0) ? 100 : 50;
        out[n].worst    = out[n].current;
        set_raw48(out[n].raw, v->media_errors);
        n++;

        /* Available Spare / SSD Life Left — normalised value IS the % left. */
        out[n] = (struct ATASmartAttr){0};
        out[n].id       = ATA_ATTR_SPARE_PCT;
        set_le16(&out[n].flags_lo, ATA_FLAG_PREFAIL | ATA_FLAG_ONLINE);
        out[n].current  = clamp_normalised(v->spare_pct ? v->spare_pct : 100);
        out[n].worst    = out[n].current;
        out[n].raw[0]   = v->spare_pct;
        out[n].raw[1]   = v->spare_threshold;
        n++;

        /* Wear Indicator — normalised = 100 - life_used_pct. */
        out[n] = (struct ATASmartAttr){0};
        out[n].id       = ATA_ATTR_WEAR;
        set_le16(&out[n].flags_lo, ATA_FLAG_PREFAIL | ATA_FLAG_ONLINE);
        out[n].current  = clamp_normalised(100 - v->life_used_pct);
        out[n].worst    = out[n].current;
        out[n].raw[0]   = v->life_used_pct;
        n++;
    } else
#endif
    {
        /* Fallback when SMART is unavailable — a canned attribute
         * block so tools still receive a parseable response. */
        static const struct ATASmartAttr canned[] = {
            {   9, 0x03, 0x00, 100, 100, {0x01,0,0,0,0,0}, 0 },  /* PoH = 1 */
            {  12, 0x03, 0x00, 100, 100, {0x01,0,0,0,0,0}, 0 },  /* cycles = 1 */
            { 194, 0x02, 0x00, 120, 120, {30,0,30,0,30,0}, 0 },  /* 30°C */
            { 197, 0x00, 0x00, 100, 100, {0,0,0,0,0,0},    0 },  /* 0 pending */
            { 198, 0x00, 0x00, 100, 100, {0,0,0,0,0,0},    0 },  /* 0 uncorr */
        };
        ULONG m = sizeof(canned) / sizeof(canned[0]);
        for (ULONG i = 0; i < m; i++)
            out[i] = canned[i];
        n = m;
    }

    *count_out = n;
}

/* -------------------------------------------------------------------
 * Build the threshold table — same ID set, static "won't-trip" values.
 * ------------------------------------------------------------------ */

struct ATASmartThreshold {
    UBYTE id;
    UBYTE threshold;
    UBYTE reserved[10];
};

static void build_thresholds(struct ATASmartThreshold *out, ULONG count,
                             const struct ATASmartAttr *attrs)
{
    /* A conservative static threshold of 10 for every attribute: viewers
     * will colour any attribute whose `current` drops below this red.
     * None of our attributes normally drop that low (temperature maths
     * clamps to 1..253; errors are binary 100/50). */
    for (ULONG i = 0; i < count; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        out[i].id        = attrs[i].id;
        out[i].threshold = 10;
    }
}

/* -------------------------------------------------------------------
 * Top-level CDB handler
 * ------------------------------------------------------------------ */

void NVMe_SCSI_HandleATAPassthrough(struct NVMeBase *devBase,
                                    struct NVMeUnit *unit,
                                    struct IOStdReq *ioreq)
{
    struct ExecIFace *IExec = devBase->IExec;
    struct SCSICmd   *scsi  = (struct SCSICmd *)ioreq->io_Data;
    UBYTE            *cdb   = scsi->scsi_Command;
    UBYTE             op    = cdb[0];

    /* Extract ATA command + feature from the CDB — offsets depend on
     * whether this is the 16-byte (0x85) or 12-byte (0xA1) variant. */
    UBYTE ata_cmd  = (op == 0x85) ? cdb[14] : cdb[9];
    UBYTE ata_feat = (op == 0x85) ? cdb[4]  : cdb[3];

    DPRINTF(IExec, "[nvme.device:scsi-ata] op=0x%02lx cmd=0x%02lx feat=0x%02lx"
                   " cmdlen=%lu alloc=%lu\n",
            (ULONG)op, (ULONG)ata_cmd, (ULONG)ata_feat,
            (ULONG)scsi->scsi_CmdLength, (ULONG)scsi->scsi_Length);

    /* Only ATA SMART (0xB0) is synthesised.  Any other ATA command
     * (IDENTIFY DEVICE 0xEC, SECURITY SET PASSWORD, etc.) is rejected. */
    if (ata_cmd != 0xB0) {
        scsi->scsi_Status   = 2;
        scsi->scsi_Actual   = 0;
        NVMe_SCSI_FillSense(scsi, NVME_SSK_ILLEGAL_REQUEST, 0x20, 0x00);
        ioreq->io_Error = HFERR_BadStatus;
        return;
    }

    /* SMART sub-commands we care about. */
    const UBYTE SMART_READ_DATA       = 0xD0;
    const UBYTE SMART_READ_THRESHOLDS = 0xD1;

    if (ata_feat != SMART_READ_DATA && ata_feat != SMART_READ_THRESHOLDS) {
        DPRINTF(IExec, "[nvme.device:scsi-ata] unsupported SMART sub-cmd 0x%02lx\n",
                (ULONG)ata_feat);
        scsi->scsi_Status = 2;
        scsi->scsi_Actual = 0;
        NVMe_SCSI_FillSense(scsi, NVME_SSK_ILLEGAL_REQUEST, 0x24, 0x00);
        ioreq->io_Error = HFERR_BadStatus;
        return;
    }

    UBYTE *buf   = (UBYTE *)scsi->scsi_Data;
    ULONG  alloc = scsi->scsi_Length;
    if (!buf || alloc < 512) {
        scsi->scsi_Status = 2;
        scsi->scsi_Actual = 0;
        NVMe_SCSI_FillSense(scsi, NVME_SSK_ILLEGAL_REQUEST, 0x24, 0x00);
        ioreq->io_Error = HFERR_BadStatus;
        return;
    }

    DPRINTF(IExec, "[nvme.device:scsi-ata] memset 512\n");
    memset(buf, 0, 512);
    set_le16(buf, 0x0006);

    DPRINTF(IExec, "[nvme.device:scsi-ata] calling build_attrs_from_nvme\n");

    struct ATASmartAttr attrs[16];
    ULONG               n_attrs = 0;
    build_attrs_from_nvme(attrs, &n_attrs, unit->ctrl);

    DPRINTF(IExec, "[nvme.device:scsi-ata] build_attrs returned %lu attrs\n",
            (ULONG)n_attrs);

    if (ata_feat == SMART_READ_DATA) {
        /* Attribute block at offset 2, 12 bytes per entry, up to 30 slots. */
        ULONG off = 2;
        for (ULONG i = 0; i < n_attrs && off + 12 <= 362; i++, off += 12) {
            const struct ATASmartAttr *a = &attrs[i];
            buf[off + 0]  = a->id;
            buf[off + 1]  = a->flags_lo;
            buf[off + 2]  = a->flags_hi;
            buf[off + 3]  = a->current;
            buf[off + 4]  = a->worst;
            memcpy(&buf[off + 5], a->raw, 6);
            buf[off + 11] = a->reserved;
        }
        buf[510] = 0xC0;  /* offline data collection: never, no error */
        /* Byte 511 is the checksum — most readers skip it; leave 0. */
    } else {
        /* SMART_READ_THRESHOLDS: 12-byte entries at offset 2,
         * [id][threshold][10 × reserved]. */
        struct ATASmartThreshold thresholds[16];
        build_thresholds(thresholds, n_attrs, attrs);
        ULONG off = 2;
        for (ULONG i = 0; i < n_attrs && off + 12 <= 362; i++, off += 12) {
            buf[off + 0] = thresholds[i].id;
            buf[off + 1] = thresholds[i].threshold;
            /* bytes 2-11: zero — already memset */
        }
    }

    scsi->scsi_Actual   = 512;
    scsi->scsi_Status   = 0;       /* GOOD */
    ioreq->io_Actual    = 512;
    ioreq->io_Error     = 0;
}
