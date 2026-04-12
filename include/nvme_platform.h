#ifndef NVME_PLATFORM_H
#define NVME_PLATFORM_H

/*
 * nvme_platform.h — host platform detection and MMIO sanity checks.
 *
 * NVMe is an MMIO-only transport: the controller exposes its doorbells
 * and capability registers via BAR0, and CPU memory cycles must reach
 * the device for the driver to function at all.  This module:
 *
 *   1) Identifies the host machine by scanning the PCI host bridge
 *      (best-effort — falls back to PLATFORM_UNKNOWN on unfamiliar
 *      hardware; non-fatal).
 *   2) Probes a candidate BAR0 for real MMIO forwarding.  Bridges that
 *      do not forward CPU memory cycles to PCI (notably the Articia S
 *      on the original AmigaOne XE/SE) fail this test and the driver
 *      cleanly aborts Init with a descriptive diagnostic.
 *
 * Platform tags are informational only — the MMIO probe result is the
 * authoritative pass/fail gate.  A machine we haven't catalogued here
 * will still initialise successfully as PLATFORM_UNKNOWN provided its
 * bridge forwards MMIO correctly.
 */

#include <exec/types.h>

struct NVMeBase;
struct ExecIFace;
struct PCIIFace;

/* Host platform identification (best-effort via PCI host bridge scan). */
typedef enum {
    NVME_PLATFORM_UNKNOWN = 0,
    NVME_PLATFORM_PEGASOS2,        /* Marvell MV64361 bridge (11AB:6460) */
    NVME_PLATFORM_SAM440,          /* AMCC 440EP/GR */
    NVME_PLATFORM_SAM460EX,        /* AMCC 460EX */
    NVME_PLATFORM_X1000,           /* P.A. Semi PA6T + Nemo */
    NVME_PLATFORM_X5000,           /* Freescale/NXP QorIQ P5020/P5040 */
    NVME_PLATFORM_A1222,           /* NXP QorIQ P1022 */
    NVME_PLATFORM_AMIGAONE_XE,     /* Mai Logic Articia S — MMIO NOT forwarded */
    NVME_PLATFORM_UNSUPPORTED      /* known-bad: MMIO probe failed */
} NVMePlatform;

/* Human-readable name for a platform tag.  Never returns NULL. */
const char *NVMe_PlatformName(NVMePlatform p);

/* Identify the host platform by probing the PCI host bridge at
 * bus 0, device 0, function 0.  Result is informational; the driver
 * never refuses to run purely on the platform tag. */
NVMePlatform NVMe_PlatformDetect(struct ExecIFace *IExec, struct PCIIFace *IPCI);

/* Probe a candidate BAR0 MMIO base for working CPU-to-device forwarding.
 *
 * Reads CAP_LO (0x00) and validates the result is neither all-zeros,
 * all-ones, nor the distinctive 0xA5A55A5A sentinel that Pegasos2's PCI
 * InLong returns when accessed through the wrong path.  Also rejects
 * values where the MQES field (bits [15:0]) is 0, which is impossible
 * for any conformant controller (minimum 2-entry queue).
 *
 * Returns TRUE if MMIO appears healthy, FALSE if the bridge is not
 * forwarding cycles.  Caller must log context and abort Init on FALSE. */
BOOL NVMe_MMIOProbe(struct ExecIFace *IExec, ULONG iobase, ULONG *cap_lo_out);

#endif /* NVME_PLATFORM_H */
