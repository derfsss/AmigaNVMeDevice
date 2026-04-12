#ifndef NVME_INIT_H
#define NVME_INIT_H

/*
 * nvme_init.h — NVMe controller reset, enable, and admin-queue setup.
 *
 * The InitNVMe → DiscoverUnits pair is the complete bring-up sequence:
 *
 *   InitNVMe(ctrl)
 *     1. Read CAP — MQES, DSTRD, MPSMIN (NVMe 1.4 §3.1.1)
 *     2. Disable controller (CC.EN=0); wait CSTS.RDY=0
 *     3. Allocate admin SQ + CQ, DMA-map, program ASQ/ACQ/AQA
 *     4. Program CC (page size, SQ/CQ entry sizes) and set CC.EN=1
 *     5. Wait CSTS.RDY=1
 *     6. Allocate the shared 4 KiB Identify scratch buffer
 *     7. Identify Controller (CNS=1) — extract MDTS, model, FW
 *
 *   DiscoverUnits(ctrl)  (in unit_discovery.c)
 *     8. Identify Active NSID List (CNS=2)
 *     9. For each NSID: Identify Namespace (CNS=0), allocate I/O SQ+CQ,
 *        Admin Create CQ, Admin Create SQ, start unit task, announce
 *        to mounter.library.
 *
 * All admin commands use polling; the interrupt line stays masked until
 * the unit tasks are up and able to service the completion signal.
 */

#include "nvme_device.h"

/* Full per-controller initialisation (steps 1-7).  Returns TRUE on
 * success.  Called once for each controller discovered on the PCI bus. */
BOOL InitNVMe(struct NVMeController *ctrl);

/* Graceful controller shutdown — issues CC.SHN normal shutdown, waits
 * for SHST=complete, then frees admin queues and the identify buffer. */
void CleanupNVMe(struct NVMeController *ctrl);

#endif /* NVME_INIT_H */
