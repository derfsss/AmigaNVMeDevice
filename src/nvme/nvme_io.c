/*
 * nvme_io.c — I/O SQ submission, Flush, and CQ harvesting.
 *
 * The I/O hot path for every namespace.  A unit has NVME_MAX_INFLIGHT
 * pipelined command slots, each with its own pre-allocated bounce
 * buffer, PRP list page, and DMAEntry pool.  Submission is split into
 * two primitives so that callers can batch:
 *
 *   NVMeIO_SubmitNoRing — reserve a slot, build the SQE, advance the
 *                         shadow SQ tail.  Does NOT write the doorbell.
 *   NVMeIO_RingSQ       — publish the shadow tail to the controller.
 *
 * NVMeIO_Submit is a convenience wrapper that does both back-to-back.
 *
 * NVMeIO_Harvest drains the CQ: match CID to slot, translate status,
 * reply the held IORequest, advance the CQ-head doorbell.
 *
 * Transfer strategy — see should_use_bounce() below:
 *
 *   - Bounce path — small transfers (≤ a few pages) or unaligned user
 *     buffers.  CPU memcpy into the slot's pre-pinned bounce, no per-
 *     I/O StartDMA.  Fast for cases where the memcpy cost is below
 *     StartDMA's fixed overhead.
 *
 *   - Direct path — aligned multi-page transfers and everything above
 *     NVME_BOUNCE_SIZE.  StartDMA + GetDMAList on the user buffer,
 *     reusing the slot's pre-allocated DMAEntry pool (no per-I/O
 *     AllocSysObject).  Skips the memcpy entirely.
 *
 * PRP construction follows NVMe 1.4 §4.3 rules — see nvme_io.h for the
 * one / two / >two-page branching table.
 */

#include "nvme_io.h"
#include "nvme_device.h"
#include "nvme_status.h"
#include <exec/exec.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <devices/newstyle.h>
#include <string.h>

/* NVMe DMA buffer accesses (SQE writes, CQE reads) must use LE byte-swap on PPC */
static inline void dma_w32(void *addr, ULONG val)
{
    __asm__ volatile ("stwbrx %0, 0, %1" : : "r"(val), "r"(addr) : "memory");
}

static inline ULONG dma_r32(const void *addr)
{
    ULONG val;
    __asm__ volatile ("lwbrx %0, 0, %1" : "=r"(val) : "r"(addr));
    return val;
}

static void write_io_sqe(struct nvme_sqe *dst, const struct nvme_sqe *src)
{
    const ULONG *s = (const ULONG *)src;
    ULONG       *d = (ULONG *)dst;
    for (int i = 0; i < 16; i++)
        dma_w32(&d[i], s[i]);
}

/* Find a free inflight slot for unit. Returns index or -1 if full. */
static LONG find_free_slot(struct NVMeUnit *unit)
{
    for (int i = 0; i < NVME_MAX_INFLIGHT; i++) {
        if (unit->inflight[i].ioreq == NULL)
            return i;
    }
    return -1;
}

/*
 * Pick the DMA path for this transfer.
 *
 * The bounce path amortises a persistent DMA pin: at submit time we
 * only pay a memcpy between the user buffer and a pre-pinned bounce
 * (O(byte_length) CPU cost, typically ~200 MB/s on PPC G3/G4).
 *
 * The direct path skips the memcpy but pays StartDMA/GetDMAList/EndDMA
 * per I/O.  On AmigaOS 4 those calls include cache-maintenance work
 * proportional to the buffer size, but include a non-trivial fixed
 * setup cost that tends to dominate for tiny transfers.
 *
 * Heuristic:
 *   - byte_length > NVME_BOUNCE_SIZE      → direct (must — the bounce
 *                                           is too small to hold it).
 *   - byte_length < NVME_DIRECT_MIN_SIZE  → bounce (the fixed setup
 *                                           cost of StartDMA dominates
 *                                           — memcpy is cheaper here).
 *   - unaligned buffer                    → bounce (simpler PRP list
 *                                           from a single contiguous
 *                                           allocation; also sidesteps
 *                                           the StartDMA/GetDMAList
 *                                           scatter-gather walk for
 *                                           awkward alignments).
 *   - aligned medium transfer             → direct (the memcpy would
 *                                           cost more than StartDMA).
 *
 * "Aligned" here means page-aligned — so the first physical page of
 * the direct-path transfer starts at offset 0 within its page, which
 * maximally simplifies PRP1 / PRP2 accounting.
 */
static BOOL should_use_bounce(ULONG byte_length, APTR data_virt,
                              ULONG page_size)
{
    /* Must-direct: doesn't fit in a single bounce slot. */
    if (byte_length > NVME_BOUNCE_SIZE)
        return FALSE;

    /* Must-bounce: below the crossover, the memcpy is cheaper than
     * the fixed per-I/O cost of the direct path. */
    ULONG min_direct = NVME_DIRECT_MIN_PAGES * page_size;
    if (byte_length < min_direct)
        return TRUE;

    /* Medium range — bounce only if the buffer isn't page-aligned.
     * Aligned transfers can go direct and skip the memcpy. */
    ULONG page_offset = ((ULONG)data_virt) & (page_size - 1);
    return (page_offset != 0);
}

/*
 * NVMeIO_RingSQ — publish the current shadow tail to the device.
 * Idempotent; the doorbell register tolerates redundant writes of the
 * same value.
 */
void NVMeIO_RingSQ(struct NVMeUnit *unit)
{
    struct NVMeController *ctrl = unit->ctrl;
    NVME_W32_DB(ctrl->pciDevice, ctrl->iobase,
                NVME_SQ_TAIL_DB(unit->queue_id, ctrl->cap_dstrd),
                unit->io_sq_tail);
}


/*
 * NVMeIO_SubmitNoRing — reserve a slot and build the SQE without
 * ringing the SQ doorbell.  Callers batch-rings via NVMeIO_RingSQ.
 */
LONG NVMeIO_SubmitNoRing(struct NVMeBase *devBase, struct NVMeUnit *unit,
                         struct IOStdReq *ioreq, BOOL is_write)
{
    struct NVMeController *ctrl  = unit->ctrl;
    struct ExecIFace      *IExec = devBase->IExec;

    LONG slot = find_free_slot(unit);
    if (slot < 0)
        return -1; /* no free slot — caller falls back to synchronous */

    /* Compute LBA and block count from IOStdReq */
    uint64 byte_offset;
    ULONG byte_length = ioreq->io_Length;

    if (ioreq->io_Command == TD_READ64 || ioreq->io_Command == TD_WRITE64 ||
        ioreq->io_Command == NSCMD_TD_READ64 || ioreq->io_Command == NSCMD_TD_WRITE64 ||
        ioreq->io_Command == NSCMD_ETD_READ64 || ioreq->io_Command == NSCMD_ETD_WRITE64) {
        /* 64-bit offset: io_Offset = low 32 bits, io_Actual = high 32 bits */
        byte_offset = ((uint64)ioreq->io_Actual << 32) | (uint64)ioreq->io_Offset;
    } else {
        byte_offset = (uint64)ioreq->io_Offset;
    }

    /* Reject anything not block-aligned rather than silently rounding:
     * a truncated nlb would report io_Actual == io_Length for a partial
     * transfer, which a filesystem has no way to detect. */
    ULONG block_mask = unit->block_size - 1;
    if ((ULONG)(byte_offset & block_mask) != 0) {
        ioreq->io_Error  = IOERR_BADADDRESS;
        ioreq->io_Actual = 0;
        return -5;
    }
    if ((byte_length & block_mask) != 0 ||
        (byte_length != 0 && ioreq->io_Data == NULL)) {
        ioreq->io_Error  = IOERR_BADLENGTH;
        ioreq->io_Actual = 0;
        return -5;
    }

    uint64 start_lba = byte_offset >> unit->block_shift;
    ULONG nlb        = (byte_length >> unit->block_shift); /* block count */
    if (nlb == 0) {
        ioreq->io_Error  = 0;
        ioreq->io_Actual = 0;
        return -2; /* nothing to do */
    }

    struct NVMeInflight *inf = &unit->inflight[slot];
    inf->ioreq    = ioreq;
    inf->is_write = is_write;
    inf->use_bounce = FALSE;
    inf->dma_list   = NULL;
    inf->bounce_user_buf = NULL;
    inf->bounce_user_len = 0;
    inf->suppress_reply  = FALSE;

    APTR   data_virt = ioreq->io_Data;
    ULONG  prp1_phys, prp2_phys;

    /* Pick the DMA path.  Alignment-aware — see should_use_bounce(). */
    if (should_use_bounce(byte_length, data_virt, ctrl->page_size)) {
        unit->stats.bounce_hits++;
        inf->use_bounce = TRUE;
        /* Snapshot the user-buffer destination so Harvest's copy-back
         * works even if the caller later mutates its IORequest (the
         * MDTS chunking path in unit_task.c does exactly this). */
        inf->bounce_user_buf = data_virt;
        inf->bounce_user_len = byte_length;
        /* NB: we deliberately do NOT use C's memcpy() here or in
         * Harvest.  compat.c's fallback memcpy is a byte-by-byte loop
         * (we build with -nostartfiles so newlib isn't linked).  That
         * loop tops out at ~200 MB/s on PPC G3 / QEMU; for a 64 KiB
         * bounce transfer it dominates end-to-end latency.  Exec's
         * CopyMem is the canonical optimised block-copy — word- or
         * doubleword-wide inside, with byte-wise fall-off for
         * misaligned tails.  Argument order is Amiga-style: src, dst,
         * length.  IOS 4 also exposes CopyMemQuick (requires mutual
         * 4-byte alignment of source, destination, and length); we
         * stick with the general-purpose CopyMem because the user
         * buffer alignment is caller-supplied and not guaranteed. */
        /* The bounce buffer is permanently DMA-pinned (cache-inhibited)
         * from UnitTask_Start — no CacheClearE is needed in either
         * direction.  Writes go RAM-coherent immediately; reads return
         * the data the device DMAs in without any cache to invalidate. */
        if (is_write)
            IExec->CopyMem(data_virt, unit->bounce_bufs[slot], byte_length);

        prp1_phys = unit->bounce_phys[slot];

        /* Build PRP2 (or a PRP list) when the transfer spans more than
         * one page.  The bounce buffer is a single contiguous physical
         * allocation so the k-th page is simply bounce_phys + k*page_size
         * — no GetDMAList walk required.
         *
         * This path was a buffer overflow in every version from the
         * 4 KiB-to-64 KiB bounce bump until this fix: we used to leave
         * prp2_phys = 0 unconditionally, which worked when the bounce
         * was a single page, but hard-freezes the NVMe controller on
         * multi-page transfers because PRP2 is required by spec. */
        ULONG page_size = ctrl->page_size;

        if (byte_length <= page_size) {
            /* Fits in PRP1 alone. */
            prp2_phys = 0;
        } else if (byte_length <= 2 * page_size) {
            /* Two-page transfer: PRP2 points at page 2 directly. */
            prp2_phys = prp1_phys + page_size;
        } else {
            /* Three-or-more pages: PRP2 points at the pre-allocated
             * PRP list page, which we fill with the phys addresses of
             * pages 2..N. */
            ULONG num_pages   = (byte_length + page_size - 1) / page_size;
            ULONG extra_pages = num_pages - 1;   /* pages after PRP1 */
            ULONG max_prp     = page_size / 8;

            if (extra_pages > max_prp) {
                /* Shouldn't happen — NVME_BOUNCE_SIZE (64 KiB) / 4 KiB =
                 * 15 extra pages, and max_prp = 512, so we never come
                 * close.  Guard anyway so a future bump doesn't silently
                 * corrupt memory. */
                DPRINTF(IExec, "[nvme.device:io] bounce PRP list overflow:"
                        " %lu bytes → %lu extra pages > %lu\n",
                        byte_length, extra_pages, max_prp);
                inf->ioreq      = NULL;
                ioreq->io_Error = IOERR_BADLENGTH;
                return -4;
            }

            ULONG *prp_list = (ULONG *)unit->prp_list_pages[slot];
            for (ULONG p = 0; p < extra_pages; p++) {
                ULONG page_phys = prp1_phys + (p + 1) * page_size;
                dma_w32(&prp_list[p * 2],     page_phys);
                dma_w32(&prp_list[p * 2 + 1], 0);
            }
            prp2_phys = unit->prp_list_phys[slot];
            unit->stats.prp_list_hits++;
        }
    } else {
        unit->stats.direct_dma_hits++;
        /* Direct DMA — StartDMA on the user buffer itself.
         *   DMA_ReadFromRAM = device reads from RAM (write command)
         *   0               = device writes to RAM (read command)
         *
         * We reuse the slot's pre-allocated DMAEntry pool whenever the
         * reported entry count fits; a per-I/O AllocSysObjectTags call
         * is used as a safety fallback for the rare case where the user
         * buffer is so fragmented that it exceeds dma_entry_pool_capacity.
         * `dma_list_is_pool` is encoded in the top bit of dma_flags so
         * Harvest knows whether to FreeSysObject.  DMA_ReadFromRAM is
         * bit 3 so the upper bits are free to claim.
         *
         * Note: we deliberately do NOT cache StartDMA pins across I/Os
         * (the "speculative pin cache" experiment in the v1.63 prototype
         * is unsafe — StartDMA/EndDMA on AmigaOS 4 is cache-maintenance,
         * not page-pinning, and the OS gives us no way to detect when
         * the caller frees their buffer between submissions.  Holding a
         * cached pin across a buffer-free crashes inside CacheClearE's
         * `dcbi` loop on eviction).  Per-I/O StartDMA/EndDMA is the
         * correct contract for this OS. */
        ULONG flags = is_write ? DMA_ReadFromRAM : 0;
        ULONG entries = IExec->StartDMA(data_virt, byte_length, flags);
        if (entries == 0) {
            /* StartDMA refused — can't DMA this buffer.  Leave the slot
             * free and surface a clean error. */
            inf->ioreq = NULL;
            ioreq->io_Error = IOERR_ABORTED;
            return -3;
        }
        NVME_LEAK_INC(nvme_leak_dma);

        struct DMAEntry *dma_list = NULL;
        BOOL dma_list_is_pool = FALSE;

        if (entries <= unit->dma_entry_pool_capacity &&
            unit->dma_entry_pool[slot] != NULL) {
            /* Fast path — reuse the pre-allocated pool. */
            dma_list         = unit->dma_entry_pool[slot];
            dma_list_is_pool = TRUE;
        } else {
            /* Overflow or pool unavailable — fall back to per-I/O alloc. */
            dma_list = IExec->AllocSysObjectTags(ASOT_DMAENTRY,
                ASODMAE_NumEntries, entries, TAG_DONE);
            if (!dma_list) {
                IExec->EndDMA(data_virt, byte_length, flags);
                NVME_LEAK_DEC(nvme_leak_dma);
                inf->ioreq = NULL;
                ioreq->io_Error = IOERR_ABORTED;
                return -3;
            }
            NVME_LEAK_INC(nvme_leak_dmaentry);
        }

        IExec->GetDMAList(data_virt, byte_length, flags, dma_list);
        inf->dma_list    = dma_list;
        inf->dma_flags   = flags | (dma_list_is_pool ? 0x80000000u : 0u);
        inf->dma_buf     = data_virt;
        inf->dma_phys    = (ULONG)dma_list[0].PhysicalAddress;
        inf->dma_size    = byte_length;
        inf->dma_entries = entries;

        prp1_phys = inf->dma_phys;

        /* -------------------------------------------------------------
         * Build PRP2 / PRP list (NVMe 1.4 §4.3).
         *
         * PRP rules in one sentence:
         *   - PRP1 covers bytes from its offset within its page up to
         *     the end of that page (or the whole transfer, if shorter).
         *   - Every subsequent PRP entry points to one full, page-
         *     aligned physical page.
         *
         * Strategy: walk the scatter-gather list returned by
         * GetDMAList.  For every byte at a host-page boundary that
         * falls past PRP1's coverage, emit a PRP-list entry pointing
         * at the physical address of that page.  Each DMA entry is
         * physically contiguous for its BlockLength bytes, so the
         * k-th page inside an entry has physical address ebase +
         * k*page_size when the byte offset within the entry is a page
         * boundary.
         *
         * We write directly into the pre-allocated PRP list page
         * (64-bit little-endian entries, 8 bytes each, so the page
         * holds page_size/8 entries — 512 on a 4 KiB system).
         *
         * Outcomes:
         *   pg == 0 → transfer fits in PRP1 alone → PRP2 = 0.
         *   pg == 1 → PRP2 = that page's physical address directly
         *             (saves a memory access to the PRP list page).
         *   pg >  1 → PRP2 = phys of our PRP list page; the entries
         *             we wrote describe pages 2..N.
         *
         * If the page count exceeds what a single PRP list page can
         * hold we fail the I/O cleanly — a second-level PRP list is
         * allowed by the spec but not implemented here because the
         * MDTS splitting in unit_task.c ensures per-command transfers
         * never approach that size on sane hardware. */
        ULONG page_size     = ctrl->page_size;
        ULONG page_mask     = page_size - 1;
        ULONG prp1_offset   = prp1_phys & page_mask;
        ULONG first_page_bytes = (prp1_offset == 0) ? page_size
                                                    : (page_size - prp1_offset);
        ULONG max_prp_entries = page_size / 8;  /* 64-bit entries */

        if (byte_length <= first_page_bytes) {
            prp2_phys = 0;
        } else {
            ULONG *prp_list = (ULONG *)unit->prp_list_pages[slot];
            ULONG  pg       = 0;
            ULONG  first_extra_phys = 0;
            ULONG  global_off       = 0;   /* bytes consumed so far */
            BOOL   overflow         = FALSE;

            for (ULONG e = 0; e < inf->dma_entries; e++) {
                ULONG ebase     = (ULONG)inf->dma_list[e].PhysicalAddress;
                ULONG elen      = (ULONG)inf->dma_list[e].BlockLength;
                ULONG entry_end = global_off + elen;

                if (entry_end <= first_page_bytes) {
                    /* Entire entry lies within PRP1's page. */
                    global_off = entry_end;
                    continue;
                }

                ULONG scan = (global_off < first_page_bytes)
                           ? first_page_bytes : global_off;

                while (scan < entry_end) {
                    ULONG page_phys = ebase + (scan - global_off);

                    if (pg == 0)
                        first_extra_phys = page_phys;

                    if (pg >= max_prp_entries) {
                        overflow = TRUE;
                        break;
                    }
                    dma_w32(&prp_list[pg * 2],     page_phys);
                    dma_w32(&prp_list[pg * 2 + 1], 0);
                    pg++;
                    scan += page_size;
                }
                global_off = entry_end;
                if (overflow) break;
            }

            if (overflow) {
                DPRINTF(IExec, "[nvme.device:io] PRP list overflow:"
                        " %lu-byte transfer needs more than %lu PRP entries\n",
                        byte_length, max_prp_entries);
                IExec->EndDMA(data_virt, byte_length, flags);
                NVME_LEAK_DEC(nvme_leak_dma);
                /* Only free the DMAEntry array if this I/O owned it —
                 * the slot's pre-allocated pool must survive. */
                if (!dma_list_is_pool) {
                    IExec->FreeSysObject(ASOT_DMAENTRY, inf->dma_list);
                    NVME_LEAK_DEC(nvme_leak_dmaentry);
                }
                inf->dma_list = NULL;
                inf->ioreq    = NULL;
                ioreq->io_Error = IOERR_BADLENGTH;
                return -4;
            }

            if (pg == 1) {
                prp2_phys = first_extra_phys;
            } else {
                prp2_phys = unit->prp_list_phys[slot];
                unit->stats.prp_list_hits++;
            }
        }
    }

    /* Assign command ID = (slot + 1) to avoid 0 */
    UWORD cid = (UWORD)(slot + 1);
    inf->cmd_id = cid;

    /* Build I/O SQE and write with LE byte-swap */
    struct nvme_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.cdw0    = (is_write ? NVME_CMD_WRITE : NVME_CMD_READ) | ((ULONG)cid << 16);
    sqe.nsid    = unit->nsid;
    sqe.prp1_lo = prp1_phys;
    sqe.prp1_hi = 0;
    sqe.prp2_lo = prp2_phys;
    sqe.prp2_hi = 0;
    sqe.cdw10   = (ULONG)(start_lba & 0xFFFFFFFF);
    sqe.cdw11   = (ULONG)(start_lba >> 32);
    sqe.cdw12   = (nlb - 1) & 0xFFFF;

    struct nvme_sqe *sq = (struct nvme_sqe *)unit->io_sq;
    write_io_sqe(&sq[unit->io_sq_tail], &sqe);

    DPRINTF(IExec, "[nvme.device:io] Submit: %s slot=%ld lba=%lu nlb=%lu\n",
                       is_write ? "WRITE" : "READ", slot,
                       (ULONG)(start_lba & 0xFFFFFFFF), nlb);

    /* Latency clock sample + per-opcode / bytes / peak-inflight stats.
     * All touches are per-unit so no locking is needed — only the
     * unit's own task updates these. */
    {
        uint64 tbr = nvme_read_tbr();
        inf->submit_ticks_hi = (uint32)(tbr >> 32);
        inf->submit_ticks_lo = (uint32)(tbr & 0xFFFFFFFFu);
    }

    if (is_write) {
        unit->stats.writes++;
        unit->stats.write_bytes += (uint64)byte_length;
    } else {
        unit->stats.reads++;
        unit->stats.read_bytes  += (uint64)byte_length;
    }
    unit->stats.inflight_current++;
    if (unit->stats.inflight_current > unit->stats.inflight_peak)
        unit->stats.inflight_peak = unit->stats.inflight_current;

    /* Advance the shadow SQ tail.  The caller is responsible for
     * publishing it to the device via NVMeIO_RingSQ — we deliberately
     * do not ring here so that bursts of submissions can share a
     * single doorbell write. */
    unit->io_sq_tail = (unit->io_sq_tail + 1) % unit->queue_depth;

    return 0; /* submitted; Harvest will ReplyMsg when done */
}

/*
 * NVMeIO_Submit — convenience wrapper: submit + ring.
 *
 * Kept for callers that do not benefit from batching (single-shot
 * dispatch, chunked path that polls per chunk).  The event loop uses
 * NVMeIO_SubmitNoRing + a trailing NVMeIO_RingSQ so that a burst of
 * queued messages pays only one doorbell write.
 */
LONG NVMeIO_Submit(struct NVMeBase *devBase, struct NVMeUnit *unit,
                   struct IOStdReq *ioreq, BOOL is_write)
{
    LONG rc = NVMeIO_SubmitNoRing(devBase, unit, ioreq, is_write);
    if (rc == 0)
        NVMeIO_RingSQ(unit);
    return rc;
}

LONG NVMeIO_Flush(struct NVMeBase *devBase, struct NVMeUnit *unit,
                  struct IOStdReq *ioreq)
{
    struct ExecIFace *IExec = devBase->IExec;

    LONG slot = find_free_slot(unit);
    if (slot < 0) {
        DPRINTF(IExec, "[nvme.device:io] Flush: no free slot\n");
        return -1;
    }

    UWORD cid = (UWORD)(slot + 1);
    struct NVMeInflight *inf = &unit->inflight[slot];
    inf->ioreq           = ioreq;
    inf->cmd_id          = cid;
    inf->is_write        = FALSE;
    inf->use_bounce      = FALSE;
    inf->dma_list        = NULL;
    inf->bounce_user_buf = NULL;
    inf->bounce_user_len = 0;
    inf->suppress_reply  = FALSE;

    struct nvme_sqe sqe_flush;
    memset(&sqe_flush, 0, sizeof(sqe_flush));
    sqe_flush.cdw0 = NVME_CMD_FLUSH | ((ULONG)cid << 16);
    sqe_flush.nsid = unit->nsid;

    struct nvme_sqe *sq = (struct nvme_sqe *)unit->io_sq;
    write_io_sqe(&sq[unit->io_sq_tail], &sqe_flush);

    DPRINTF(IExec, "[nvme.device:io] Flush: slot=%ld cid=%u tail=%u\n",
            slot, cid, unit->io_sq_tail);

    {
        uint64 tbr = nvme_read_tbr();
        inf->submit_ticks_hi = (uint32)(tbr >> 32);
        inf->submit_ticks_lo = (uint32)(tbr & 0xFFFFFFFFu);
    }
    unit->stats.flushes++;
    unit->stats.inflight_current++;
    if (unit->stats.inflight_current > unit->stats.inflight_peak)
        unit->stats.inflight_peak = unit->stats.inflight_current;

    unit->io_sq_tail = (unit->io_sq_tail + 1) % unit->queue_depth;
    /* Flush is a control command — caller expects it to hit the wire
     * immediately, so ring the doorbell directly here rather than
     * asking the caller to chase a ring-after. */
    NVMeIO_RingSQ(unit);
    return 0;
}

/*
 * NVMeIO_SubmitAndWait — submit a caller-built SQE and block the unit
 * task until the controller replies.  Unlike NVMeIO_Submit this does
 * NOT hand the reply back to the normal async harvest path; the inflight
 * slot is flagged `suppress_reply` so Harvest only does its bookkeeping,
 * leaving the IOStdReq available to the caller for post-completion work
 * (typically: translate the NVMe status into SCSI scsi_Status / sense).
 *
 * The caller fills opcode / nsid / prp1 / prp2 / cdw10..15 on the SQE;
 * this function assigns the command ID.  For no-data commands
 * (Flush, DSM) the caller sets PRPs as required for that command; we
 * don't touch data buffers here.
 *
 * Polling strategy mirrors the chunked-path poll in unit_task.c:
 * repeatedly Harvest the CQ and yield the CPU to QEMU between passes
 * via Forbid/Permit.  A 5-second budget protects against a dead
 * controller.  Interrupt re-unmask is handled on each iteration for
 * parity with the event-loop yield-poll.
 */
UWORD NVMeIO_SubmitAndWait(struct NVMeBase *devBase, struct NVMeUnit *unit,
                           struct IOStdReq *ioreq, struct nvme_sqe *sqe)
{
    struct ExecIFace      *IExec = devBase->IExec;

    LONG slot = find_free_slot(unit);
    if (slot < 0) {
        DPRINTF(IExec, "[nvme.device:io] SubmitAndWait: no free slot\n");
        return 0xFFFEu;
    }

    UWORD cid = (UWORD)(slot + 1);
    struct NVMeInflight *inf = &unit->inflight[slot];
    inf->ioreq           = ioreq;
    inf->cmd_id          = cid;
    inf->is_write        = FALSE;
    inf->use_bounce      = FALSE;
    inf->dma_list        = NULL;
    inf->bounce_user_buf = NULL;
    inf->bounce_user_len = 0;
    inf->suppress_reply  = TRUE;  /* caller will reply after translating status */

    /* Embed the CID in cdw0[31:16] without disturbing the caller-supplied
     * opcode and fuse/PSDT bits in the low half. */
    sqe->cdw0 = (sqe->cdw0 & 0x0000FFFFu) | ((ULONG)cid << 16);

    struct nvme_sqe *sq = (struct nvme_sqe *)unit->io_sq;
    write_io_sqe(&sq[unit->io_sq_tail], sqe);

    DPRINTF(IExec, "[nvme.device:io] SubmitAndWait: slot=%ld cid=%u opcode=0x%02lx tail=%u\n",
            slot, cid, (ULONG)(sqe->cdw0 & 0xFFu), unit->io_sq_tail);

    {
        uint64 tbr = nvme_read_tbr();
        inf->submit_ticks_hi = (uint32)(tbr >> 32);
        inf->submit_ticks_lo = (uint32)(tbr & 0xFFFFFFFFu);
    }
    unit->stats.inflight_current++;
    if (unit->stats.inflight_current > unit->stats.inflight_peak)
        unit->stats.inflight_peak = unit->stats.inflight_current;

    unit->io_sq_tail = (unit->io_sq_tail + 1) % unit->queue_depth;
    NVMeIO_RingSQ(unit);

    /* Poll-harvest until our slot's ioreq is released (Harvest clears
     * ioreq unconditionally after processing; suppress_reply just stops
     * the ReplyMsg).  The NVMe status that Harvest decoded is folded
     * into ioreq->io_Error; on non-NVMe failure (timeout) we fabricate
     * an IOERR_ABORTED. */
    for (ULONG poll = 0; poll < 5000000UL; poll++) {
        NVMeIO_Harvest(devBase, unit);
        if (unit->inflight[slot].ioreq == NULL) {
            /* Slot cleared — completion observed.  The IORequest's
             * io_Error carries the translated status from the
             * NVMe_StatusToIOErr mapper; we don't know the raw 16-bit
             * NVMe status anymore (Harvest discarded it after mapping),
             * so fold it back into a synthetic 0/non-zero token. */
            return (ioreq->io_Error == 0) ? 0u : 0xFF00u;
        }
        if (unit->ctrl->irq_installed)
            nvme_w32(unit->ctrl->iobase + NVME_REG_INTMC, 1);
        /* Yield so QEMU's main loop can run the NVMe emulation BHs and
         * post the CQE we're waiting for. */
        IExec->Forbid();
        IExec->Permit();
    }

    /* Timeout — forcibly release the slot (otherwise it leaks forever)
     * and report.  Mask suppression off before clearing so a very-late
     * CQE doesn't touch a freed IORequest. */
    DPRINTF(IExec, "[nvme.device:io] SubmitAndWait: timeout (cid=%u slot=%ld)\n",
            cid, slot);
    unit->inflight[slot].suppress_reply = FALSE;
    unit->inflight[slot].ioreq          = NULL;
    if (unit->stats.inflight_current > 0)
        unit->stats.inflight_current--;
    return 0xFFFDu;
}

/* ISR counters — declared in nvme_irq.c */
extern volatile ULONG nvme_isr_count;
extern volatile ULONG nvme_isr_claimed;
extern volatile ULONG nvme_isr_not_ours;

void NVMeIO_Harvest(struct NVMeBase *devBase, struct NVMeUnit *unit)
{
    struct NVMeController *ctrl  = unit->ctrl;
    struct ExecIFace      *IExec = devBase->IExec;
    ULONG                  base  = ctrl->iobase;

    UBYTE *cq_base = (UBYTE *)unit->io_cq;
    ULONG harvested = 0;

    while (1) {
        UBYTE *entry = cq_base + unit->io_cq_head * NVME_CQE_SIZE;
        /* CQE dword3 (bytes 12-15) in LE: [15:0]=cid, [31:16]=status */
        ULONG dw3 = dma_r32(entry + 12);
        UWORD status_le = (UWORD)(dw3 >> 16);
        UWORD cid       = (UWORD)(dw3 & 0xFFFF);
        if (NVME_CQE_PHASE(status_le) != unit->io_cq_phase)
            break;

        UWORD status = status_le;

        /* Advance CQ head and ring doorbell */
        unit->io_cq_head = (unit->io_cq_head + 1) % unit->queue_depth;
        if (unit->io_cq_head == 0)
            unit->io_cq_phase ^= 1;
        NVME_W32_DB(ctrl->pciDevice, base, NVME_CQ_HEAD_DB(unit->queue_id, ctrl->cap_dstrd),
                    unit->io_cq_head);

        /* Find inflight slot by cid (cid = slot + 1) */
        LONG slot = (LONG)cid - 1;
        if (slot < 0 || slot >= NVME_MAX_INFLIGHT || !unit->inflight[slot].ioreq) {
            DPRINTF(IExec, "[nvme.device:io] Harvest: unmatched cid %u\n", cid);
            continue;
        }

        struct NVMeInflight *inf = &unit->inflight[slot];
        struct IOStdReq     *req = inf->ioreq;

        harvested++;
        DPRINTF(IExec, "[nvme.device:io] Harvest: slot=%ld cid=%u status=0x%04x phase=%u head=%u\n",
                           slot, cid, NVME_CQE_SC(status),
                           (ULONG)unit->io_cq_phase, (ULONG)unit->io_cq_head);

        /* Decode completion status via the shared mapper so every
         * completion path agrees on what each NVMe error becomes. */
        LONG ioerr = NVMe_StatusToIOErr(status);
        if (ioerr == 0) {
            req->io_Error  = 0;
            req->io_Actual = req->io_Length;
        } else {
            DPRINTF(IExec, "[nvme.device:io] Harvest: %s (status=0x%04x)"
                    " for slot %ld -> io_Error=%ld\n",
                    NVMe_StatusDescribe(status), status, slot, ioerr);
            req->io_Error  = ioerr;
            req->io_Actual = 0;
            /* Classify into stats error buckets. */
            if (ioerr == IOERR_ABORTED)          unit->stats.err_abort++;
            else                                  unit->stats.err_status++;
        }

        /* Latency accumulator: now-ticks minus submit-ticks. */
        {
            uint64 nowv = nvme_read_tbr();
            uint64 subv = ((uint64)inf->submit_ticks_hi << 32) |
                          (uint64)inf->submit_ticks_lo;
            if (nowv >= subv) {
                uint64 elapsed = nowv - subv;
                unit->stats.total_io_ticks += elapsed;
                if (elapsed > unit->stats.max_io_ticks)
                    unit->stats.max_io_ticks = elapsed;
            }
        }
        if (unit->stats.inflight_current > 0)
            unit->stats.inflight_current--;

        /* Clean up DMA / bounce — only copy back on a clean read.  The
         * bounce buffer is cache-inhibited so no CacheClearE is needed
         * before the memcpy.  Copy-back uses the Submit-time snapshot
         * so a caller that has since mutated its IORequest (the MDTS
         * chunking path does this) still targets the correct bytes. */
        if (inf->use_bounce) {
            /* Read-side copy-back uses Exec's CopyMem (not the compat.c
             * byte-by-byte memcpy) to keep bounce-path latency low;
             * see the rationale next to the write-side copy in Submit. */
            if (!inf->is_write && ioerr == 0 && inf->bounce_user_buf)
                IExec->CopyMem(unit->bounce_bufs[slot],
                               inf->bounce_user_buf,
                               inf->bounce_user_len);
        } else if (inf->dma_list) {
            /* Extract the original DMA direction flags and the pool-vs-
             * alloc marker written by Submit (top bit of dma_flags).
             * EndDMA must see the unmodified direction flags it started
             * with; FreeSysObject must run only for per-I/O allocations. */
            BOOL dma_list_is_pool = (inf->dma_flags & 0x80000000u) != 0;
            ULONG end_flags = inf->dma_flags & ~0x80000000u;
            IExec->EndDMA(inf->dma_buf, inf->dma_size, end_flags);
            NVME_LEAK_DEC(nvme_leak_dma);
            if (!dma_list_is_pool) {
                IExec->FreeSysObject(ASOT_DMAENTRY, inf->dma_list);
                NVME_LEAK_DEC(nvme_leak_dmaentry);
            }
            inf->dma_list = NULL;
        }

        /* If suppress_reply is set, a synchronous caller inside the
         * unit task (NVMeIO_SubmitAndWait) is poll-waiting on this
         * slot and will reply the IORequest once it has translated
         * the NVMe status into SCSI response fields.  Otherwise
         * reply here as usual. */
        BOOL was_suppressed = inf->suppress_reply;
        inf->suppress_reply = FALSE;
        inf->ioreq = NULL; /* free slot */
        if (!was_suppressed)
            IExec->ReplyMsg((struct Message *)req);
    }

    if (harvested > 0) {
        DPRINTF(IExec, "[nvme.device:io] Harvest: reaped %lu CQEs (ISR: total=%lu claimed=%lu not_ours=%lu)\n",
                harvested, nvme_isr_count, nvme_isr_claimed, nvme_isr_not_ours);
    }

    /* NOTE: INTMC unmask is handled by the unit task event loop, NOT here.
     * The ISR masks INTMS when it claims an interrupt. The task unmasks
     * INTMC after harvesting whenever it received the ISR signal — even
     * if Harvest found nothing (a prior poll-harvest may have consumed
     * the CQEs). Moving unmask here caused a deadlock: ISR masks INTMS,
     * Harvest finds nothing (reaped by earlier inline harvest), skips
     * unmask → interrupts stay masked forever. */
}
