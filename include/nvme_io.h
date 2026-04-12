#ifndef NVME_IO_H
#define NVME_IO_H

/*
 * nvme_io.h — I/O queue submission, flush, and harvest.
 *
 * Each NVMeUnit owns a dedicated I/O SQ/CQ pair (queue ID = unit_num+1).
 * I/O commands are asynchronous: BeginIO queues an IORequest to the
 * unit's message port, the unit task calls NVMeIO_Submit (below) which
 * reserves an inflight slot and rings the SQ tail doorbell; completion
 * is delivered by NVMeIO_Harvest, called from the unit task when the
 * ISR signals arrival of one or more CQEs.
 *
 * Data-transfer options depend on the transfer length and buffer
 * alignment (selected by should_use_bounce() in nvme_io.c):
 *
 *   Bounce path   — small (≤ NVME_DIRECT_MIN_PAGES × page_size) OR
 *                   unaligned buffer with size ≤ NVME_BOUNCE_SIZE.
 *                   CPU memcpy between the user buffer and a pre-
 *                   pinned bounce, no per-I/O StartDMA.
 *
 *   Direct path   — aligned buffer with size ≥ one page, or any
 *                   transfer > NVME_BOUNCE_SIZE.
 *                   StartDMA / GetDMAList on the user buffer using
 *                   the slot's pre-allocated DMAEntry pool (no per-I/O
 *                   AllocSysObject); build PRP1 / PRP2 or a PRP list;
 *                   EndDMA in Harvest.
 *
 * All PRP values are 64-bit device-physical addresses (little-endian in
 * the SQE), so PRP-list pages and DMA entry arrays all allocate from
 * MEMF_SHARED.
 *
 * PRP rules (NVMe 1.4 §4.3):
 *   - transfer fits in one page  → PRP1 = page phys, PRP2 = 0
 *   - transfer fits in two pages → PRP1 = page1 phys, PRP2 = page2 phys
 *   - transfer > two pages       → PRP1 = page1 phys,
 *                                  PRP2 = phys of a pre-allocated PRP
 *                                  list page holding the remainder
 */

#include "nvme_device.h"
#include <exec/io.h>

/*
 * NVMeIO_Submit — reserve an inflight slot, build an NVMe Read or Write
 * SQE for ioreq, and ring the SQ tail doorbell.
 *
 * Context: unit task only.  Must not be called from ISR.
 *
 * Returns  0 — slot reserved, IORequest held; completion will arrive via
 *               NVMeIO_Harvest → ReplyMsg.
 *         -1 — no free slot.  Caller should retry later (BeginIO.c
 *              converts this to IOERR_UNITBUSY).
 *       < -1 — hard error (io_Error already set, caller replies now).
 *
 * Internally this is a wrapper that calls NVMeIO_SubmitNoRing followed
 * immediately by NVMeIO_RingSQ; callers that need to batch several
 * submissions behind a single doorbell write should invoke those two
 * primitives directly instead.
 */
LONG NVMeIO_Submit(struct NVMeBase *devBase, struct NVMeUnit *unit,
                   struct IOStdReq *ioreq, BOOL is_write);

/*
 * NVMeIO_SubmitNoRing — identical semantics to NVMeIO_Submit but does
 * NOT ring the SQ tail doorbell.  The caller MUST follow up with a
 * call to NVMeIO_RingSQ(unit) before any submitted command can make
 * progress on the device; failing to ring leaves the I/O stalled.
 *
 * Intended for batched submission — e.g. the unit task's event loop
 * draining a burst of queued messages, where ringing once at the end
 * of the drain is measurably cheaper than ringing per message.
 *
 * Return values match NVMeIO_Submit exactly.
 */
LONG NVMeIO_SubmitNoRing(struct NVMeBase *devBase, struct NVMeUnit *unit,
                         struct IOStdReq *ioreq, BOOL is_write);

/*
 * NVMeIO_RingSQ — publish the unit's current SQ tail to the controller
 * via a doorbell write.  Safe (and idempotent) to call when no new
 * SQEs have been placed since the last ring — the controller tolerates
 * doorbell writes with an unchanged tail value.
 *
 * Context: unit task only (the tail counter is per-unit and unlocked).
 */
void NVMeIO_RingSQ(struct NVMeUnit *unit);

/*
 * NVMeIO_Flush — submit an NVMe Flush (opcode 0x00) on unit's I/O queue.
 * Same slot/async semantics as Submit; returns 0 on success, <0 on error.
 */
LONG NVMeIO_Flush(struct NVMeBase *devBase, struct NVMeUnit *unit,
                  struct IOStdReq *ioreq);

/*
 * NVMeIO_SubmitAndWait — synchronous variant for internal-to-driver
 * I/O commands that need to block the unit task until the controller
 * replies.  Used by the SCSI translation layer (SYNCHRONIZE CACHE,
 * UNMAP) where the SCSI response depends on the NVMe status.
 *
 * Caller supplies a pre-built SQE (opcode + nsid + prp1/prp2 + cdw10-15);
 * the CID is assigned internally.  The call blocks inside a poll-harvest
 * loop with a generous timeout, during which the slot is flagged
 * `suppress_reply` so the ordinary Harvest path does not ReplyMsg the
 * owning IORequest — the caller owns that and will reply it once it has
 * translated the NVMe status into SCSI fields.
 *
 * Context: unit task only.  Must not be called from the caller path
 * of a sync-wait command that is already in progress (would nest).
 *
 * Returns the NVMe 16-bit status word (0 = success; 0xFFFE = no free
 * slot; 0xFFFD = timeout; 0xFFxx = device-side NVMe error).
 */
UWORD NVMeIO_SubmitAndWait(struct NVMeBase *devBase, struct NVMeUnit *unit,
                           struct IOStdReq *ioreq, struct nvme_sqe *sqe);

/*
 * NVMeIO_Harvest — drain all pending CQEs on unit's I/O CQ.
 *
 * Context: unit task only, invoked when the unit's IRQ signal fires
 * (or on the polling tick when IRQs are not installed).  For each CQE
 * whose phase bit matches the expected value:
 *   - Match CID back to an inflight slot
 *   - Translate NVMe status → io_Error
 *   - Copy bounce → user buffer (on read completions) or EndDMA
 *   - Call ReplyMsg on the held IORequest
 *   - Advance the CQ head doorbell
 *
 * After draining, unmasks INTMC so the next CQE raises the line again.
 */
void NVMeIO_Harvest(struct NVMeBase *devBase, struct NVMeUnit *unit);

#endif /* NVME_IO_H */
