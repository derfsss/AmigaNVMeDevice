#ifndef NVME_IO_H
#define NVME_IO_H

#include "nvme_device.h"
#include <exec/io.h>

/*
 * Submit a block read or write I/O command to the unit's I/O queue.
 * Finds a free inflight slot, builds the NVMe read/write SQE,
 * DMA-maps the data buffer (or uses bounce buffer if <= NVME_BOUNCE_SIZE),
 * and rings the I/O SQ tail doorbell.
 *
 * Returns  0 on success (IORequest held; unit task calls ReplyMsg on completion)
 *         -1 if no free inflight slot (caller should fall back to synchronous)
 *         <0 on hard error (io_Error set, caller should ReplyMsg immediately)
 */
LONG NVMeIO_Submit(struct NVMeBase *devBase, struct NVMeUnit *unit,
                   struct IOStdReq *ioreq, BOOL is_write);

/*
 * Submit an NVMe Flush command (CMD_UPDATE).
 */
LONG NVMeIO_Flush(struct NVMeBase *devBase, struct NVMeUnit *unit,
                  struct IOStdReq *ioreq);

/*
 * Drain the I/O completion queue for unit.
 * For each completed CQE: match cmd_id to inflight slot, set io_Error/io_Actual,
 * clean up DMA, call IExec->ReplyMsg(). Ring CQ head doorbell.
 * Called from unit task when ISR fires io_signal_mask.
 */
void NVMeIO_Harvest(struct NVMeBase *devBase, struct NVMeUnit *unit);

#endif /* NVME_IO_H */
