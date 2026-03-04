#ifndef PCI_DISCOVERY_H
#define PCI_DISCOVERY_H

#include "nvme_device.h"

/*
 * Scans PCI bus for a QEMU NVMe controller (1B36:0010),
 * populates devBase->pciDevice, devBase->bar0, devBase->iobase.
 * Returns TRUE on success, FALSE if no device found.
 */
BOOL DiscoverNVMe(struct NVMeBase *devBase);

#endif /* PCI_DISCOVERY_H */
