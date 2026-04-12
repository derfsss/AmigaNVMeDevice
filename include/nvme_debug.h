#ifndef NVME_DEBUG_H
#define NVME_DEBUG_H

/*
 * nvme_debug.h — Debug / logging macros for nvme.device
 *
 * Single logging primitive:
 *
 *   DPRINTF(iexec, fmt, ...)
 *       Verbose trace output.  Compiled to a no-op in release builds.
 *       Use freely in hot paths and boot-time reporting — all costs
 *       vanish when DEBUG is not defined.
 *
 * For backwards compatibility with existing call sites, DLOG is a
 * deprecated alias — both macros compile out identically in release.
 * Release builds should emit nothing except the startup banner
 * (a direct IExec->DebugPrintF in Init.c).  If a new always-on line
 * is needed, add another direct call there rather than bringing back
 * a release-mode logging macro.
 *
 * Routes through IExec->DebugPrintF, the AmigaOS 4 kernel-side output
 * sink (visible via QEMU's -serial stdio or the SERxxx serial console
 * on real hardware).
 *
 * AmigaOS 4 PPC DebugPrintF does not honour %llu / %lld — always
 * split 64-bit values into hi/lo %lu pairs, or cast to (ULONG) when the
 * value is known to fit in 32 bits.
 */

#include <proto/exec.h>

#ifdef DEBUG
#define DPRINTF(iexec, ...) ((iexec)->DebugPrintF(__VA_ARGS__))
#else
/* Release: evaluate-to-nothing, but preserve type-checking so release builds
 * still catch format-string mistakes in the call sites. */
#define DPRINTF(iexec, ...)                                                 \
    do {                                                                    \
        if (0)                                                              \
            ((struct ExecIFace *)(iexec))->DebugPrintF(__VA_ARGS__);        \
    } while (0)
#endif

/* Deprecated alias — kept so existing source doesn't need a mass
 * rename.  Release builds emit nothing except the banner in Init.c. */
#define DLOG(iexec, ...) DPRINTF(iexec, __VA_ARGS__)

#endif /* NVME_DEBUG_H */
