#include "nvme_device.h"
/* CMD_READ / TD_READ64 / NSCMD_TD_READ64 are dispatched in unit_task.c
 * via NVMeIO_Submit(). This file is a placeholder for any read-specific
 * helper logic that may be needed in future (e.g. sector-granularity
 * alignment checks, partial-block handling). */
