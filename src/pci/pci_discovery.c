/*
 * pci_discovery.c — enumerate every NVMe controller on the PCI bus,
 * map BAR0 for each, identify the host platform, and verify that each
 * bridge forwards MMIO to the device.
 *
 * Flow:
 *   1. One-time host-platform identification (informational).
 *   2. Loop FindDeviceTags(FDT_Class=0x010802, FDT_Index=n) with
 *      increasing n until no more match.  For each hit:
 *        a. Enable PCI memory + bus-master, clear INTx-disable
 *        b. GetResourceRange(0) → BAR0
 *        c. Apply MEMATTRF_CACHEINHIBIT|GUARDED
 *        d. Run NVMe_MMIOProbe on CAP_LO
 *      On success, populate devBase->controllers[n]; on failure clean
 *      up that slot and continue enumerating.
 *
 * Returns TRUE if at least one working controller was found.
 */

#include "pci/pci_discovery.h"
#include "nvme_platform.h"
#include "nvme_mmu.h"
#include <expansion/pci.h>
#include <exec/exec.h>

/* NVMe controllers are matched by PCI class code, not vendor/device ID:
 * base class 0x01 (mass storage), subclass 0x08 (NVM), progif 0x02
 * (NVM Express I/O command set).  Every real-silicon NVMe SSD and
 * QEMU's `-device nvme` advertise exactly this triple, so one scan
 * covers them all.  FDT_Class carries the full 24-bit code; the mask
 * selects all three bytes (same pattern usb2's HCD scan uses). */
#define NVME_PCI_CLASS       0x010802u
#define NVME_PCI_CLASS_MASK  0x00FFFFFFu

/* Smallest BAR0 a spec-conformant controller can expose: 4 KiB of
 * registers + at least one page of doorbells. */
#define NVME_BAR0_MIN_SIZE   0x2000u

BOOL DiscoverNVMe(struct NVMeBase *devBase)
{
    struct ExecIFace *IExec = devBase->IExec;
    struct PCIIFace  *IPCI  = devBase->IPCI;

    /* One-time platform identification. */
    devBase->platform = NVMe_PlatformDetect(IExec, IPCI);
    if (devBase->platform == NVME_PLATFORM_AMIGAONE_XE) {
        DLOG(IExec, "[nvme.device:pci] WARNING: AmigaOne XE Articia S does"
                    " not forward PCI MMIO — NVMe init is expected to fail.\n");
    }

    devBase->num_controllers = 0;

    for (ULONG idx = 0; idx < NVME_MAX_CONTROLLERS; idx++) {
        struct PCIDevice *dev = IPCI->FindDeviceTags(
            FDT_Class,     NVME_PCI_CLASS,
            FDT_ClassMask, NVME_PCI_CLASS_MASK,
            FDT_Index,     idx,
            TAG_DONE);
        if (!dev) break;   /* ran out of controllers */
        NVME_LEAK_INC(nvme_leak_pcidev);

        struct NVMeController *ctrl = &devBase->controllers[devBase->num_controllers];

        /* Initialise per-controller bookkeeping. */
        ctrl->dev_base = devBase;
        ctrl->ctrl_idx = devBase->num_controllers;
        IExec->InitSemaphore(&ctrl->io_lock);

        ctrl->pciDevice = dev;

        UWORD vid = dev->ReadConfigWord(PCI_VENDOR_ID);
        UWORD did = dev->ReadConfigWord(PCI_DEVICE_ID);

        /* Enable PCI Memory + Bus Master, and clear the PCI 2.3 INTx
         * disable bit — some firmwares leave it set, which would make
         * MapInterrupt-based INTx delivery silently dead and force the
         * driver into polling mode for no reason. */
        UWORD cmd = dev->ReadConfigWord(PCI_COMMAND);
        cmd |= PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
        cmd &= (UWORD)~PCI_COMMAND_INT_DISABLE;
        dev->WriteConfigWord(PCI_COMMAND, cmd);

        ctrl->bar0 = dev->GetResourceRange(0);
        if (!ctrl->bar0) {
            DLOG(IExec, "[nvme.device:pci] ctrl %lu (%04x:%04x):"
                        " GetResourceRange(0) failed; skipping\n",
                 ctrl->ctrl_idx, vid, did);
            IPCI->FreeDevice(dev);
            NVME_LEAK_DEC(nvme_leak_pcidev);
            ctrl->pciDevice = NULL;
            continue;
        }
        NVME_LEAK_INC(nvme_leak_resource);

        if (ctrl->bar0->Size != 0 && ctrl->bar0->Size < NVME_BAR0_MIN_SIZE) {
            DLOG(IExec, "[nvme.device:pci] ctrl %lu (%04x:%04x): BAR0 size"
                        " 0x%lx below NVMe minimum; skipping\n",
                 ctrl->ctrl_idx, vid, did, ctrl->bar0->Size);
            dev->FreeResourceRange(ctrl->bar0);
            NVME_LEAK_DEC(nvme_leak_resource);
            ctrl->bar0 = NULL;
            IPCI->FreeDevice(dev);
            NVME_LEAK_DEC(nvme_leak_pcidev);
            ctrl->pciDevice = NULL;
            continue;
        }

        DLOG(IExec, "[nvme.device:pci] ctrl %lu: BAR0 Base=0x%08lx"
                    " Phys=0x%08lx Size=%lu\n",
             ctrl->ctrl_idx, ctrl->bar0->BaseAddress, ctrl->bar0->Physical,
             ctrl->bar0->Size);

        if (ctrl->bar0->BaseAddress != 0)
            ctrl->iobase = ctrl->bar0->BaseAddress;
        else
            ctrl->iobase = ctrl->bar0->Physical;

        NVMe_MMU_SetupBAR(IExec, ctrl->bar0);

        ULONG cap_lo = 0;
        if (!NVMe_MMIOProbe(IExec, ctrl->iobase, &cap_lo)) {
            DLOG(IExec, "[nvme.device:pci] ctrl %lu: MMIO probe failed on %s;"
                        " skipping\n",
                 ctrl->ctrl_idx, NVMe_PlatformName(devBase->platform));
            dev->FreeResourceRange(ctrl->bar0);
            NVME_LEAK_DEC(nvme_leak_resource);
            ctrl->bar0 = NULL;
            IPCI->FreeDevice(dev);
            NVME_LEAK_DEC(nvme_leak_pcidev);
            ctrl->pciDevice = NULL;
            continue;
        }

        DLOG(IExec, "[nvme.device:pci] ctrl %lu: %04x:%04x iobase=0x%08lx"
                    " CAP_LO=0x%08lx\n",
             ctrl->ctrl_idx, vid, did, ctrl->iobase, cap_lo);

        devBase->num_controllers++;
    }

    if (devBase->num_controllers == 0) {
        DLOG(IExec, "[nvme.device:pci] No working NVMe controllers found"
                    " (platform: %s)\n",
             NVMe_PlatformName(devBase->platform));
        return FALSE;
    }

    DLOG(IExec, "[nvme.device:pci] %lu controller(s) enumerated on %s\n",
         devBase->num_controllers, NVMe_PlatformName(devBase->platform));
    return TRUE;
}
