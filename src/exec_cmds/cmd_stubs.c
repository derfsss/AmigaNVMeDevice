#include "nvme_device.h"
/* Simple inline commands (motor, seek, geometry, change state, etc.)
 * are handled directly in BeginIO.c.
 * TD_GETGEOMETRY is handled in unit_task.c dispatch_ioreq().
 * This file is a placeholder for any future per-command helper logic. */
