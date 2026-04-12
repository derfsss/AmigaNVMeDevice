#ifndef NVME_LEAK_H
#define NVME_LEAK_H

/*
 * nvme_leak.h — debug-only allocation/release counters.
 *
 * Each tracked allocator has an `nvme_leak_<type>` counter that
 * increments on a successful acquisition and decrements on the
 * matching release.  At Expunge time the driver calls
 * NVMe_DumpLeakStats which emits any non-zero values to the serial
 * console.  A clean shutdown leaves every counter at zero.
 *
 * In release builds every counter macro and the dump function
 * compile to nothing — there is no runtime cost when !DEBUG.
 *
 * The counters are `volatile LONG` rather than atomics: they're only
 * touched from the driver task and (in release) never read back, so
 * lack of SMP-safe atomics is not a correctness hazard here.
 */

#include <exec/types.h>
#include <proto/exec.h>

#ifdef DEBUG

/* Per-resource counters.  Volatile so any debug-build race window
 * between read and write is preserved at source level. */
extern volatile LONG nvme_leak_vec;        /* AllocVecTags / FreeVec            */
extern volatile LONG nvme_leak_dma;        /* StartDMA / EndDMA                  */
extern volatile LONG nvme_leak_dmaentry;   /* AllocSysObject ASOT_DMAENTRY      */
extern volatile LONG nvme_leak_port;       /* AllocSysObject ASOT_PORT          */
extern volatile LONG nvme_leak_signal;     /* AllocSignal / FreeSignal          */
extern volatile LONG nvme_leak_library;    /* OpenLibrary / CloseLibrary        */
extern volatile LONG nvme_leak_interface;  /* GetInterface / DropInterface      */
extern volatile LONG nvme_leak_pcidev;     /* FindDevice* / FreeDevice          */
extern volatile LONG nvme_leak_resource;   /* GetResourceRange / FreeResourceRange */
extern volatile LONG nvme_leak_irq;        /* AddIntServer / RemIntServer       */

#define NVME_LEAK_INC(c) do { (c)++; } while (0)
#define NVME_LEAK_DEC(c) do { (c)--; } while (0)

void NVMe_DumpLeakStats(struct ExecIFace *IExec);

#else  /* !DEBUG */

#define NVME_LEAK_INC(c) do { (void)0; } while (0)
#define NVME_LEAK_DEC(c) do { (void)0; } while (0)
#define NVMe_DumpLeakStats(iexec) do { (void)(iexec); } while (0)

#endif /* DEBUG */

#endif /* NVME_LEAK_H */
