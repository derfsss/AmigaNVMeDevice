/*
 * nvme_mmu.c — MMU attribute helpers for NVMe BAR regions.
 *
 * See include/nvme_mmu.h for the rationale.  This is a direct adaptation
 * of the VirtIOGPU `chip_immu_setup_bar` pattern, which itself was taken
 * from RadeonRX.chip (0x101ab54) and pa6t_eth.device.
 */

#include "nvme_mmu.h"
#include "nvme_debug.h"

#include <exec/exec.h>
#include <exec/memory.h>
#include <expansion/pci.h>
#include <proto/exec.h>

void NVMe_MMU_SetupBAR(struct ExecIFace *IExec, struct PCIResourceRange *bar)
{
    if (!bar || !(bar->Flags & PCI_RANGE_MEMORY))
        return;

    /* Fetch the MMU interface off ExecBase.  Interface is ref-counted;
     * drop it before returning to avoid leaking a reference. */
    struct ExecBase *SysBase = *(struct ExecBase **)4;
    struct MMUIFace *IMMU = (struct MMUIFace *)
        IExec->GetInterface((struct Library *)SysBase, "mmu", 1, NULL);
    if (!IMMU) {
        DLOG(IExec, "[nvme.device:mmu] MMU interface unavailable — BAR may be"
                    " unreliable on real hardware\n");
        return;
    }

    /* The MMU table is shared global state; serialise the read-modify-write
     * under Forbid/Permit so we don't race another task's update. */
    IExec->Forbid();

    ULONG existing = IMMU->GetMemoryAttrs((APTR)bar->BaseAddress, 0);
    ULONG newattrs = existing | MEMATTRF_CACHEINHIBIT | MEMATTRF_GUARDED;
    IMMU->SetMemoryAttrs((APTR)bar->BaseAddress, bar->Size, newattrs);

    IExec->Permit();

    DPRINTF(IExec, "[nvme.device:mmu] BAR @ 0x%08lx size=0x%lx attrs"
                   " 0x%lx -> 0x%lx (CI+G)\n",
            bar->BaseAddress, bar->Size, existing, newattrs);

    IExec->DropInterface((struct Interface *)IMMU);
}
