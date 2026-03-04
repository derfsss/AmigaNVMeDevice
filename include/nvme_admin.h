#ifndef NVME_ADMIN_H
#define NVME_ADMIN_H

#include "nvme_device.h"

/*
 * Submit a single admin command synchronously (polling).
 * Builds the SQE at admin_sq[admin_sq_tail], rings the doorbell,
 * polls the admin CQ for the matching completion.
 * Returns the 16-bit completion status word (0 = success).
 */
UWORD NVMe_AdminCmd(struct NVMeBase *devBase, struct nvme_sqe *sqe);

/*
 * Identify Controller → fills devBase->identify_buf (4KB).
 * Extracts mdts and populates devBase fields.
 */
BOOL NVMe_IdentifyController(struct NVMeBase *devBase);

/*
 * Identify Active Namespace List → fills devBase->identify_buf.
 * Returns number of NSIDs found (0 on error).
 */
ULONG NVMe_IdentifyNSList(struct NVMeBase *devBase, ULONG *nsids, ULONG max_nsids);

/*
 * Identify Namespace nsid → fills devBase->identify_buf.
 * Extracts block_size and total_blocks, writes into *unit.
 */
BOOL NVMe_IdentifyNamespace(struct NVMeBase *devBase, struct NVMeUnit *unit);

/*
 * Create I/O Completion Queue for unit.
 */
BOOL NVMe_CreateIOCQ(struct NVMeBase *devBase, struct NVMeUnit *unit);

/*
 * Create I/O Submission Queue for unit.
 */
BOOL NVMe_CreateIOSQ(struct NVMeBase *devBase, struct NVMeUnit *unit);

#endif /* NVME_ADMIN_H */
