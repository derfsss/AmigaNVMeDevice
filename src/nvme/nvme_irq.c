/*
 * nvme_irq.c — per-controller PCI INTx ISR + install/remove plumbing.
 *
 * Each NVMeController installs its own IRQ server — `is_Data` points
 * at the owning struct NVMeController, and the ISR only inspects its
 * own controller's unit queues.  On systems with multiple NVMe
 * controllers sharing an INTx line, each ISR claims independently.
 *
 * Runs in interrupt context: no allocations, no DebugPrintF, no locks.
 * Claim criterion: any unit of THIS controller has a CQE whose phase
 * bit matches the expected value.  Admin CQ isn't checked because
 * admin commands are polled.
 *
 * On claim: mask INTMS (this controller only) so the IRQ line can
 * de-assert while the task drains; signal each unit task; return 1.
 * Harvest unmasks INTMC after draining.
 */

#include "nvme_irq.h"
#include "nvme_device.h"
#include <exec/exec.h>
#include <exec/interrupts.h>
#include <expansion/pci.h>

static inline ULONG isr_dma_r32(const void *addr)
{
    ULONG val;
    __asm__ volatile ("lwbrx %0, 0, %1" : "=r"(val) : "r"(addr));
    return val;
}

/* ISR-visible statistics.  Volatile so debug readers see fresh values.
 * These are global rather than per-controller for simplicity; a
 * multi-controller setup will see aggregate counts. */
volatile ULONG nvme_isr_count    = 0;
volatile ULONG nvme_isr_claimed  = 0;
volatile ULONG nvme_isr_not_ours = 0;

static ULONG nvme_irq_handler(struct ExceptionContext *ctx,
                              struct ExecBase *exec, APTR data)
{
    struct NVMeController *ctrl = (struct NVMeController *)data;
    (void)ctx; (void)exec;

    nvme_isr_count++;

    /* Peek each of this controller's unit I/O CQ heads. */
    BOOL ours = FALSE;
    for (ULONG i = 0; i < ctrl->num_units; i++) {
        struct NVMeUnit *unit = ctrl->units[i];
        if (unit && unit->io_cq) {
            UBYTE *cq_entry = (UBYTE *)unit->io_cq +
                              unit->io_cq_head * NVME_CQE_SIZE;
            ULONG dw3 = isr_dma_r32(cq_entry + 12);
            UWORD status = (UWORD)(dw3 >> 16);
            if (NVME_CQE_PHASE(status) == unit->io_cq_phase) {
                ours = TRUE;
                break;
            }
        }
    }

    if (!ours) {
        nvme_isr_not_ours++;
        return 0;
    }

    nvme_isr_claimed++;

    /* Mask THIS controller's INTx vector.  Other NVMe controllers on
     * the shared line continue to assert independently. */
    nvme_w32(ctrl->iobase + NVME_REG_INTMS, 1);

    /* Signal every unit task on this controller.  Each one's Harvest
     * will consume any CQEs on its own CQ and eventually unmask INTMC. */
    struct ExecIFace *IExec =
        (struct ExecIFace *)((struct ExecBase *)*((ULONG *)4))->MainInterface;
    for (ULONG i = 0; i < ctrl->num_units; i++) {
        struct NVMeUnit *unit = ctrl->units[i];
        if (unit && unit->io_wait_task)
            IExec->Signal(unit->io_wait_task, unit->io_signal_mask);
    }

    return 1;
}

BOOL InstallNVMeInterrupt(struct NVMeController *ctrl)
{
    struct ExecIFace *IExec = ctrl->dev_base->IExec;

    /* Default to polling; only clear it on full success. */
    ctrl->polling_mode  = TRUE;
    ctrl->irq_installed = FALSE;

    if (!ctrl->pciDevice) return FALSE;

    ULONG irq = ctrl->pciDevice->MapInterrupt();
    if (irq == 0) {
        DLOG(IExec, "[nvme.device:irq] ctrl %lu: MapInterrupt returned 0 —"
                    " polling fallback\n", ctrl->ctrl_idx);
        return FALSE;
    }

    ctrl->irq_handler.is_Node.ln_Type = NT_INTERRUPT;
    ctrl->irq_handler.is_Node.ln_Pri  = 0;
    ctrl->irq_handler.is_Node.ln_Name = "nvme.device irq";
    ctrl->irq_handler.is_Code         = (VOID (*)())nvme_irq_handler;
    ctrl->irq_handler.is_Data         = ctrl;

    if (!IExec->AddIntServer(irq, &ctrl->irq_handler)) {
        DLOG(IExec, "[nvme.device:irq] ctrl %lu: AddIntServer(%lu) failed —"
                    " polling fallback\n", ctrl->ctrl_idx, irq);
        return FALSE;
    }
    NVME_LEAK_INC(nvme_leak_irq);

    ctrl->irq_vector    = irq;
    ctrl->irq_installed = TRUE;
    ctrl->polling_mode  = FALSE;

    DLOG(IExec, "[nvme.device:irq] ctrl %lu: INTx installed on vector %lu\n",
         ctrl->ctrl_idx, irq);
    return TRUE;
}

void RemoveNVMeInterrupt(struct NVMeController *ctrl)
{
    struct ExecIFace *IExec = ctrl->dev_base->IExec;

    if (!ctrl->irq_installed) return;

    /* Mask before removing so a late CQE cannot call into the
     * soon-to-be-freed Interrupt struct. */
    nvme_w32(ctrl->iobase + NVME_REG_INTMS, 1);

    IExec->RemIntServer(ctrl->irq_vector, &ctrl->irq_handler);
    NVME_LEAK_DEC(nvme_leak_irq);
    ctrl->irq_installed = FALSE;
    ctrl->polling_mode  = TRUE;

    DLOG(IExec, "[nvme.device:irq] ctrl %lu: INTx removed from vector %lu\n",
         ctrl->ctrl_idx, ctrl->irq_vector);
}
