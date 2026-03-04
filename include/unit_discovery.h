#ifndef UNIT_DISCOVERY_H
#define UNIT_DISCOVERY_H

#include "nvme_device.h"

/*
 * Enumerate NVMe namespaces, allocate NVMeUnit objects,
 * create per-unit I/O queues, and announce each unit to mounter.library.
 * Populates devBase->units[] and devBase->num_units.
 */
void DiscoverUnits(struct NVMeBase *devBase);

#endif /* UNIT_DISCOVERY_H */
