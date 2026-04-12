#ifndef PCI_DISCOVERY_H
#define PCI_DISCOVERY_H

/*
 * pci_discovery.h — Locate the NVMe controller on the PCI bus.
 *
 * Owned by src/pci/pci_discovery.c.  The implementation also drives
 * platform identification (NVMe_PlatformDetect), MMIO attribute setup
 * (NVMe_MMU_SetupBAR) and the MMIO forwarding probe (NVMe_MMIOProbe);
 * callers only see the one entry point below.
 *
 * The currently supported controller ID is QEMU's 1B36:0010 — real-
 * silicon NVMe devices (Samsung, WD, etc.) will be enumerated via a
 * class-based scan in the multi-controller commit.
 */

#include "nvme_device.h"

/*
 * DiscoverNVMe — scan PCI for the NVMe controller, map BAR0, identify
 * host platform, install MMU attributes, and verify MMIO forwarding.
 *
 * On success populates devBase->pciDevice, devBase->bar0, devBase->iobase,
 * devBase->platform.  On failure all acquired resources are released
 * before returning so the caller may simply abort Init.
 *
 * Returns TRUE if an NVMe controller was found AND its BAR0 is reachable.
 */
BOOL DiscoverNVMe(struct NVMeBase *devBase);

#endif /* PCI_DISCOVERY_H */
