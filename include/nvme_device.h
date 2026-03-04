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

/* ------------------------------------------------------------------ */
/* Inflight I/O slot — one per in-flight NVMe I/O command              */
/* ------------------------------------------------------------------ */

struct NVMeInflight {
    struct IOStdReq *ioreq;         /* waiting IORequest, NULL = slot free */
    UWORD            cmd_id;        /* NVMe command ID assigned to this slot */
    BOOL             is_write;      /* TRUE = write (for bounce copy-back) */

    /* Direct DMA path (transfer > NVME_BOUNCE_SIZE) */
    APTR             dma_buf;       /* virtual address of user buffer */
    ULONG            dma_phys;      /* physical address from StartDMA */
    ULONG            dma_size;
    struct DMAEntry *dma_list;
    ULONG            dma_entries;

    /* Bounce buffer path (transfer <= NVME_BOUNCE_SIZE) */
    BOOL             use_bounce;
};

/* ------------------------------------------------------------------ */
/* NVMeUnit — one per NVMe namespace (AmigaOS device unit)             */
/* ------------------------------------------------------------------ */

struct NVMeUnit {
    struct Unit      unit_Base;
    ULONG            unit_num;      /* 0-based AmigaOS unit number */
    ULONG            nsid;          /* NVMe namespace ID (1-based) */
    ULONG            open_count;

    /* Geometry (populated from Identify Namespace) */
    ULONG            block_size;    /* bytes per LBA */
    UQUAD            total_blocks;  /* total LBAs */
    ULONG            block_shift;   /* log2(block_size) */

    /* Per-unit I/O queues (queue ID = unit_num + 1) */
    APTR             io_sq;         /* virtual address, MEMF_SHARED */
    APTR             io_cq;         /* virtual address, MEMF_SHARED */
    ULONG            io_sq_phys;    /* physical address */
    ULONG            io_cq_phys;
    UWORD            io_sq_tail;    /* next slot to write */
    UWORD            io_cq_head;    /* next slot to read */
    UBYTE            io_cq_phase;   /* expected phase bit */
    UWORD            queue_depth;   /* number of entries (NVME_IO_QUEUE_DEPTH) */
    UWORD            queue_id;      /* NVMe queue ID (= unit_num + 1) */

    /* Unit task */
    struct Task     *task;
    struct MsgPort  *io_port;
    ULONG            io_port_mask;
    ULONG            io_signal_mask; /* ISR signals this bit to wake task */
    struct Task     *io_wait_task;   /* = task once started; ISR checks this */
    BOOL             task_shutdown;

    /* I/O pipeline (NVME_MAX_INFLIGHT inflight slots) */
    struct NVMeInflight inflight[NVME_MAX_INFLIGHT];

    /* Pre-allocated bounce buffers (one per inflight slot, NVME_BOUNCE_SIZE each) */
    APTR             bounce_bufs[NVME_MAX_INFLIGHT];
    ULONG            bounce_phys[NVME_MAX_INFLIGHT];
    ULONG            bounce_dma_entries[NVME_MAX_INFLIGHT];

    /* Pre-allocated PRP list pages (one per inflight slot, 4KB each, MEMF_SHARED) */
    APTR             prp_list_pages[NVME_MAX_INFLIGHT];
    ULONG            prp_list_phys[NVME_MAX_INFLIGHT];

    /* Held change-notification requests (must NOT be replied to until removed) */
    struct IOStdReq *changeint_req;

    /* Back-pointer to device base */
    struct NVMeBase *dev_base;
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

    /* Admin queue serialisation — held for all admin commands */
    struct SignalSemaphore   io_lock;

    /* PCI device and BAR0 */
    struct PCIDevice        *pciDevice;
    struct PCIResourceRange *bar0;      /* BAR0: NVMe register region */
    ULONG                    iobase;    /* bar0->Physical — base for pciDev->InX() */

    /* Controller capabilities (read from CAP register at init) */
    ULONG                    cap_mqes;  /* max queue entries per queue (from CAP[15:0]) */
    ULONG                    cap_dstrd; /* doorbell stride (0 = 4 bytes) */
    ULONG                    page_size; /* host page size in bytes (4096 minimum) */

    /* Admin queues (shared, one pair for the whole controller) */
    APTR                     admin_sq;      /* virtual, MEMF_SHARED, 64*64 bytes */
    APTR                     admin_cq;      /* virtual, MEMF_SHARED, 64*16 bytes */
    ULONG                    admin_sq_phys;
    ULONG                    admin_cq_phys;
    struct DMAEntry         *admin_sq_dma;
    struct DMAEntry         *admin_cq_dma;
    UWORD                    admin_sq_tail;
    UWORD                    admin_cq_head;
    UBYTE                    admin_cq_phase;  /* expected phase bit */
    UWORD                    next_cmd_id;     /* rolling 16-bit command ID counter */

    /* 4KB identify scratch buffer (DMA-mapped, reused for all Identify calls at init) */
    APTR                     identify_buf;
    ULONG                    identify_phys;
    struct DMAEntry         *identify_dma;

    /* Units (one per NVMe namespace, up to 8) */
    struct NVMeUnit         *units[8];
    ULONG                    num_units;

    /* Interrupt handler */
    struct Interrupt         irq_handler;
    BOOL                     irq_installed;
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
/* Debugging macro                                                      */
/* ------------------------------------------------------------------ */

#ifdef DEBUG
#define DPRINTF(iexec, ...) ((iexec)->DebugPrintF(__VA_ARGS__))
#else
#define DPRINTF(iexec, ...)                         \
    do {                                            \
        if (0)                                      \
            ((struct ExecIFace *)(iexec))->DebugPrintF(__VA_ARGS__); \
    } while (0)
#endif

/* ------------------------------------------------------------------ */
/* NVMe register access via pciDev->InX() / OutX()                     */
/*                                                                      */
/* These go through the expansion.library PCIDevice bridge layer and   */
/* work correctly on AmigaOne's Articia-S chipset. The methods handle  */
/* endian conversion automatically (PCI_MODE_REVERSE_ENDIAN).          */
/* ------------------------------------------------------------------ */

#define NVME_R32(base, dev, off)     ((dev)->InLong((base) + (off)))
#define NVME_W32(base, dev, off, v)  ((dev)->OutLong((base) + (off), (v)))
#define NVME_R16(base, dev, off)     ((dev)->InWord((base) + (off)))
#define NVME_W16(base, dev, off, v)  ((dev)->OutWord((base) + (off), (v)))

#endif /* NVME_DEVICE_H */
