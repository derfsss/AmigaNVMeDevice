#include "nvme_device.h"
#include "nvme_init.h"
#include "nvme_irq.h"
#include "unit_task.h"
#include <exec/exec.h>

BPTR _manager_Expunge(struct DeviceManagerInterface *Self)
{
    struct NVMeBase    *devBase = (struct NVMeBase *)Self->Data.LibBase;
    BPTR                seglist = (BPTR)NULL;
    struct ExecIFace   *IExec   = devBase->IExec;

    DPRINTF(IExec, "[nvme.device:Expunge] Entering with OpenCnt = %u\n",
            devBase->dev_Base.dd_Library.lib_OpenCnt);

    if (devBase->dev_Base.dd_Library.lib_OpenCnt == 0) {
        seglist = devBase->dev_SegList;

        IExec->Remove((struct Node *)devBase);

        RemoveNVMeInterrupt(devBase);
        CleanupNVMe(devBase);

        /* Shut down and free any still-open units */
        for (int i = 0; i < 8; i++) {
            if (devBase->units[i]) {
                UnitTask_Shutdown(devBase, devBase->units[i]);
                IExec->FreeVec(devBase->units[i]);
                devBase->units[i] = NULL;
            }
        }

        /* Free PCI resources */
        if (devBase->pciDevice) {
            if (devBase->bar0)
                devBase->pciDevice->FreeResourceRange(devBase->bar0);
            devBase->IPCI->FreeDevice(devBase->pciDevice);
            devBase->pciDevice = NULL;
        }

        if (devBase->IUtility)
            IExec->DropInterface((struct Interface *)devBase->IUtility);
        if (devBase->UtilityBase)
            IExec->CloseLibrary(devBase->UtilityBase);
        if (devBase->IPCI)
            IExec->DropInterface((struct Interface *)devBase->IPCI);
        if (devBase->ExpansionBase)
            IExec->CloseLibrary(devBase->ExpansionBase);

        IExec->DeleteLibrary((struct Library *)devBase);
    } else {
        DPRINTF(IExec, "[nvme.device:Expunge] Still in use — setting LIBF_DELEXP\n");
        devBase->dev_Base.dd_Library.lib_Flags |= LIBF_DELEXP;
    }

    return seglist;
}
