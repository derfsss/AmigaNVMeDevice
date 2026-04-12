/*
 * Expunge.c — driver-manager Expunge vector (_manager_Expunge).
 *
 * Called when exec tries to reclaim the library's memory.  If OpenCnt
 * is non-zero we set LIBF_DELEXP and return 0 so the caller defers.
 * Once all opens have released we walk every controller and unit,
 * freeing resources in strict reverse-allocation order:
 *
 *   For each unit:
 *       Shutdown unit task (frees bounce/PRP pages, message port)
 *       Free unit struct
 *   For each controller:
 *       RemoveNVMeInterrupt, CleanupNVMe
 *       Free BAR0 resource range, release PCIDevice
 *   Close utility, expansion; DeleteLibrary.
 *
 * Returns the driver seglist on a true unload; returns 0 when deferred.
 */

#include "nvme_device.h"
#include "nvme_init.h"
#include "nvme_irq.h"
#include "unit_task.h"
#include <exec/exec.h>

BPTR _manager_Expunge(struct DeviceManagerInterface *Self)
{
    struct NVMeBase  *devBase = (struct NVMeBase *)Self->Data.LibBase;
    struct ExecIFace *IExec   = devBase->IExec;
    BPTR              seglist = (BPTR)NULL;

    DPRINTF(IExec, "[nvme.device:Expunge] OpenCnt=%u\n",
            devBase->dev_Base.dd_Library.lib_OpenCnt);

    if (devBase->dev_Base.dd_Library.lib_OpenCnt != 0) {
        devBase->dev_Base.dd_Library.lib_Flags |= LIBF_DELEXP;
        DPRINTF(IExec, "[nvme.device:Expunge] Deferred (LIBF_DELEXP set)\n");
        return 0;
    }

    seglist = devBase->dev_SegList;
    IExec->Remove((struct Node *)devBase);

    /* Shut down every unit task, free its I/O SQ/CQ backing, and
     * release the unit struct.  Walk the flat table since that's the
     * definitive enumeration. */
    ULONG sq_bytes = NVME_IO_QUEUE_DEPTH * NVME_SQE_SIZE;
    ULONG cq_bytes = NVME_IO_QUEUE_DEPTH * NVME_CQE_SIZE;
    for (ULONG u = 0; u < devBase->num_global_units; u++) {
        struct NVMeUnit *unit = devBase->global_units[u];
        if (!unit) continue;
        UnitTask_Shutdown(devBase, unit);

        if (unit->io_sq) {
            IExec->EndDMA(unit->io_sq, sq_bytes, DMA_ReadFromRAM);
            NVME_LEAK_DEC(nvme_leak_dma);
            IExec->FreeVec(unit->io_sq);
            NVME_LEAK_DEC(nvme_leak_vec);
            unit->io_sq = NULL;
        }
        if (unit->io_cq) {
            IExec->EndDMA(unit->io_cq, cq_bytes, DMA_ReadFromRAM);
            NVME_LEAK_DEC(nvme_leak_dma);
            IExec->FreeVec(unit->io_cq);
            NVME_LEAK_DEC(nvme_leak_vec);
            unit->io_cq = NULL;
        }

        IExec->FreeVec(unit);
        NVME_LEAK_DEC(nvme_leak_vec);
        devBase->global_units[u] = NULL;
    }
    devBase->num_global_units = 0;

    /* Per-controller IRQ + admin teardown + PCI resource release. */
    for (ULONG c = 0; c < devBase->num_controllers; c++) {
        struct NVMeController *ctrl = &devBase->controllers[c];

        RemoveNVMeInterrupt(ctrl);
        CleanupNVMe(ctrl);

        if (ctrl->pciDevice) {
            if (ctrl->bar0) {
                ctrl->pciDevice->FreeResourceRange(ctrl->bar0);
                NVME_LEAK_DEC(nvme_leak_resource);
            }
            devBase->IPCI->FreeDevice(ctrl->pciDevice);
            NVME_LEAK_DEC(nvme_leak_pcidev);
            ctrl->pciDevice = NULL;
            ctrl->bar0      = NULL;
        }
        ctrl->num_units = 0;
    }
    devBase->num_controllers = 0;

    if (devBase->IUtility) {
        IExec->DropInterface((struct Interface *)devBase->IUtility);
        NVME_LEAK_DEC(nvme_leak_interface);
    }
    if (devBase->UtilityBase) {
        IExec->CloseLibrary(devBase->UtilityBase);
        NVME_LEAK_DEC(nvme_leak_library);
    }
    if (devBase->IPCI) {
        IExec->DropInterface((struct Interface *)devBase->IPCI);
        NVME_LEAK_DEC(nvme_leak_interface);
    }
    if (devBase->ExpansionBase) {
        IExec->CloseLibrary(devBase->ExpansionBase);
        NVME_LEAK_DEC(nvme_leak_library);
    }

    /* Final audit — debug builds only.  Any non-zero count is a leak. */
    NVMe_DumpLeakStats(IExec);

    IExec->DeleteLibrary((struct Library *)devBase);
    return seglist;
}
