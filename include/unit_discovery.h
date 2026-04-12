#ifndef UNIT_DISCOVERY_H
#define UNIT_DISCOVERY_H

/*
 * unit_discovery.h — Namespace enumeration and unit creation.
 *
 * DiscoverUnits walks the active NSID list returned by Identify CNS=2
 * and, for each namespace, allocates a NVMeUnit, sets up its I/O SQ/CQ
 * pair, issues the Admin Create CQ + Create SQ commands to register the
 * queues with the controller, and announces the unit to mounter.library
 * so diskboot.kmod and SmartFilesystem can pick it up.
 *
 * Unit tasks are NOT started here — they start lazily on the first
 * Open() of each unit (see src/Open.c).
 */

#include "nvme_device.h"

/* Enumerate namespaces on one controller: populates ctrl->units[] and
 * appends each unit to devBase->global_units[].  Returns the number
 * of units successfully brought up. */
ULONG DiscoverUnits(struct NVMeController *ctrl);

#endif /* UNIT_DISCOVERY_H */
