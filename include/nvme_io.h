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
 * alignment:
 *
 *   len ≤ NVME_BOUNCE_SIZE → copy through a pre-pinned bounce buffer
 *                            (one page allocated per inflight slot at
 *                            task start, reused indefinitely).
 *
 *   len > NVME_BOUNCE_SIZE → StartDMA/GetDMAList on the user buffer,
 *                            build PRP1/PRP2 or a PRP list page,
 *                            EndDMA in Harvest.
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
 */
LONG NVMeIO_Submit(struct NVMeBase *devBase, struct NVMeUnit *unit,
                   struct IOStdReq *ioreq, BOOL is_write);

/*
 * NVMeIO_Flush — submit an NVMe Flush (opcode 0x00) on unit's I/O queue.
 * Same slot/async semantics as Submit; returns 0 on success, <0 on error.
 */
LONG NVMeIO_Flush(struct NVMeBase *devBase, struct NVMeUnit *unit,
                  struct IOStdReq *ioreq);

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
