#include "nvme_device.h"
/* CMD_WRITE / TD_WRITE64 / NSCMD_TD_WRITE64 are dispatched in unit_task.c
 * via NVMeIO_Submit(). This file is a placeholder for write-specific helpers
 * (e.g. write-protect checks, write-verify mode). */
