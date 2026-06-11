# nvme.device — Architecture and Implementation Plan

## Overview

`nvme.device` is a native AmigaOS 4.1 Final Edition block-device driver for
NVMe controllers on PCIe.  **A single binary runs on every AmigaOS 4.1 FE
platform with a working PCIe bridge** — runtime-detected via a host-bridge
table and a BAR0 MMIO forwarding probe.

It is a single-phase `RTF_COLDSTART` driver (priority 0, matching the
system disk drivers for boot-drive compatibility).  Multi-controller, up to
4 × 8-namespace each (32 flat units).  All NVMe I/O is available immediately
at boot.

Canonical changelog: `docs/history.md`.

---

## Hardware Target

Host bridges recognised by the platform-detection table (diagnostic —
the BAR0 MMIO probe is the authoritative gate):

| Platform        | Host bridge           |
|-----------------|-----------------------|
| QEMU Pegasos2   | Marvell MV64361       |
| Pegasos II      | MV64361               |
| Sam440ep        | AMCC 440EP            |
| Sam460ex        | AMCC 460EX            |
| X1000           | P.A. Semi PA6T "Nemo" |
| X5000           | NXP QorIQ P5020/P5040 |
| A1222 "Tabor"   | NXP QorIQ P1022       |
| AmigaOne 500    | -                     |

**Practical real-hardware targets are the X1000 and X5000 only** — an
NVMe drive needs a free PCIe slot (via an M.2 adapter card), and the
other machines have none available (PCI/AGP only, or PCIe lanes
already occupied).  On those, the class-code scan simply finds no
controller and the driver declines to load.  Under QEMU all tested
machines (pegasos2, sam460ex, amigaone) work, since the emulator
attaches the NVMe controller to the emulated PCI bus.

**Not supported**: AmigaOne XE/SE (Mai Logic Articia S) — the bridge does
not forward CPU memory cycles to PCIe.  The driver detects this via the
BAR0 MMIO probe at init time and aborts cleanly with a diagnostic.

- **Register access**: `lwbrx`/`stwbrx` PPC inline asm + `sync` barrier
- **DMA**: `AllocVecTags(MEMF_SHARED)` + `StartDMA`/`GetDMAList`/
  `EndDMA(DMAF_NoModify)` — buffer stays cacheable, physical address cached
- **Cache coherency**: `CacheClearE(CACRF_ClearD)` before write-path DMA,
  `CacheClearE(CACRF_InvalidateD)` before read-path CPU access
- **MMU**: `MEMATTRF_CACHEINHIBIT | MEMATTRF_GUARDED` on each controller's
  BAR0 region (`NVMe_MMU_SetupBAR` in `src/nvme_mmu.c`)
- **Interrupts**: per-controller INTx via `pciDev->MapInterrupt()` +
  `IExec->AddIntServer()`; automatic polling fallback if either fails

---

## MMIO Access Pattern

NVMe registers are little-endian; PPC is big-endian.  `lwbrx`/`stwbrx`
instructions do the byte-swap automatically.  The post-write barrier is
`sync` (full memory barrier, architectured PPC) rather than `eieio` —
under QEMU TCG `eieio` is too weak to force MMIO stores out of the CPU
pipeline in time.

```c
static inline ULONG nvme_r32(ULONG addr) {
    ULONG val;
    __asm__ volatile ("lwbrx %0, 0, %1" : "=r"(val) : "r"(addr));
    return val;
}
static inline void nvme_w32(ULONG addr, ULONG val) {
    __asm__ volatile ("stwbrx %0, 0, %1; sync" : : "r"(val), "r"(addr) : "memory");
}
```

---

## Driver Architecture

### Resident flags
```c
RTF_NATIVE | RTF_COLDSTART | RTF_AUTOINIT
```
Priority: `0` (matches the system disk drivers, e.g. disk.device;
allows `diskboot.kmod` to consider nvme.device as a boot-drive candidate).

### Interface vectors
```
Obtain, Release, NULL, NULL, Open, Close, Expunge, NULL, BeginIO, AbortIO, (APTR)-1
```

### Init sequence (`_manager_Init`)
1. Always-on startup banner: `[nvme.device] vX.Y build DD.MM.YYYY HH:MM:SS`
2. Open `expansion.library` v54, get `IPCI`
3. `DiscoverNVMe` — platform identification, PCI loop via `FDT_Index`
   enumerating every NVMe controller (up to `NVME_MAX_CONTROLLERS=4`),
   BAR0 map, MMU attrs, MMIO probe.  Populates `devBase->controllers[]`.
4. Open `utility.library` v50, get `IUtility`
5. For each populated controller:
   - `InitNVMe(ctrl)` — reset, enable, identify (captures model/serial/FW)
   - `InstallNVMeInterrupt(ctrl)` — per-controller ISR install (non-fatal)
   - `DiscoverUnits(ctrl)` — namespace enumeration → `ctrl->units[]` +
     `devBase->global_units[]`; announce to mounter.library
6. Unmask `INTMC` on each controller that has a live IRQ vector
7. Success log: `[nvme.device:Init] Initialised — N controller(s), M unit(s) online`

---

## NVMe Protocol

### Controller init sequence
1. Read `CAP` → extract MQES (max queue entries), DSTRD (doorbell stride), page size
2. Disable: `CC.EN = 0`, poll `CSTS.RDY = 0`
3. Allocate admin SQ (64 × 64 bytes) + CQ (64 × 16 bytes), `MEMF_SHARED`, DMA-mapped
4. Write `ASQ`/`ACQ` physical addresses (lo/hi 32-bit pairs), `AQA` sizes
5. Set `CC`: page size, IOSQES=6, IOCQES=4, EN=1; poll `CSTS.RDY = 1`
6. Admin: **Identify Controller** (CNS=1) → model, firmware, MDTS
7. Admin: **Identify Active NS List** (CNS=2) → array of NSIDs
8. For each NSID: Admin **Identify Namespace** (CNS=0) → NSZE, LBAF → block_size/shift
9. For each NSID: Admin **Create IO CQ** + **Create IO SQ** (queue ID = unit_num + 1)

### Admin command dispatch (polling)
- Build 64-byte SQE in pre-allocated DMA buffer
- Write SQ tail doorbell: `NVME_W32(base, dev, NVME_SQ_TAIL_DB(0, dstrd), sq_tail)`
- Poll CQ phase bit for completion — IOERR_ABORTED on timeout
- Ring CQ head doorbell after consuming entry

### I/O command dispatch (async, unit task)
- Read opcode `0x02`, Write opcode `0x01`, Flush opcode `0x00`
- SQE DWORD10-11 = start LBA (64-bit), DWORD12[15:0] = NLB (block count - 1)
- PRP1 = physical address of transfer buffer
- PRP2 = 0 if transfer ≤ page_size; physical of second page if ≤ 2×page_size;
  physical of PRP list page if larger
- Ring SQ tail doorbell after SQE placed
- Harvest: check CQ phase bit; match cid to inflight slot (cid = slot + 1);
  set io_Error/io_Actual; EndDMA/bounce copy-back; ReplyMsg

---

## AmigaOS Device Interface

### Inline commands (BeginIO, no unit task)
- `NSCMD_DEVICEQUERY` — fills `NSDeviceQueryResult`
- `TD_CHANGESTATE`, `TD_PROTSTATUS`, `TD_CHANGENUM` — return 0
- `CMD_START`, `CMD_STOP`, `TD_MOTOR`, `ETD_MOTOR` — no-op, return success
- `TD_SEEK`, `TD_SEEK64`, `NSCMD_TD_SEEK64`, `ETD_SEEK`, `TD_EJECT` — no-op
- `CMD_CLEAR`, `ETD_CLEAR`, `CMD_FLUSH` — no-op
- `TD_GETDRIVETYPE` — returns `DRIVE_NEWSTYLE`
- `TD_GETNUMTRACKS` — computed from total_blocks
- `TD_ADDCHANGEINT` — hold request (NVMe has no media change)
- `TD_REMCHANGEINT` — reply held ADDCHANGEINT, then self
- `TD_REMOVE` — hold request

### Queued commands (unit task)
- `CMD_READ`, `CMD_WRITE`, `CMD_UPDATE` (NVMe Flush)
- `ETD_READ`, `ETD_WRITE`, `ETD_UPDATE`, `ETD_FORMAT`
- `TD_FORMAT`, `TD_FORMAT64`, `NSCMD_TD_FORMAT64`, `NSCMD_ETD_FORMAT64`
- `TD_READ64`, `TD_WRITE64`
- `NSCMD_TD_READ64`, `NSCMD_TD_WRITE64`
- `NSCMD_ETD_READ64`, `NSCMD_ETD_WRITE64`, `NSCMD_ETD_SEEK64`
- `TD_GETGEOMETRY` — fills `DriveGeometry` struct
- `NSCMD_TD_GETGEOMETRY64` — fills `DriveGeometry64` (from `<libraries/mounter.h>`)
- `HD_SCSICMD` — synthesizes SCSI responses:
  - INQUIRY (standard + VPD 0x00 / 0x80 / 0x83)
  - TEST UNIT READY, READ CAPACITY 10
  - LOG SENSE (CDB 0x4D) pages 0x00 / 0x2F — `src/scsi_cmds/scsi_log_sense.c`
  - ATA PASS-THROUGH 16 / 12 (CDB 0x85 / 0xA1), ATA command 0xB0 SMART,
    sub-commands 0xD0 READ DATA / 0xD1 READ THRESHOLDS — synthesised live
    from `ctrl->smart_cache` (`src/scsi_cmds/scsi_ata_passthrough.c`).
    Attributes 9 / 12 / 192 / 194 / 196 / 231 / 233 are populated.  This
    satisfies AmigaDiskBench's SMART tab unmodified.

---

## Unit Task Architecture

### Startup handshake
```c
struct NVMeTaskStartMsg {
    struct NVMeBase *devBase;
    struct NVMeUnit *unit;
    struct Task     *parent_task;
    ULONG            ready_mask;
    LONG             ready_bit;
};
```
- Parent allocates `ready_bit = IExec->AllocSignal(-1)`
- Task created with `CreateTaskTags` under `Forbid/Permit`
- `task->tc_UserData = &startMsg` set before `Permit()`
- Task entry: `(*(struct ExecBase **)4)->MainInterface` → IExec → `FindTask(NULL)` → `tc_UserData`
- Task signals `ready_mask` when port/signal allocated (or on failure)
- Parent waits on `ready_mask`, then frees `ready_bit`

### Event loop
```
Wait(io_port_mask | io_signal_mask | SIGBREAKF_CTRL_C)
  CTRL_C → break (shutdown)
  io_signal_mask → NVMeIO_Harvest (ISR woke us)
  io_port_mask → GetMsg loop → dispatch_ioreq → NVMeIO_Harvest
```

### I/O pipeline
- `NVME_MAX_INFLIGHT = 16` slots per unit
- Each slot: one in-flight NVMe command + pre-allocated bounce buffer + PRP list page
- Bounce buffers: `NVME_BOUNCE_SIZE = 4096` bytes, `MEMF_SHARED`, pre-pinned at task start
- DMA_ReadFromRAM for writes (device reads RAM); `0` for reads (device writes RAM)
- cid = slot + 1 (avoid cid 0)

### Shutdown (v1.57 signal handshake)
- Parent allocates ack signal, stashes `{task, mask}` in the unit
- Sets `task_shutdown = TRUE`, signals `SIGBREAKF_CTRL_C`
- `Wait(ack_mask)` — proper scheduler yield (Forbid/Permit does **not**
  yield to same-priority tasks and was observed spinning 432 M iterations
  before v1.57's fix)
- Task exit: snapshot ack fields, clear `unit->task` (volatile), then Signal
  parent.  `task` and `task_shutdown` are `volatile` in `struct NVMeUnit`.

---

## Interrupt Handler

```c
static ULONG nvme_irq_handler(struct ExceptionContext *ctx,
                               struct ExecBase *exec, APTR data)
```
- Registered via `pciDev->MapInterrupt()` + `IExec->AddIntServer(irq, &handler)`
  (one per controller; `is_Data` points at `NVMeController`).
- Inspects each unit's I/O CQ phase bit.  If any CQE is ready, signals the
  corresponding unit's `io_wait_task` on its `io_signal_mask`.
- Returns `0` (pass-through) for IRQs that don't belong to us (shared INTx).
- Removed via `IExec->RemIntServer(irq_vector, &handler)` on Expunge.

### Admin IRQ permanently masked (v1.61)

The admin completion vector (`INTMS` bit 0) stays **masked** for the life
of the driver.  Admin commands are polled in `NVMe_AdminCmd`, and all I/O
CQs are created with `IEN=0` (`NVMe_CreateIOCQ` passes
`cdw11 = NVME_CQ_FLAGS_PC` without the IEN bit), so no NVMe vector should
ever fire.  `NVMe_AdminCmd` re-writes `INTMS=1` on entry as a defensive
belt-and-braces.

**Why the mask matters:** if `INTMC` is used to unmask the admin vector
while admin is polled, the first admin CQE asserts level-triggered INTx
that the ISR doesn't claim (it only inspects I/O CQs), leaving the line
asserted forever — exec re-enters the IRQ chain until the system hangs.
This was triggered pre-v1.61 by AmigaDiskBench opening the SMART tab
(first runtime admin command since boot).  The ISR is still installed —
on a shared line it is visited by other drivers' IRQs and must correctly
return not-ours.

---

## DMA Pattern

All DMA buffers use the AmigaOS 4.1 DMA API:
```c
/* Allocate MEMF_SHARED, page-aligned */
buf = IExec->AllocVecTags(size, AVT_Type, MEMF_SHARED,
                           AVT_Alignment, page_size, AVT_ClearWithValue, 0, TAG_DONE);

/* Pin and get physical address */
entries = IExec->StartDMA(buf, size, DMA_ReadFromRAM);
dma = IExec->AllocSysObjectTags(ASOT_DMAENTRY, ASODMAE_NumEntries, entries, TAG_DONE);
IExec->GetDMAList(buf, size, DMA_ReadFromRAM, dma);
phys = (ULONG)dma[0].PhysicalAddress;
IExec->FreeSysObject(ASOT_DMAENTRY, dma);  /* physical address cached */

/* Unpin on teardown */
IExec->EndDMA(buf, size, DMA_ReadFromRAM);
IExec->FreeVec(buf);
```

**Important**: Use `AVT_ClearWithValue, 0` (not `AVT_Clear, 0`) to zero MEMF_SHARED buffers.
`AVT_Clear, 0` does NOT clear the memory.

Persistent pinning: admin queues, identify buffer, I/O queues, bounce buffers, and PRP
list pages are all pinned for the lifetime of the controller/unit — no per-I/O StartDMA
for pre-pinned buffers.

---

## Boot Strategy

`nvme.device` is placed inside `kickstart.zip` (in the `Kickstart/` folder).
bboot loads the entire zip into RAM before starting Kickstart, so the driver is
available to the resident scanner before any disk is accessed. `mounter.library`
finds and mounts NVMe namespaces automatically.

See `nvme_boot_strategy.md` for the full QEMU command line and step-by-step setup.

---

## Key Implementation Notes

### `UQUAD` does not exist in AmigaOS 4 SDK
Use `uint64` (from `<exec/types.h>`) for 64-bit values.

### `ASOT_TASK` / `ASOTASK_*` do not exist
Use `IExec->CreateTaskTags(name, priority, entry, stacksize, TAG_DONE)`.

### No `IExec->Delay()` in driver context
`Delay()` is dos.library. Use a busy-poll loop or `Forbid/Permit` yield.

### `DMA_WriteToRAM` does not exist
Use `0` for device→RAM transfers (disk reads). `DMA_ReadFromRAM` is for RAM→device
(disk writes).

### memset/memcpy require `src/compat.c`
Driver uses `-nostartfiles` so newlib (`INewlib`) is not linked. `src/compat.c`
provides minimal `memset`/`memcpy` implementations.

### GCC 11 loop→memset/memcpy synthesis
`-fno-tree-loop-distribute-patterns` prevents GCC 11 `-O2` from synthesising calls
to `memset`/`memcpy` from loops. Required in CFLAGS.

---

## File Layout

```
src/
  device.c           — Resident struct, interface vectors (RTF_NATIVE|COLDSTART|AUTOINIT), pri 0
  Init.c             — _manager_Init: libs, PCI discovery, per-controller init + unit discovery, IRQ
  Open.c             — Lazy unit task start on first open
  Close.c            — Unit shutdown on last close (signal-handshake)
  Expunge.c          — Full reverse-order teardown + debug-only leak dump
  BeginIO.c          — Inline command dispatch, async queue, NSCMD_NVME_GETSTATS
  unit_discovery.c   — Per-controller namespace enumeration + mounter.library announce
  unit_task.c        — Per-unit async I/O task, inflight pipeline, TD_GETGEOMETRY, HD_SCSICMD dispatch
  compat.c           — memset/memcpy for -nostartfiles driver context
  nvme_mmu.c         — BAR0 cache-inhibit + guarded MMU attrs
  nvme_status.c      — Unified CQE-status → io_Error mapper
  nvme_stats.c       — NSCMD_NVME_GETSTATS handler + snapshot assembly
  nvme_leak.c        — Debug-only allocation counters + Expunge dump
  nvme/
    nvme_init.c      — Controller reset/enable/identify
    nvme_admin.c     — Admin SQE/CQE (polled, INTMS bit 0 masked on entry); NVMe_RefreshSMART
    nvme_io.c        — I/O Submit / Flush / Harvest + PRP1/PRP2/PRP-list build
    nvme_irq.c       — Per-controller INTx ISR + install/remove
  pci/
    pci_discovery.c  — FDT_Index loop over NVMe controllers, BAR mapping
    platform_detect.c — Host-bridge VID/DID table + BAR0 MMIO forwarding probe
  scsi_cmds/
    scsi_ata_passthrough.c — CDB 0x85 / 0xA1 → ATA 0xB0 SMART synthesis from NVMe Log Page 0x02
    scsi_log_sense.c       — CDB 0x4D LOG SENSE pages 0x00 / 0x2F
include/
  version.h          — DEVNAME, DEVVER, DEVREV, BUILD_DATE, BUILD_TIME
  nvme.h             — BAR0 offsets (NVMe 1.4 citations), CAP/CC/CSTS/AQA macros, SQE/CQE, opcodes
  nvme_device.h      — NVMeBase, NVMeController, NVMeUnit, NVMeInflight + nvme_r32/nvme_w32
  nvme_admin.h       — Admin command prototypes (all take NVMeController *)
  nvme_init.h        — InitNVMe / CleanupNVMe
  nvme_io.h          — NVMeIO_Submit / Flush / Harvest
  nvme_irq.h         — InstallNVMeInterrupt / RemoveNVMeInterrupt
  nvme_stats.h       — Wire NVMeStats + in-driver NVMeUnitStats + TBR helpers
  nvme_status.h      — NVMe_StatusToIOErr / StatusDescribe
  nvme_mmu.h         — MMU helper prototype
  nvme_platform.h    — NVMePlatform enum + PlatformDetect / MMIOProbe
  nvme_debug.h       — DPRINTF (debug-only) + deprecated DLOG alias (v1.59)
  nvme_scsi.h        — Prototypes for scsi_cmds/ handlers
  nvme_leak.h        — Debug-only NVME_LEAK_INC/DEC macros
  unit_discovery.h   — DiscoverUnits prototype
  unit_task.h        — UnitTask_Start / Shutdown prototypes
  pci/pci_discovery.h — DiscoverNVMe prototype
tests/
  test_nvme.c        — 10-step functional test (INQUIRY, geometry, read, write+verify,
                        64 KiB round-trip, TD_READ64 high-offset)
  nvme_stats.c       — CLI monitor for NSCMD_NVME_GETSTATS (one-shot / watch / summary)
docs/
  modernization_plan.md — 16-commit plan (authoritative — check here first)
  architecture.md    — This file
  nvme_boot_strategy.md — QEMU + kickstart.zip boot setup
  history.md         — Session-by-session changelog
```

---

## Validation Status (v1.61, QEMU Pegasos2)

### End-to-end validated

- [x] Boot banner + per-controller init on all supported platforms
- [x] Admin command completion (polled; INTMS bit 0 permanently masked)
- [x] Identify Controller / Namespace response parsing
- [x] I/O queue creation with `IEN=0` (no NVMe IRQs ever fire)
- [x] CMD_READ / CMD_WRITE end-to-end; AmigaDiskBench 265–279 MB/s sustained
- [x] mounter.library announce — NVMe partition appears on Workbench
- [x] INTx ISR — shared-line compatible, returns not-ours for non-NVMe IRQs
- [x] NSCMD_TD_GETGEOMETRY64 via `DriveGeometry64` from `<libraries/mounter.h>`
- [x] HD_SCSICMD: INQUIRY (std + VPD 0x00/0x80/0x83), READ CAPACITY 10,
      TEST UNIT READY, LOG SENSE 0x00/0x2F
- [x] **ATA PASS-THROUGH SMART (v1.60)** — AmigaDiskBench SMART tab shows
      live NVMe telemetry unmodified
- [x] Media Toolbox partitioning, SFS/00 format, multi-MB/s transfers
- [x] Shutdown signal handshake (v1.57) — no more 432 M iteration spin
- [x] Bounce-path PRP2 (v1.58) — multi-page transfers stable at 64 KiB bounce
- [x] Release build emits only the startup banner on serial (v1.59 cleanup)
- [x] Admin IRQ storm prevented (v1.61) — SMART tab no longer freezes system

### Still to validate

- [ ] Multi-controller on physical hardware (tested single-controller in QEMU)
- [ ] Boot from NVMe as SYS: (full AmigaOS install)
- [ ] Real-hardware testing on non-Pegasos2 platforms

### Known constraints

- `DebugPrintF` does not support `%llu` — 64-bit values print as garbage.
  Use split hi/lo 32-bit prints or cast to `(ULONG)` if value fits.
- diskboot.kmod only activates its scan pipeline when it finds at least one
  VirtIO SCSI device in the QEMU config; NVMe-only is not sufficient.  The
  QEMU command line must include a VirtIO SCSI device (even with a stub
  drive) for diskboot to also probe nvme.device.
- AmigaOne XE/SE (Mai Logic Articia S) is unsupported — bridge does not
  forward MMIO to PCIe.  Driver detects this and aborts Init cleanly.
