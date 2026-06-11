#ifndef NVME_VERSION_H
#define NVME_VERSION_H

/*
 * version.h — identification strings for nvme.device.
 *
 * BUILD_DATE and BUILD_TIME are injected by the Makefile on every build
 * so they always reflect the actual compile wall-clock time, never the
 * stale __DATE__ / __TIME__ of a cached translation unit.  If compiled
 * outside the Makefile, they fall back to literal "unknown".
 */

#define DEVNAME            "nvme.device"
#define DEVVER             1
#define DEVREV             68
#define DEVVERSIONSTRING   "nvme.device 1.68 (2026-06-11)"
#define VERSION_LOG_STRING "1.68"

#ifndef BUILD_DATE
#define BUILD_DATE "unknown"
#endif

#ifndef BUILD_TIME
#define BUILD_TIME "unknown"
#endif

#endif /* NVME_VERSION_H */
