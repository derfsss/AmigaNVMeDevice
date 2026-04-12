#ifndef NVME_ADMIN_H
#define NVME_ADMIN_H

/*
 * nvme_admin.h — Admin Submission/Completion queue command helpers.
 *
 * All admin commands in this driver are serialised via devBase->io_lock
 * and use synchronous polling of the admin CQ phase bit (NVMe 1.4 §4.6).
 * The controller's admin interrupt is kept masked throughout Init so
 * that no IRQ fires before the per-unit tasks are running.
 *
 * Functions here are called only from Init-time code paths
 * (InitNVMe / DiscoverUnits); the I/O hot path never touches admin.
 *
 * NVMe specification references use "NVMe 1.4 §x.y" throughout this
 * code base.  Register field layouts match that revision.
 */

#include "nvme_device.h"

/*
 * NVMe_AdminCmd — submit one admin SQE to `ctrl` and poll for its
 * completion.  Caller fills opcode / NSID / PRP / cdwN; this function
 * assigns a command ID, writes the SQE with LE byte-swap, rings the
 * SQ tail doorbell, and spins on the matching CQE phase bit (~5 s
 * timeout).
 *
 * Caller must hold ctrl->io_lock.  Returns the 16-bit status word
 * (0 = success, 0xFFFF = timeout).
 */
UWORD NVMe_AdminCmd(struct NVMeController *ctrl, struct nvme_sqe *sqe);

/*
 * NVMe_IdentifyController — Admin Identify, CNS=1.
 * Populates ctrl->identify_buf and extracts MDTS into
 * ctrl->max_transfer_bytes.  Returns TRUE on success.
 */
BOOL NVMe_IdentifyController(struct NVMeController *ctrl);

/* Admin Identify, CNS=2 — fills caller-supplied array with up to
 * max_nsids active namespace IDs; returns count (0 on error). */
ULONG NVMe_IdentifyNSList(struct NVMeController *ctrl, ULONG *nsids, ULONG max_nsids);

/* Admin Identify, CNS=0, NSID=unit->nsid.  Populates block_size,
 * total_blocks, block_shift on *unit. */
BOOL NVMe_IdentifyNamespace(struct NVMeController *ctrl, struct NVMeUnit *unit);

/* Admin Create I/O Completion Queue (unit->queue_id). */
BOOL NVMe_CreateIOCQ(struct NVMeController *ctrl, struct NVMeUnit *unit);

/* Admin Create I/O Submission Queue, paired with the CQ of the same
 * queue ID. */
BOOL NVMe_CreateIOSQ(struct NVMeController *ctrl, struct NVMeUnit *unit);

#ifdef ENABLE_SMART
/* Admin Get Log Page 0x02 (SMART / Health Information, NVMe 1.4 §5.14.1.2).
 * Populates ctrl->smart_cache; stamps the TBR tick when done so the
 * next call within NVME_SMART_REFRESH_SECS is skipped.  Acquires
 * ctrl->io_lock internally; caller MUST NOT hold it. */
BOOL NVMe_RefreshSMART(struct NVMeController *ctrl);
#endif

#endif /* NVME_ADMIN_H */
