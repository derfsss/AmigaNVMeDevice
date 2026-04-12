/*
 * nvme_io.c — I/O SQ submission, Flush, and CQ harvesting.
 *
 * The I/O hot path for every namespace.  A unit has NVME_MAX_INFLIGHT
 * pipelined command slots, each with its own pre-allocated bounce
 * buffer and PRP list page.  NVMeIO_Submit picks a free slot, builds
 * the SQE (opcode / PRP1 / PRP2 / LBA / NLB), and rings the doorbell.
 * NVMeIO_Harvest is the symmetric consumer: drain CQEs, match CID to
 * slot, translate status, reply the held IORequest, and advance the
 * CQ-head doorbell.
 *
 * Transfer strategy depends on size:
 *   - size ≤ NVME_BOUNCE_SIZE  → CPU copies into bounce, DMA cached
 *   - size > NVME_BOUNCE_SIZE  → StartDMA on the user buffer itself
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

/* Build and submit a read or write SQE for the given IOStdReq. */
LONG NVMeIO_Submit(struct NVMeBase *devBase, struct NVMeUnit *unit,
                   struct IOStdReq *ioreq, BOOL is_write)
{
    struct NVMeController *ctrl  = unit->ctrl;
    struct ExecIFace      *IExec = devBase->IExec;
    ULONG                  base  = ctrl->iobase;

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

    APTR   data_virt = ioreq->io_Data;
    ULONG  prp1_phys, prp2_phys;

    /* Choose bounce vs. direct DMA */
    if (byte_length <= NVME_BOUNCE_SIZE) {
        unit->stats.bounce_hits++;
        inf->use_bounce = TRUE;
        /* The bounce buffer is permanently DMA-pinned (cache-inhibited)
         * from UnitTask_Start — no CacheClearE is needed in either
         * direction.  Writes go RAM-coherent immediately; reads return
         * the data the device DMAs in without any cache to invalidate. */
        if (is_write)
            memcpy(unit->bounce_bufs[slot], data_virt, byte_length);

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
        /* Direct DMA: StartDMA on user buffer
         * DMA_ReadFromRAM = device reads from RAM (write command)
         * 0               = device writes to RAM (read command)   */
        ULONG flags = is_write ? DMA_ReadFromRAM : 0;
        ULONG entries = IExec->StartDMA(data_virt, byte_length, flags);
        NVME_LEAK_INC(nvme_leak_dma);
        inf->dma_list = IExec->AllocSysObjectTags(ASOT_DMAENTRY,
            ASODMAE_NumEntries, entries, TAG_DONE);
        if (!inf->dma_list) {
            IExec->EndDMA(data_virt, byte_length, flags);
            NVME_LEAK_DEC(nvme_leak_dma);
            inf->ioreq = NULL;
            ioreq->io_Error = IOERR_ABORTED;
            return -3;
        }
        NVME_LEAK_INC(nvme_leak_dmaentry);
        IExec->GetDMAList(data_virt, byte_length, flags, inf->dma_list);
        inf->dma_buf     = data_virt;
        inf->dma_phys    = (ULONG)inf->dma_list[0].PhysicalAddress;
        inf->dma_size    = byte_length;
        inf->dma_entries = entries;
        inf->dma_flags   = flags;

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

            for (ULONG e = 0; e < entries; e++) {
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
                IExec->FreeSysObject(ASOT_DMAENTRY, inf->dma_list);
                NVME_LEAK_DEC(nvme_leak_dmaentry);
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

    /* Advance SQ tail and ring doorbell */
    unit->io_sq_tail = (unit->io_sq_tail + 1) % unit->queue_depth;
    NVME_W32_DB(ctrl->pciDevice, base, NVME_SQ_TAIL_DB(unit->queue_id, ctrl->cap_dstrd),
                unit->io_sq_tail);

    return 0; /* submitted; Harvest will ReplyMsg when done */
}

LONG NVMeIO_Flush(struct NVMeBase *devBase, struct NVMeUnit *unit,
                  struct IOStdReq *ioreq)
{
    struct NVMeController *ctrl  = unit->ctrl;
    struct ExecIFace      *IExec = devBase->IExec;
    ULONG                  base  = ctrl->iobase;

    LONG slot = find_free_slot(unit);
    if (slot < 0) {
        DPRINTF(IExec, "[nvme.device:io] Flush: no free slot\n");
        return -1;
    }

    UWORD cid = (UWORD)(slot + 1);
    unit->inflight[slot].ioreq    = ioreq;
    unit->inflight[slot].cmd_id   = cid;
    unit->inflight[slot].is_write = FALSE;
    unit->inflight[slot].use_bounce = FALSE;
    unit->inflight[slot].dma_list   = NULL;

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
        unit->inflight[slot].submit_ticks_hi = (uint32)(tbr >> 32);
        unit->inflight[slot].submit_ticks_lo = (uint32)(tbr & 0xFFFFFFFFu);
    }
    unit->stats.flushes++;
    unit->stats.inflight_current++;
    if (unit->stats.inflight_current > unit->stats.inflight_peak)
        unit->stats.inflight_peak = unit->stats.inflight_current;

    unit->io_sq_tail = (unit->io_sq_tail + 1) % unit->queue_depth;
    NVME_W32_DB(ctrl->pciDevice, base, NVME_SQ_TAIL_DB(unit->queue_id, ctrl->cap_dstrd),
                unit->io_sq_tail);
    return 0;
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
         * before the memcpy. */
        if (inf->use_bounce) {
            if (!inf->is_write && ioerr == 0)
                memcpy(req->io_Data, unit->bounce_bufs[slot], req->io_Length);
        } else if (inf->dma_list) {
            IExec->EndDMA(inf->dma_buf, inf->dma_size, inf->dma_flags);
            NVME_LEAK_DEC(nvme_leak_dma);
            IExec->FreeSysObject(ASOT_DMAENTRY, inf->dma_list);
            NVME_LEAK_DEC(nvme_leak_dmaentry);
            inf->dma_list = NULL;
        }

        inf->ioreq = NULL; /* free slot */
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
