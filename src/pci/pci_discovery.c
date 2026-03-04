#include "pci/pci_discovery.h"
#include <expansion/pci.h>

/* QEMU NVMe PCI IDs */
#define NVME_PCI_VENDOR_ID   0x1B36
#define NVME_PCI_DEVICE_ID   0x0010

BOOL DiscoverNVMe(struct NVMeBase *devBase)
{
    struct ExecIFace *IExec = devBase->IExec;
    struct PCIIFace  *IPCI  = devBase->IPCI;

    struct PCIDevice *dev = IPCI->FindDeviceTags(
        FDT_VendorID, NVME_PCI_VENDOR_ID,
        FDT_DeviceID, NVME_PCI_DEVICE_ID,
        TAG_DONE);

    if (!dev) {
        iexec_dbg(IExec, "[nvme.device:pci] No NVMe controller found (1B36:0010)\n");
        return FALSE;
    }

    devBase->pciDevice = dev;

    /* Obtain BAR0 (NVMe register MMIO space) */
    devBase->bar0 = dev->GetResourceRange(0);
    if (!devBase->bar0) {
        IExec->DebugPrintF("[nvme.device:pci] Failed to get BAR0\n");
        IPCI->FreeDevice(dev);
        devBase->pciDevice = NULL;
        return FALSE;
    }

    devBase->iobase = (ULONG)devBase->bar0->Physical;

    /* Enable PCI Memory + Bus Master */
    UWORD cmd = dev->ReadConfigWord(PCI_COMMAND);
    if (!(cmd & (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER))) {
        dev->WriteConfigWord(PCI_COMMAND, cmd | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    }

    IExec->DebugPrintF("[nvme.device:pci] Found NVMe 1B36:0010 at BAR0 phys 0x%08lx\n",
                       devBase->iobase);
    return TRUE;
}
