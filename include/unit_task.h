#ifndef UNIT_TASK_H
#define UNIT_TASK_H

#include "nvme_device.h"

/*
 * Start the per-unit I/O task. Called on first Open() of a unit.
 * Allocates pre-pinned bounce buffers and PRP list pages.
 * Signals the opening task when the unit task is ready.
 * Returns TRUE on success.
 */
BOOL UnitTask_Start(struct NVMeBase *devBase, struct NVMeUnit *unit);

/*
 * Shut down the per-unit I/O task. Called on last Close() of a unit.
 * Sends SIGBREAKF_CTRL_C to the unit task and busy-waits for it to exit.
 * Frees bounce buffers and PRP list pages.
 */
void UnitTask_Shutdown(struct NVMeBase *devBase, struct NVMeUnit *unit);

#endif /* UNIT_TASK_H */
