/*
 * platform_detect.c — identify the host PPC machine via PCI host bridge
 * scan, and sanity-check NVMe BAR0 MMIO forwarding.
 *
 * The host bridge lives at PCI bus 0, device 0, function 0 on every
 * AmigaOS 4 PPC target.  Matching its vendor/device ID against the
 * known table below yields a platform tag used in diagnostics.
 *
 * The MMIO probe is the real gate: on machines whose PCI bridge does
 * not forward CPU memory cycles (original AmigaOne XE with Articia S),
 * every register read returns a telltale garbage value and the driver
 * aborts Init cleanly rather than hanging.
 */

#include "nvme_platform.h"
#include "nvme_device.h"
#include "nvme_debug.h"

#include <exec/exec.h>
#include <expansion/pci.h>
#include <interfaces/expansion.h>
#include <proto/exec.h>

/* ------------------------------------------------------------------ */
/* Known host-bridge signatures                                        */
/* ------------------------------------------------------------------ */

struct BridgeEntry {
    UWORD        vid;
    UWORD        did;
    NVMePlatform platform;
    const char  *name;
};

static const struct BridgeEntry bridge_table[] = {
    /* Pegasos II — Marvell Discovery-II (MV64361) system controller. */
    { 0x11AB, 0x6460, NVME_PLATFORM_PEGASOS2,    "Pegasos II (MV64361)"     },

    /* AmigaOne XE / SE / micro — Mai Logic Articia S (VID 0x10CC).
     * NOTE: REAL Articia S silicon does not reliably forward CPU memory
     * cycles to PCI memory BARs, so MMIO-only devices are expected to
     * fail the probe below on real boards.  Emulators (QEMU's amigaone
     * machine) forward MMIO fine and the driver is fully functional
     * there once the half-programmed 64-bit BAR is repaired in
     * pci_discovery.c.  The platform tag is diagnostic only — the MMIO
     * probe remains the authoritative gate either way. */
    { 0x10CC, 0x0660, NVME_PLATFORM_AMIGAONE_XE, "AmigaOne XE (Articia S)"  },

    /* Sam440ep / Sam460ex — AMCC / Applied Micro 440EP / 460EX SoC PCIe.
     * Different core IDs; both report as AMCC (0x10E8) host bridge. */
    { 0x10E8, 0x7460, NVME_PLATFORM_SAM460EX,    "Sam460ex (AMCC 460EX)"    },
    { 0x10E8, 0x7440, NVME_PLATFORM_SAM440,      "Sam440ep (AMCC 440EP)"    },

    /* X1000 — P.A. Semi PA6T-1682M "Nemo". */
    { 0x1959, 0x000A, NVME_PLATFORM_X1000,       "X1000 (PA6T Nemo)"        },

    /* X5000 / Cyrus+ — NXP/Freescale QorIQ P5020 or P5040. */
    { 0x1957, 0x0420, NVME_PLATFORM_X5000,       "X5000 (QorIQ P5020)"      },
    { 0x1957, 0x0440, NVME_PLATFORM_X5000,       "X5000 (QorIQ P5040)"      },

    /* A1222 "Tabor" — NXP QorIQ P1022. */
    { 0x1957, 0x0109, NVME_PLATFORM_A1222,       "A1222 (QorIQ P1022)"      },

    { 0, 0, NVME_PLATFORM_UNKNOWN, NULL }
};

const char *NVMe_PlatformName(NVMePlatform p)
{
    switch (p) {
        case NVME_PLATFORM_PEGASOS2:     return "Pegasos II";
        case NVME_PLATFORM_SAM440:       return "Sam440ep";
        case NVME_PLATFORM_SAM460EX:     return "Sam460ex";
        case NVME_PLATFORM_X1000:        return "X1000";
        case NVME_PLATFORM_X5000:        return "X5000";
        case NVME_PLATFORM_A1222:        return "A1222";
        case NVME_PLATFORM_AMIGAONE_XE:  return "AmigaOne XE (Articia S)";
        case NVME_PLATFORM_UNSUPPORTED:  return "unsupported bridge";
        case NVME_PLATFORM_UNKNOWN:
        default:                         return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* Host-bridge scan                                                    */
/* ------------------------------------------------------------------ */

NVMePlatform NVMe_PlatformDetect(struct ExecIFace *IExec, struct PCIIFace *IPCI)
{
    if (!IPCI)
        return NVME_PLATFORM_UNKNOWN;

    /* Host bridge is bus 0, device 0, function 0 on every AmigaOS 4 PPC board. */
    struct PCIDevice *bridge = IPCI->FindDeviceTags(
        FDT_BusNr,      0,
        FDT_DeviceNr,   0,
        FDT_FunctionNr, 0,
        TAG_DONE);

    if (!bridge) {
        DLOG(IExec, "[nvme.device:platform] Host bridge @ 0:0:0 not"
                    " enumerated — platform unknown\n");
        return NVME_PLATFORM_UNKNOWN;
    }
    NVME_LEAK_INC(nvme_leak_pcidev);

    UWORD vid = bridge->ReadConfigWord(PCI_VENDOR_ID);
    UWORD did = bridge->ReadConfigWord(PCI_DEVICE_ID);

    /* Release the bridge handle — we only needed its VID/DID. */
    IPCI->FreeDevice(bridge);
    NVME_LEAK_DEC(nvme_leak_pcidev);

    for (const struct BridgeEntry *e = bridge_table; e->name; e++) {
        if (e->vid == vid && e->did == did) {
            DLOG(IExec, "[nvme.device:platform] Host bridge %04x:%04x"
                        " — %s\n", vid, did, e->name);
            return e->platform;
        }
    }

    DLOG(IExec, "[nvme.device:platform] Host bridge %04x:%04x"
                " — platform unknown (driver will attempt to run)\n",
         vid, did);
    return NVME_PLATFORM_UNKNOWN;
}

/* ------------------------------------------------------------------ */
/* BAR0 MMIO probe                                                     */
/* ------------------------------------------------------------------ */

BOOL NVMe_MMIOProbe(struct ExecIFace *IExec, ULONG iobase, ULONG *cap_lo_out)
{
    ULONG cap_lo = nvme_r32(iobase + NVME_REG_CAP_LO);
    if (cap_lo_out)
        *cap_lo_out = cap_lo;

    /* Four distinct failure modes observed on bridges that do not
     * forward MMIO correctly.  Any one of them marks the probe as
     * failed; we do not attempt to distinguish between them. */
    if (cap_lo == 0x00000000UL) {
        DLOG(IExec, "[nvme.device:platform] CAP_LO reads as 0x00000000"
                    " — bridge not forwarding MMIO\n");
        return FALSE;
    }
    if (cap_lo == 0xFFFFFFFFUL) {
        DLOG(IExec, "[nvme.device:platform] CAP_LO reads as 0xFFFFFFFF"
                    " — no device responding on MMIO\n");
        return FALSE;
    }
    if (cap_lo == 0xA5A55A5AUL) {
        DLOG(IExec, "[nvme.device:platform] CAP_LO reads as 0xA5A55A5A"
                    " — Pegasos2 I/O-port sentinel (wrong access path)\n");
        return FALSE;
    }

    /* MQES == 0 means a 1-entry queue, which is below the NVMe 1.0
     * minimum of 2 entries — treat as bogus. */
    ULONG mqes = NVME_CAP_MQES(cap_lo);
    if (mqes == 0) {
        DLOG(IExec, "[nvme.device:platform] CAP_LO MQES=0 (reg=0x%08lx)"
                    " — register read is likely bogus\n", cap_lo);
        return FALSE;
    }

    /* Strongest validity gate: the Version register must parse as a
     * plausible NVMe version (major 1 or 2, minor below 16, low byte
     * zero per spec layout MJR[31:16].MNR[15:8].TER[7:0] with TER only
     * used from 1.2.1 on — accept any TER).  A bridge that returns
     * stale bus data or open-bus noise for CAP_LO can slip past the
     * four sentinel checks above; it will not also produce a coherent
     * version number at offset 0x08. */
    ULONG vs  = nvme_r32(iobase + NVME_REG_VS);
    ULONG mjr = (vs >> 16) & 0xFFFF;
    ULONG mnr = (vs >> 8) & 0xFF;
    if (mjr < 1 || mjr > 2 || mnr > 15) {
        DLOG(IExec, "[nvme.device:platform] VS=0x%08lx is not a valid"
                    " NVMe version — register reads are bogus\n", vs);
        return FALSE;
    }

    return TRUE;
}
