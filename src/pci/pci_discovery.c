/*
 * pci_discovery.c — enumerate every NVMe controller on the PCI bus,
 * map BAR0 for each, identify the host platform, and verify that each
 * bridge forwards MMIO to the device.
 *
 * Flow:
 *   1. One-time host-platform identification (informational).
 *   2. Loop FindDeviceTags(VID=1B36, DID=0010, FDT_Index=n) with
 *      increasing n until no more match.  For each hit:
 *        a. Enable PCI memory + bus-master
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

/* QEMU NVMe PCI IDs — real-silicon NVMe uses per-vendor IDs and will
 * need class-code scanning (class 0x01, subclass 0x08, progif 0x02);
 * that is a later enhancement. */
#define NVME_PCI_VENDOR_ID   0x1B36
#define NVME_PCI_DEVICE_ID   0x0010

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
            FDT_VendorID, NVME_PCI_VENDOR_ID,
            FDT_DeviceID, NVME_PCI_DEVICE_ID,
            FDT_Index,    idx,
            TAG_DONE);
        if (!dev) break;   /* ran out of controllers */
        NVME_LEAK_INC(nvme_leak_pcidev);

        struct NVMeController *ctrl = &devBase->controllers[devBase->num_controllers];

        /* Initialise per-controller bookkeeping. */
        ctrl->dev_base = devBase;
        ctrl->ctrl_idx = devBase->num_controllers;
        IExec->InitSemaphore(&ctrl->io_lock);

        ctrl->pciDevice = dev;

        /* Enable PCI Memory + Bus Master. */
        UWORD cmd = dev->ReadConfigWord(PCI_COMMAND);
        dev->WriteConfigWord(PCI_COMMAND,
                             cmd | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);

        ctrl->bar0 = dev->GetResourceRange(0);
        if (!ctrl->bar0) {
            DLOG(IExec, "[nvme.device:pci] ctrl %lu: GetResourceRange(0)"
                        " failed; skipping\n", ctrl->ctrl_idx);
            IPCI->FreeDevice(dev);
            NVME_LEAK_DEC(nvme_leak_pcidev);
            ctrl->pciDevice = NULL;
            continue;
        }
        NVME_LEAK_INC(nvme_leak_resource);

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
             ctrl->ctrl_idx, (UWORD)NVME_PCI_VENDOR_ID,
             (UWORD)NVME_PCI_DEVICE_ID, ctrl->iobase, cap_lo);

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
