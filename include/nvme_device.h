#ifndef NVME_DEVICE_H
#define NVME_DEVICE_H

#include <devices/scsidisk.h>
#include <devices/trackdisk.h>
#include <exec/devices.h>
#include <exec/interrupts.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <exec/semaphores.h>
#include <exec/libraries.h>
#include <exec/types.h>

#include <proto/exec.h>

#include <expansion/pci.h>
#include <interfaces/expansion.h>
#include <interfaces/utility.h>
#include <utility/utility.h>

#include "version.h"
#include "nvme.h"
#include "nvme_debug.h"
#include "nvme_leak.h"
#include "nvme_platform.h"
#include "nvme_stats.h"

/* ------------------------------------------------------------------ */
/* Inflight I/O slot — one per in-flight NVMe I/O command              */
/* ------------------------------------------------------------------ */

struct NVMeInflight {
    struct IOStdReq *ioreq;         /* waiting IORequest, NULL = slot free */
    UWORD            cmd_id;        /* NVMe command ID assigned to this slot */
    BOOL             is_write;      /* TRUE = write (for bounce copy-back) */

    /* Direct-DMA path state.  `dma_list` points at the slot's
     * pre-allocated DMAEntry pool (NVMeUnit.dma_entry_pool[slot]) once
     * GetDMAList has populated it — never to a per-I/O allocation.
     * EndDMA in Harvest uses `dma_buf`, `dma_size`, and `dma_flags`. */
    APTR             dma_buf;       /* virtual address of user buffer */
    ULONG            dma_phys;      /* physical address from StartDMA */
    ULONG            dma_size;
    struct DMAEntry *dma_list;      /* alias of dma_entry_pool[slot] */
    ULONG            dma_entries;
    ULONG            dma_flags;     /* DMA direction flags for EndDMA */

    /* Bounce-path state — `use_bounce` gates the copy-back in Harvest.
     * `bounce_user_buf` / `bounce_user_len` snapshot the destination
     * at Submit time, so that Harvest can copy back correctly even if
     * the caller has mutated its IORequest (e.g. the MDTS-chunked path
     * in unit_task.c temporarily overwrites io_Data / io_Length). */
    BOOL             use_bounce;
    APTR             bounce_user_buf;
    ULONG            bounce_user_len;

    /* When TRUE, Harvest must NOT ReplyMsg the IORequest — a synchronous
     * caller inside the unit task (e.g. NVMeIO_SubmitAndWait used by the
     * SCSI SYNCHRONIZE CACHE / UNMAP handlers) is poll-harvesting the
     * slot itself and will take care of the reply.  Harvest still does
     * status translation, stats bookkeeping, and slot release. */
    BOOL             suppress_reply;

    /* For latency tracking — EClock snapshot taken at Submit time,
     * consumed at Harvest.  A 64-bit tick count lives across the two
     * halves of a DoIO. */
    uint32           submit_ticks_hi;
    uint32           submit_ticks_lo;
};

/* Forward decls — NVMeBase/NVMeUnit/NVMeController form a mutually
 * referential cluster; each struct carries a back-pointer to the ones
 * that logically contain it. */
struct NVMeBase;
struct NVMeController;

/* ------------------------------------------------------------------ */
/* NVMeUnit — one per NVMe namespace (AmigaOS device unit)             */
/* ------------------------------------------------------------------ */

struct NVMeUnit {
    struct Unit      unit_Base;
    ULONG            unit_num;      /* 0-based FLAT AmigaOS unit number */
    ULONG            nsid;          /* NVMe namespace ID (1-based) */
    ULONG            open_count;

    /* Geometry (populated from Identify Namespace) */
    ULONG            block_size;    /* bytes per LBA */
    uint64            total_blocks;  /* total LBAs */
    ULONG            block_shift;   /* log2(block_size) */

    /* Per-unit I/O queues (queue ID = local_idx + 1 within controller) */
    APTR             io_sq;         /* virtual address, MEMF_SHARED */
    APTR             io_cq;         /* virtual address, MEMF_SHARED */
    ULONG            io_sq_phys;    /* physical address */
    ULONG            io_cq_phys;
    UWORD            io_sq_tail;    /* next slot to write */
    UWORD            io_cq_head;    /* next slot to read */
    UBYTE            io_cq_phase;   /* expected phase bit */
    UWORD            queue_depth;   /* number of entries (NVME_IO_QUEUE_DEPTH) */
    UWORD            queue_id;      /* NVMe queue ID (1..NVME_MAX_UNITS_PER_CTRL) */

    /* Unit task.  `task` and `task_shutdown` are touched by two tasks
     * at a time (parent + unit task); `volatile` stops the compiler
     * from caching them across the Forbid/Permit or Wait calls used to
     * synchronise. */
    struct Task     *volatile task;
    struct MsgPort  *io_port;
    ULONG            io_port_mask;
    ULONG            io_signal_mask; /* ISR signals this bit to wake task */
    struct Task     *io_wait_task;   /* = task once started; ISR checks this */
    volatile BOOL    task_shutdown;

    /* Shutdown handshake: parent AllocSignals a bit in its own task,
     * hands the mask + task pointer to the unit task, then Wait()s on
     * the mask.  Unit task Signal()s the parent immediately before
     * clearing unit->task.  Replaces the original Forbid/Permit busy-
     * wait, which did not yield CPU to same-priority tasks. */
    struct Task     *shutdown_ack_task;
    ULONG            shutdown_ack_mask;

    /* I/O pipeline (NVME_MAX_INFLIGHT inflight slots) */
    struct NVMeInflight inflight[NVME_MAX_INFLIGHT];

    /* Pre-allocated bounce buffers (one per inflight slot, NVME_BOUNCE_SIZE each) */
    APTR             bounce_bufs[NVME_MAX_INFLIGHT];
    ULONG            bounce_phys[NVME_MAX_INFLIGHT];
    ULONG            bounce_dma_entries[NVME_MAX_INFLIGHT];

    /* Pre-allocated PRP list pages (one per inflight slot, 4KB each, MEMF_SHARED) */
    APTR             prp_list_pages[NVME_MAX_INFLIGHT];
    ULONG            prp_list_phys[NVME_MAX_INFLIGHT];

    /* Pre-allocated DMAEntry pool, one array per inflight slot.  The
     * direct-DMA path in NVMeIO_Submit reuses these instead of issuing
     * AllocSysObjectTags(ASOT_DMAENTRY) / FreeSysObject per I/O — a
     * measurable win on large transfers where the AllocSysObject
     * overhead is a meaningful fraction of end-to-end latency.
     *
     * Capacity is sized in UnitTask_Start to cover the worst-case
     * number of physical fragments a single NVMe command can span,
     * which is max_transfer_bytes / page_size + 1 (the +1 covers
     * unaligned starts).  Any I/O whose StartDMA reports more entries
     * than the pool can hold falls back to a per-I/O AllocSysObject as
     * a safety net. */
    struct DMAEntry *dma_entry_pool[NVME_MAX_INFLIGHT];
    ULONG            dma_entry_pool_capacity; /* entries per slot */

    /* Per-unit DSM (Dataset Management / TRIM) range descriptor buffer.
     * One 4 KiB page, allocated MEMF_SHARED at UnitTask_Start and left
     * DMA-pinned for the unit's lifetime (same pattern as the bounce
     * buffers).  Holds up to NVME_DSM_MAX_RANGES 16-byte descriptors;
     * the SCSI UNMAP handler fills it on demand, issues one DSM command
     * per UNMAP request, and the pin stays live across commands.  Only
     * one DSM can be in flight at a time per unit (the unit task is
     * single-threaded against this buffer). */
    APTR             dsm_buf;
    ULONG            dsm_phys;

    /* Held change-notification requests (must NOT be replied to until removed) */
    struct IOStdReq *changeint_req;
    struct IOStdReq *remove_req;       /* held TD_REMOVE request */

    /* Per-unit live statistics — updated in the hot paths. */
    struct NVMeUnitStats stats;

    /* Back-pointers.  `ctrl` is the fast way to reach admin queues,
     * BAR0, and IRQ state; `dev_base` is kept for codepaths that need
     * the driver library base (mounter.library handle, utility, etc.). */
    struct NVMeController *ctrl;
    struct NVMeBase       *dev_base;
};

/* ------------------------------------------------------------------ */
/* NVMeController — per-physical-controller state                      */
/*                                                                      */
/* One PCI NVMe controller.  Holds its own BAR0, admin queues, IRQ     */
/* vector, and up to NVME_MAX_UNITS_PER_CTRL namespace units.  A       */
/* typical system has exactly one of these; the array on NVMeBase      */
/* supports configurations with multiple controllers (RAID, ZFS L2ARC, */
/* etc.) without re-plumbing the I/O paths.                            */
/* ------------------------------------------------------------------ */

struct NVMeController {
    /* PCI identity */
    struct PCIDevice        *pciDevice;
    struct PCIResourceRange *bar0;          /* NVMe register region */
    ULONG                    iobase;        /* CPU-mapped MMIO base */

    /* Admin-command serialisation. */
    struct SignalSemaphore   io_lock;

    /* CAP-derived parameters. */
    ULONG                    cap_mqes;      /* max queue entries */
    ULONG                    cap_dstrd;     /* doorbell stride exponent */
    ULONG                    page_size;     /* host page size */
    ULONG                    max_transfer_bytes;  /* MDTS-derived cap */
    UWORD                    io_queue_depth; /* min(NVME_IO_QUEUE_DEPTH, MQES+1) */

    /* Admin SQ/CQ pair (one per controller). */
    APTR                     admin_sq;
    APTR                     admin_cq;
    ULONG                    admin_sq_phys;
    ULONG                    admin_cq_phys;
    struct DMAEntry         *admin_sq_dma;
    struct DMAEntry         *admin_cq_dma;
    UWORD                    admin_sq_tail;
    UWORD                    admin_cq_head;
    UBYTE                    admin_cq_phase;
    UWORD                    next_cmd_id;

    /* Shared 4 KiB Identify scratch buffer. */
    APTR                     identify_buf;
    ULONG                    identify_phys;
    struct DMAEntry         *identify_dma;

    /* Units belonging to this controller, local indexing. */
    struct NVMeUnit         *units[NVME_MAX_UNITS_PER_CTRL];
    ULONG                    num_units;

    /* Interrupt state. */
    struct Interrupt         irq_handler;
    BOOL                     irq_installed;
    ULONG                    irq_vector;
    BOOL                     polling_mode;

    /* Identify-derived static info (captured in NVMe_IdentifyController).
     * Kept as fixed-length strings so the stats snapshot can copy them
     * out without re-running Identify. */
    char                     model[41];     /* MN, NUL-padded  */
    char                     serial[21];    /* SN, NUL-padded  */
    char                     fw_rev[9];     /* FR, NUL-padded  */

    /* Optional NVM Command Support (ONCS) and Volatile Write Cache (VWC)
     * bits from Identify Controller (bytes 520-521 and 525).  Used by
     * the SCSI translation layer to reject UNMAP (TRIM) on controllers
     * that don't support Dataset Management, and to report/toggle the
     * write-cache state through SCSI Mode Page 0x08.  `vwc_enabled`
     * tracks the most recent Set Features value we issued; it starts
     * TRUE because NVMe defaults a present VWC to enabled on reset. */
    BOOL                     onc_dsm;          /* ONCS bit 2 — DSM supported */
    BOOL                     vwc_present;      /* VWC byte 525 bit 0 */
    BOOL                     vwc_enabled;      /* last-known VWC state */

#ifdef ENABLE_SMART
    /* SMART / Health Information Log cache.  Populated lazily on the
     * first NSCMD_NVME_GETSTATS after NVME_SMART_REFRESH_SECS elapse. */
    struct NVMeSMARTCache    smart_cache;
#endif

    /* Identity / location. */
    ULONG                    ctrl_idx;      /* index within NVMeBase.controllers[] */
    struct NVMeBase         *dev_base;      /* back-pointer */
};

/* ------------------------------------------------------------------ */
/* NVMeBase — the device library base                                  */
/* ------------------------------------------------------------------ */

struct NVMeBase {
    struct Device            dev_Base;
    struct ExecIFace        *IExec;
    struct Library          *ExpansionBase;
    struct PCIIFace         *IPCI;
    struct Library          *UtilityBase;
    struct UtilityIFace     *IUtility;
    BPTR                     dev_SegList;

    /* Per-physical-controller state. */
    struct NVMeController    controllers[NVME_MAX_CONTROLLERS];
    ULONG                    num_controllers;

    /* Flat unit table — unit_num is the index.  Allows Open/BeginIO
     * to resolve a unit without knowing which controller owns it.
     * Each pointer also appears in exactly one ctrl->units[] slot. */
    struct NVMeUnit         *global_units[NVME_MAX_GLOBAL_UNITS];
    ULONG                    num_global_units;

    /* Host platform identification (shared by all controllers). */
    NVMePlatform             platform;
};

/* ------------------------------------------------------------------ */
/* Function prototypes for device manager interface                    */
/* ------------------------------------------------------------------ */

struct Library      *_manager_Init(struct Library *library, BPTR seglist, struct Interface *exec);
struct NVMeBase     *_manager_Open(struct DeviceManagerInterface *Self, struct IOStdReq *ioreq,
                                   ULONG unitNum, ULONG flags);
BPTR                 _manager_Expunge(struct DeviceManagerInterface *Self);
BPTR                 _manager_Close(struct DeviceManagerInterface *Self, struct IOStdReq *ioreq);
void                 _manager_BeginIO(struct DeviceManagerInterface *Self, struct IOStdReq *ioreq);
LONG                 _manager_AbortIO(struct DeviceManagerInterface *Self, struct IOStdReq *ioreq);
ULONG                _manager_Obtain(struct DeviceManagerInterface *Self);
ULONG                _manager_Release(struct DeviceManagerInterface *Self);

/* ------------------------------------------------------------------ */
/* NVMe register access — cache-inhibited memory-mapped BAR0           */
/*                                                                      */
/* NVMe registers live in a memory BAR that any working PCIe bridge    */
/* maps into the CPU address space.  The bridge does NOT byte-swap the */
/* register data — NVMe registers are defined by the spec as little-   */
/* endian, so on PowerPC (big-endian) we swap in software via the      */
/* byte-reversed load/store instructions:                              */
/*     lwbrx rT, 0, rA   — load word byte-reversed (LE load)           */
/*     stwbrx rS, 0, rA  — store word byte-reversed (LE store)         */
/*                                                                      */
/* Ordering: every MMIO store is followed by a heavy `sync` (aka mbar) */
/* rather than `eieio`.  Under QEMU TCG, `eieio` is known to be too    */
/* weak to force the store out of the CPU pipeline before the next     */
/* instruction reads it back; `sync` is architecturally strict enough  */
/* to be safe on both QEMU and real hardware.                          */
/*                                                                      */
/* DMA buffers (SQE/CQE and PRP data in RAM) are NOT routed through    */
/* the bridge's endian swap — they travel as raw bytes.  The CPU must  */
/* therefore write SQEs in LE form (see dma_w32/dma_r32 helpers in     */
/* nvme_admin.c / nvme_io.c).                                          */
/* ------------------------------------------------------------------ */

/* Read a 32-bit little-endian register via lwbrx. */
static inline ULONG nvme_r32(ULONG addr)
{
    ULONG val;
    __asm__ volatile ("lwbrx %0, 0, %1" : "=r"(val) : "r"(addr));
    return val;
}

/* Write a 32-bit little-endian register via stwbrx + sync.
 * `sync` is a full memory barrier (PowerPC architected; same encoding as
 * `mbar`).  We intentionally pay that cost on every MMIO write — NVMe
 * doorbell writes are rare on the hot path, and correctness under QEMU
 * TCG outweighs the extra cycles. */
static inline void nvme_w32(ULONG addr, ULONG val)
{
    __asm__ volatile ("stwbrx %0, 0, %1; sync"
                      : : "r"(val), "r"(addr) : "memory");
}

#define NVME_R32(base, dev, off)     nvme_r32((base) + (off))
#define NVME_W32(base, dev, off, v)  nvme_w32((base) + (off), (v))
#define NVME_W32_DB(pciDev, base, off, v) \
    nvme_w32((base) + (off), (v))

#endif /* NVME_DEVICE_H */
