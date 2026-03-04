#include "nvme_irq.h"
#include "nvme_device.h"
#include <exec/exec.h>
#include <expansion/pci.h>

/*
 * NVMe interrupt handler.
 * Runs in interrupt context — no allocations, no DebugPrintF.
 * On completion: signals each unit's io_wait_task.
 *
 * NVMe spec: device asserts INTx when it writes a new CQE with the correct
 * phase bit. The interrupt is cleared by reading/acknowledging via PCI.
 * For polled mode (irq not installed), the unit task polls io_cq_phase directly.
 */
static ULONG nvme_irq_handler(struct ExceptionContext *ctx, struct ExecBase *exec, APTR data)
{
    struct NVMeBase *devBase = (struct NVMeBase *)data;
    (void)ctx; (void)exec;

    /* Signal all units with active wait tasks */
    for (ULONG i = 0; i < devBase->num_units; i++) {
        struct NVMeUnit *unit = devBase->units[i];
        if (unit && unit->io_wait_task) {
            ((struct ExecIFace *)((struct ExecBase *)*((ULONG *)4)))->Signal(
                unit->io_wait_task, unit->io_signal_mask);
        }
    }

    return 0; /* pass through to next handler */
}

BOOL InstallNVMeInterrupt(struct NVMeBase *devBase)
{
    /* TODO: Use expansion.library MapInterrupt() / AddIntServer() pattern
     * (same as virtioscsi.device virtio_irq.c) once the exact AmigaOS 4.1
     * PCI interrupt API is confirmed for NVMe INTx routing.
     * For now: return FALSE to use polling fallback. */
    devBase->irq_installed = FALSE;
    return FALSE;
}

void RemoveNVMeInterrupt(struct NVMeBase *devBase)
{
    if (!devBase->irq_installed) return;
    /* TODO: RemoveIntServer / UnmapInterrupt */
    devBase->irq_installed = FALSE;
}
