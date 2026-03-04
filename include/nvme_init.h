#ifndef NVME_INIT_H
#define NVME_INIT_H

#include "nvme_device.h"

/*
 * Full NVMe controller initialisation:
 *   - Read CAP, disable controller, allocate admin queues, enable controller
 *   - Identify Controller, Identify Active Namespace List
 *   - For each namespace: Identify Namespace, Create I/O CQ + SQ
 * Returns TRUE on success.
 */
BOOL InitNVMe(struct NVMeBase *devBase);

/*
 * Graceful controller shutdown and resource cleanup.
 * Disables controller (CC.SHN normal shutdown), frees DMA buffers.
 */
void CleanupNVMe(struct NVMeBase *devBase);

#endif /* NVME_INIT_H */
