#include "nvme_device.h"
#include "pci/pci_discovery.h"
#include "nvme_init.h"
#include "nvme_irq.h"
#include "unit_discovery.h"
#include "version.h"
#include <exec/exec.h>

extern const APTR devInterfaces[];

struct Library *_manager_Init(struct Library *library, BPTR seglist, struct Interface *exec)
{
    struct NVMeBase *devBase = (struct NVMeBase *)library;
    struct ExecIFace *iexec = (struct ExecIFace *)exec;

    iexec->DebugPrintF("[nvme.device] Loading version: %s\n", VERSION_LOG_STRING);

    devBase->IExec      = iexec;
    devBase->dev_SegList = seglist;

    iexec->InitSemaphore(&devBase->io_lock);

    for (int i = 0; i < 8; i++)
        devBase->units[i] = NULL;
    devBase->num_units = 0;

    /* Open expansion.library and get PCI interface */
    devBase->ExpansionBase = iexec->OpenLibrary("expansion.library", 54);
    if (!devBase->ExpansionBase) {
        iexec->DebugPrintF("[nvme.device:Init] Failed to open expansion.library v54\n");
        return NULL;
    }
    devBase->IPCI = (struct PCIIFace *)iexec->GetInterface(devBase->ExpansionBase, "pci", 1, NULL);
    if (!devBase->IPCI) {
        iexec->DebugPrintF("[nvme.device:Init] Failed to get IPCI interface\n");
        iexec->CloseLibrary(devBase->ExpansionBase);
        return NULL;
    }

    /* Discover NVMe PCI device (1B36:0010) */
    if (!DiscoverNVMe(devBase)) {
        iexec->DropInterface((struct Interface *)devBase->IPCI);
        iexec->CloseLibrary(devBase->ExpansionBase);
        return NULL;
    }

    /* Open utility.library */
    devBase->UtilityBase = iexec->OpenLibrary("utility.library", 50);
    if (!devBase->UtilityBase) {
        iexec->DebugPrintF("[nvme.device:Init] Failed to open utility.library v50\n");
        devBase->IPCI->FreeDevice(devBase->pciDevice);
        iexec->DropInterface((struct Interface *)devBase->IPCI);
        iexec->CloseLibrary(devBase->ExpansionBase);
        return NULL;
    }
    devBase->IUtility = (struct UtilityIFace *)iexec->GetInterface(devBase->UtilityBase, "main", 1, NULL);
    if (!devBase->IUtility) {
        iexec->DebugPrintF("[nvme.device:Init] Failed to get IUtility interface\n");
        iexec->CloseLibrary(devBase->UtilityBase);
        devBase->IPCI->FreeDevice(devBase->pciDevice);
        iexec->DropInterface((struct Interface *)devBase->IPCI);
        iexec->CloseLibrary(devBase->ExpansionBase);
        return NULL;
    }

    /* Initialise NVMe controller (disable, alloc queues, enable, identify) */
    if (!InitNVMe(devBase)) {
        CleanupNVMe(devBase);
        iexec->DropInterface((struct Interface *)devBase->IUtility);
        iexec->CloseLibrary(devBase->UtilityBase);
        devBase->IPCI->FreeDevice(devBase->pciDevice);
        iexec->DropInterface((struct Interface *)devBase->IPCI);
        iexec->CloseLibrary(devBase->ExpansionBase);
        return NULL;
    }

    /* Install INTx interrupt handler (non-fatal — falls back to polling) */
    if (!InstallNVMeInterrupt(devBase)) {
        DPRINTF(iexec, "[nvme.device:Init] Interrupt install failed, using polling fallback\n");
    }

    /* Populate device library node */
    devBase->dev_Base.dd_Library.lib_Node.ln_Type = NT_DEVICE;
    devBase->dev_Base.dd_Library.lib_Node.ln_Pri  = 0;
    devBase->dev_Base.dd_Library.lib_Node.ln_Name = DEVNAME;
    devBase->dev_Base.dd_Library.lib_Flags        = LIBF_SUMUSED | LIBF_CHANGED;
    devBase->dev_Base.dd_Library.lib_Version      = DEVVER;
    devBase->dev_Base.dd_Library.lib_Revision     = DEVREV;
    devBase->dev_Base.dd_Library.lib_IdString     = DEVVERSIONSTRING;

    /* Enumerate namespaces and announce to mounter.library */
    DiscoverUnits(devBase);

    iexec->DebugPrintF("[nvme.device:Init] Initialised: %lu namespace(s) found\n", devBase->num_units);

    return (struct Library *)devBase;
}
