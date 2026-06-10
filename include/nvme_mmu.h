#ifndef NVME_MMU_H
#define NVME_MMU_H

/*
 * nvme_mmu.h — MMU attribute helpers for NVMe BAR regions.
 *
 * PowerPC CPUs speculatively execute and reorder memory accesses.  For an
 * MMIO region (PCI BAR), this can produce:
 *
 *   - Register reads returning stale cache lines
 *   - Register writes coalesced or deferred past the next write
 *   - Re-ordered reads of side-effectful registers (e.g. CSTS after CC)
 *
 * The AmigaOS 4 MMU interface lets us mark a memory region as
 *   CACHE-INHIBITED (no cache lines allocated)
 *   GUARDED           (no speculative execution of loads from it)
 *
 * which, together, make MMIO accesses behave the way a register-level
 * programmer expects on any PPC platform (Pegasos2 MV64361, SAM460ex,
 * X1000, X5000, A1222).
 *
 * This helper is idempotent and safe to call multiple times on the same
 * region; it reads the existing attrs and ORs in CI+G rather than
 * overwriting, so platform-specific bits (e.g. 0x82 from the diag init on
 * some machines) are preserved.
 *
 * A failure to obtain the MMU interface is logged but not fatal —
 * the driver will attempt to run without CI+G and may misbehave on real
 * hardware; under QEMU it will still work.
 */

#include <exec/types.h>
#include <proto/exec.h>

struct PCIResourceRange;

/* Apply MEMATTRF_CACHEINHIBIT | MEMATTRF_GUARDED to a PCI memory BAR.
 * No-op for I/O-port BARs.  Safe to call with a NULL range. */
void NVMe_MMU_SetupBAR(struct ExecIFace *IExec, struct PCIResourceRange *bar);

#endif /* NVME_MMU_H */
