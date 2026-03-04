#ifndef NVME_IRQ_H
#define NVME_IRQ_H

#include "nvme_device.h"

/*
 * Install PCI INTx interrupt handler via expansion.library.
 * On success: devBase->irq_installed = TRUE.
 * On failure: non-fatal — unit task falls back to polling.
 */
BOOL InstallNVMeInterrupt(struct NVMeBase *devBase);

/*
 * Remove the interrupt handler installed by InstallNVMeInterrupt.
 */
void RemoveNVMeInterrupt(struct NVMeBase *devBase);

#endif /* NVME_IRQ_H */
