/*
 * Init.c — driver-manager Init vector (_manager_Init).
 *
 * Invoked once by exec at RTF_AUTOINIT time.  Responsibilities, in order:
 *
 *   1. Emit the always-on startup banner (release + debug builds both).
 *   2. Open expansion.library + IPCI; open utility.library + IUtility.
 *   3. DiscoverNVMe — enumerate all NVMe controllers into
 *      devBase->controllers[] (at most NVME_MAX_CONTROLLERS).
 *   4. For each controller: InitNVMe → InstallNVMeInterrupt →
 *      DiscoverUnits.  Failure of one controller does not abort the
 *      others; the driver publishes whatever it successfully brings up.
 *   5. Populate library node identity.
 *   6. Admin INTx stays masked — see the INTMS rationale at step 5 below.
 *
 * On complete failure (no controllers or no units), unwinds everything
 * and returns NULL — exec treats that as "driver did not load".
 */

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
    struct NVMeBase  *devBase = (struct NVMeBase *)library;
    struct ExecIFace *iexec   = (struct ExecIFace *)exec;

    /* Always-on identification banner. */
    iexec->DebugPrintF("[%s] v%lu.%lu build %s %s\n",
                       DEVNAME, (ULONG)DEVVER, (ULONG)DEVREV,
                       BUILD_DATE, BUILD_TIME);
#ifdef DEBUG
    iexec->DebugPrintF("[%s] DEBUG build — verbose logging enabled\n", DEVNAME);
#endif

    devBase->IExec            = iexec;
    devBase->dev_SegList      = seglist;
    devBase->num_controllers  = 0;
    devBase->num_global_units = 0;

    for (int i = 0; i < NVME_MAX_GLOBAL_UNITS; i++)
        devBase->global_units[i] = NULL;

    BOOL have_expansion = FALSE;
    BOOL have_ipci      = FALSE;
    BOOL have_discovery = FALSE;
    BOOL have_utility   = FALSE;
    BOOL have_iutility  = FALSE;

    /* (1) expansion.library + IPCI. */
    devBase->ExpansionBase = iexec->OpenLibrary("expansion.library", 54);
    if (!devBase->ExpansionBase) {
        DLOG(iexec, "[nvme.device:Init] OpenLibrary(expansion.library, 54) failed\n");
        goto err;
    }
    NVME_LEAK_INC(nvme_leak_library);
    have_expansion = TRUE;

    devBase->IPCI = (struct PCIIFace *)iexec->GetInterface(
        devBase->ExpansionBase, "pci", 1, NULL);
    if (!devBase->IPCI) {
        DLOG(iexec, "[nvme.device:Init] GetInterface(expansion, pci) failed\n");
        goto err;
    }
    NVME_LEAK_INC(nvme_leak_interface);
    have_ipci = TRUE;

    /* (2) Enumerate every NVMe controller on the PCI bus. */
    if (!DiscoverNVMe(devBase))
        goto err;
    have_discovery = TRUE;

    /* (3) utility.library. */
    devBase->UtilityBase = iexec->OpenLibrary("utility.library", 50);
    if (!devBase->UtilityBase) {
        DLOG(iexec, "[nvme.device:Init] OpenLibrary(utility.library, 50) failed\n");
        goto err;
    }
    NVME_LEAK_INC(nvme_leak_library);
    have_utility = TRUE;

    devBase->IUtility = (struct UtilityIFace *)iexec->GetInterface(
        devBase->UtilityBase, "main", 1, NULL);
    if (!devBase->IUtility) {
        DLOG(iexec, "[nvme.device:Init] GetInterface(utility, main) failed\n");
        goto err;
    }
    NVME_LEAK_INC(nvme_leak_interface);
    have_iutility = TRUE;

    /* Populate library node identity before unit discovery so that
     * mounter.library sees a well-formed device node. */
    devBase->dev_Base.dd_Library.lib_Node.ln_Type = NT_DEVICE;
    devBase->dev_Base.dd_Library.lib_Node.ln_Pri  = 0;
    devBase->dev_Base.dd_Library.lib_Node.ln_Name = DEVNAME;
    devBase->dev_Base.dd_Library.lib_Flags        = LIBF_SUMUSED | LIBF_CHANGED;
    devBase->dev_Base.dd_Library.lib_Version      = DEVVER;
    devBase->dev_Base.dd_Library.lib_Revision     = DEVREV;
    devBase->dev_Base.dd_Library.lib_IdString     = DEVVERSIONSTRING;

    /* (4) Per-controller bring-up.  Keep going past any individual
     * failure so a broken controller doesn't hide working ones. */
    ULONG ctrls_up = 0;
    for (ULONG i = 0; i < devBase->num_controllers; i++) {
        struct NVMeController *ctrl = &devBase->controllers[i];

        if (!InitNVMe(ctrl)) {
            DLOG(iexec, "[nvme.device:Init] ctrl %lu: InitNVMe failed — skipping\n", i);
            continue;
        }

        InstallNVMeInterrupt(ctrl);
        DLOG(iexec, "[nvme.device:Init] ctrl %lu: I/O mode = %s\n",
             i, ctrl->polling_mode ? "POLLING (no IRQ)" : "IRQ-driven");

        DiscoverUnits(ctrl);
        ctrls_up++;
    }

    if (ctrls_up == 0 || devBase->num_global_units == 0) {
        DLOG(iexec, "[nvme.device:Init] No usable controllers/units —"
                    " aborting load\n");
        /* Walk back through whatever was set up. */
        for (ULONG i = 0; i < devBase->num_controllers; i++) {
            RemoveNVMeInterrupt(&devBase->controllers[i]);
            CleanupNVMe(&devBase->controllers[i]);
        }
        goto err;
    }

    /* (5) Admin interrupt intentionally stays masked (INTMS remains
     * set from InitNVMe).  Our I/O CQs are created with IEN=0 (polling-
     * style, see NVMe_CreateIOCQ), so the only possible source of NVMe
     * IRQs would be admin-CQ completions — but we poll admin in
     * NVMe_AdminCmd ourselves.  Leaving admin unmasked would cause an
     * IRQ storm on the shared Pegasos2 INTx line: the device asserts
     * level-triggered INTx on admin CQE, our ISR (which only inspects
     * I/O CQs) correctly returns "not ours", no handler in the chain
     * claims, exec retries forever, system hard-freezes.
     *
     * Defensive re-mask also happens at every NVMe_AdminCmd entry. */

    DLOG(iexec, "[nvme.device:Init] Initialised — %lu controller(s),"
                " %lu unit(s) online\n",
         devBase->num_controllers, devBase->num_global_units);
    return (struct Library *)devBase;

err:
    if (have_iutility) {
        iexec->DropInterface((struct Interface *)devBase->IUtility);
        NVME_LEAK_DEC(nvme_leak_interface);
    }
    if (have_utility) {
        iexec->CloseLibrary(devBase->UtilityBase);
        NVME_LEAK_DEC(nvme_leak_library);
    }
    if (have_discovery) {
        /* Discovery allocated PCI resources; release them. */
        for (ULONG i = 0; i < devBase->num_controllers; i++) {
            struct NVMeController *ctrl = &devBase->controllers[i];
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
        }
    }
    if (have_ipci) {
        iexec->DropInterface((struct Interface *)devBase->IPCI);
        NVME_LEAK_DEC(nvme_leak_interface);
    }
    if (have_expansion) {
        iexec->CloseLibrary(devBase->ExpansionBase);
        NVME_LEAK_DEC(nvme_leak_library);
    }
    return NULL;
}
