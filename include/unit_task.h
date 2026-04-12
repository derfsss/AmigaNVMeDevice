#ifndef UNIT_TASK_H
#define UNIT_TASK_H

/*
 * unit_task.h — Per-unit asynchronous I/O task management.
 *
 * Every opened unit gets its own exec Task that owns the unit's message
 * port, NVME_MAX_INFLIGHT DMA-pinned bounce buffers, and NVME_MAX_INFLIGHT
 * PRP-list pages.  BeginIO dispatches I/O requests to that port; the
 * task submits them to the NVMe queue via NVMeIO_Submit, sleeps on
 * its signal mask, and harvests completions when the ISR signals it.
 *
 * Task start-up uses a handshake: the parent allocates a ready-signal
 * bit, hands it to the task via tc_UserData, and blocks until the
 * task confirms it has its resources before returning.  This lets
 * Open() return a guaranteed-usable unit.
 *
 * Shutdown is symmetric: the parent sets task_shutdown + raises
 * SIGBREAKF_CTRL_C, then busy-waits until the task clears unit->task
 * back to NULL (Delay() is dos.library and is unavailable in driver
 * context).
 */

#include "nvme_device.h"

/* Spawn the unit's I/O task.  Allocates per-slot bounce buffers and
 * PRP list pages, starts the task, and blocks until it signals ready.
 * Returns TRUE on success; caller should reply IOERR_OPENFAIL on FALSE. */
BOOL UnitTask_Start(struct NVMeBase *devBase, struct NVMeUnit *unit);

/* Request the unit task exit, wait for it to clear unit->task, then
 * free per-slot bounce buffers and PRP list pages. */
void UnitTask_Shutdown(struct NVMeBase *devBase, struct NVMeUnit *unit);

#endif /* UNIT_TASK_H */
