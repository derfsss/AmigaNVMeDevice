#ifndef NVME_IRQ_H
#define NVME_IRQ_H

/*
 * nvme_irq.h — PCI INTx interrupt handler for NVMe.
 *
 * NVMe on QEMU and most consumer SSDs uses level-triggered PCI INTx
 * rather than MSI-X (which the AmigaOS 4 exec does not yet support).
 * The line is shared across PCI devices on most boards (Pegasos2 shares
 * it with virtioscsi, IDE, etc.), so the ISR must positively identify
 * that an NVMe CQE is pending before claiming the interrupt — otherwise
 * it swallows interrupts belonging to other drivers.
 *
 * Identification is done by peeking at each unit's I/O CQ phase bit;
 * no MMIO read of the controller status is required.
 *
 * Once NVMe work is found, the ISR writes INTMS to mask the vector
 * (preventing an IRQ storm while the task is still draining the CQ)
 * and signals each unit task.  The unit task's Harvest path unmasks
 * INTMC after the CQ has been fully drained.
 */

#include "nvme_device.h"

/*
 * InstallNVMeInterrupt — map an IRQ vector via ctrl->pciDevice, fill
 * ctrl->irq_handler, and AddIntServer the shared INTx line.  The
 * handler's is_Data points at the owning NVMeController so ISRs can
 * be registered independently per controller.
 *
 * Returns TRUE on success; FALSE is non-fatal — the unit tasks on
 * this controller switch to polling-mode automatically. */
BOOL InstallNVMeInterrupt(struct NVMeController *ctrl);

/* Reverse of Install.  Idempotent. */
void RemoveNVMeInterrupt(struct NVMeController *ctrl);

#endif /* NVME_IRQ_H */
