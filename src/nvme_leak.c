/*
 * nvme_leak.c — counter storage + dump implementation.
 *
 * Lives behind #ifdef DEBUG so release builds produce no symbols and
 * link with no overhead.  The header provides macros that vanish
 * entirely in release.
 */

#ifdef DEBUG

#include "nvme_leak.h"
#include "nvme_debug.h"
#include "version.h"

volatile LONG nvme_leak_vec       = 0;
volatile LONG nvme_leak_dma       = 0;
volatile LONG nvme_leak_dmaentry  = 0;
volatile LONG nvme_leak_port      = 0;
volatile LONG nvme_leak_signal    = 0;
volatile LONG nvme_leak_library   = 0;
volatile LONG nvme_leak_interface = 0;
volatile LONG nvme_leak_pcidev    = 0;
volatile LONG nvme_leak_resource  = 0;
volatile LONG nvme_leak_irq       = 0;

/* Report every tracked counter; explicitly flag any non-zero value so
 * the serial log makes the leak obvious.  Called from _manager_Expunge. */
void NVMe_DumpLeakStats(struct ExecIFace *IExec)
{
    struct {
        const char   *name;
        volatile LONG *counter;
    } entries[] = {
        { "AllocVec",         &nvme_leak_vec       },
        { "StartDMA",         &nvme_leak_dma       },
        { "DMAEntry obj",     &nvme_leak_dmaentry  },
        { "MsgPort obj",      &nvme_leak_port      },
        { "AllocSignal",      &nvme_leak_signal    },
        { "OpenLibrary",      &nvme_leak_library   },
        { "GetInterface",     &nvme_leak_interface },
        { "PCI device",       &nvme_leak_pcidev    },
        { "Resource range",   &nvme_leak_resource  },
        { "Int server",       &nvme_leak_irq       },
    };

    IExec->DebugPrintF("[%s] leak report at Expunge:\n", DEVNAME);
    BOOL any_leaked = FALSE;
    for (ULONG i = 0; i < sizeof(entries)/sizeof(entries[0]); i++) {
        LONG v = *entries[i].counter;
        const char *flag = (v == 0) ? "   OK" : " LEAK";
        IExec->DebugPrintF("[%s] %s %-16s = %ld\n",
                           DEVNAME, flag, entries[i].name, v);
        if (v != 0) any_leaked = TRUE;
    }
    if (any_leaked)
        IExec->DebugPrintF("[%s] *** one or more resources leaked — investigate ***\n",
                           DEVNAME);
}

#endif /* DEBUG */
