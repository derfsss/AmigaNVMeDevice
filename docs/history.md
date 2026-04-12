# nvme.device — Changelog

## Session 8 — 2026-04-12 (late)

### Post-modernization bug-fix sweep — v1.55 → v1.61

With the 16-commit modernization in the can, we hit four real-world
issues while validating on QEMU Pegasos2 with Media Toolbox / SFS /
AmigaDiskBench.  Each fix ships as a single version bump.

**v1.56 — bounce DMA regression fix.**  The virtioscsi-style
`EndDMA(DMAF_NoModify)` pattern introduced in commit 7 left the 64 KiB
bounce buffers in an unpredictable cache-attribute state on Pegasos2's
MV64361 + our MMU CI+G setup; the first `CMD_READ` to unit 0 would
never complete (CQE arrives but bounce-buffer contents are stale).
Reverted to v1.47's "persistent `StartDMA` pin for the unit-task
lifetime" pattern — bounce stays cache-inhibited, no `CacheClearE`
needed.  Kept the 64 KiB size.

**v1.57 — shutdown handshake.**  The `UnitTask_Shutdown` busy-wait
```
  task_shutdown = TRUE;
  Signal(unit->task, SIGBREAKF_CTRL_C);
  while (unit->task != NULL) { Forbid(); Permit(); }
```
turned out to be a deadlock when the calling task (`exec.task`) and
the unit task run at the same priority.  `Forbid`/`Permit` is a
nesting-count barrier, **not** a yield, so the unit task never got
scheduled and `unit->task` never cleared.  We observed 432 million
spin iterations in the real log before QEMU was killed.  Replaced
with a proper signal handshake: parent `Wait()`s on a dedicated
signal bit, the unit task `Signal()`s back after clearing
`unit->task`.  Also made `task` and `task_shutdown` fields `volatile`.

**v1.58 — bounce-path PRP2 fix.**  When `NVME_BOUNCE_SIZE` went from
4 KiB to 64 KiB in commit 7, the bounce-path still unconditionally
set `prp2_phys = 0`.  That was correct for ≤1-page transfers but
caught us on Media Toolbox's 8 KiB (2-page) RDB scan — the controller
read past the first page and QEMU hard-froze.  Now we build PRP2
(for 2-page transfers) or fill the PRP-list page (for 3+ pages)
arithmetically — the bounce buffer is contiguous, so page _k_ is
`bounce_phys + k*page_size`.

**v1.59 — release-build debug leaks.**  The `DLOG` macro (intended
as "always-on milestone logging") + three direct `IExec->DebugPrintF`
calls in `Open.c` and `unit_task.c` were spilling a dozen-plus lines
into release-build serial logs.  Only the startup banner should
survive release.  Consolidated: `DLOG` is now an alias for `DPRINTF`
(compiles out in release), and the stray direct calls converted to
`DPRINTF`.  Release binary shrank from 73,652 → 72,256 bytes.
`strings build/nvme.device | grep '^\['` now returns exactly one
format string — the banner.

**v1.60-v1.61 — ATA PASS-THROUGH SMART.**  Added
`src/scsi_cmds/scsi_ata_passthrough.c` and `scsi_log_sense.c`
(mirroring virtioscsi's file layout), wired CDB `0x85` / `0xA1` /
`0x4D` into the existing `handle_scsi_cmd` dispatch in
`unit_task.c`.  The 512-byte ATA SMART block is synthesised live
from `ctrl->smart_cache` (NVMe Log Page 0x02), mapping NVMe fields
into ATA attribute IDs 9/12/192/194/196/231/233.  AmigaDiskBench's
SMART tab now shows real NVMe temperature / power-on hours /
wear% / spare% without any changes on the ADB side.

A runtime admin command (the first SMART refresh) immediately
hard-froze the system.  Root cause: `Init.c` step (7) was unmasking
`INTMC` at end of init, which let admin CQEs fire level-triggered
INTx.  Our ISR only inspects I/O CQs (correct for shared INTx),
returns "not ours", line stays asserted, exec re-enters the handler
chain forever.  Fix: keep `INTMS` bit 0 masked permanently (I/O CQs
already have `IEN=0` per `NVMe_CreateIOCQ` so admin is the only
would-be IRQ source), and `NVMe_AdminCmd` now defensively re-masks
at every entry.  Documented in
`feedback_task_sync_gotchas.md` Gotcha 3.

### Validation (v1.61)

Clean boot to DOS / Workbench.  Media Toolbox partitions the NVMe
volume; SFS/00 format completes without errors.  AmigaDiskBench
runs 10 benchmarks end-to-end:

| Benchmark | Result |
|---|---|
| Sequential write (1 MiB blocks) | 278.94 MB/s |
| HeavyLifter (1 MiB) | 265.34 MB/s |
| Legacy (1 MiB) | 267.26 MB/s |
| Random 4 KiB write | 225.38 MB/s |
| Sequential read | 206.91 MB/s |
| Random 4 KiB read | 196.57 MB/s |
| Mixed 70/30 | 204.00 MB/s |

Debug-log statistics across ADB runs: 71,568 `NVMeIO_Submit` calls,
71,602 `status=0x0000` completions, 0 non-success statuses, 0
leaks/timeouts.  All three PRP branches exercised (single-page,
PRP2-direct, PRP list).

### Build sizes (v1.61)

| artefact | size |
|---|---|
| `build/nvme.device` (release) | 72,620 B |
| `build/nvme.device.debug` | 80,088 B |
| `build/test_nvme` | 70,036 B |
| `build/nvme_stats` | 79,268 B |
| `build/nvme.lha` (distribution) | 69,470 B |

The release binary emits only the startup banner on the serial
console.  Debug build emits full trace (admin queue slot/phase,
ATA-SMART call tree, I/O Submit / Harvest) — invaluable for
diagnosing future issues but off by default.

---

## Session 7 — 2026-04-12

### Modernization sweep — 16-commit plan landed (v1.47 → v1.55)

Delivered the full plan in `docs/modernization_plan.md` — sixteen
self-contained commits, each building clean and deploying to
`s:/temp/` as a sanity check before moving on.

**Commit summaries**:

1. `eieio` → `sync` MMIO barrier; MMU CACHEINHIBIT+GUARDED on BAR0;
   new `include/nvme_debug.h` with `DPRINTF` (debug-only) and `DLOG`
   (always-on); Makefile-injected `BUILD_DATE`/`BUILD_TIME`; always-on
   startup banner showing name/version/build stamp.
2. Host-platform identification + BAR0 MMIO forwarding probe
   (`src/pci/platform_detect.c`).  Four failure modes classified;
   driver aborts cleanly on Articia S with a descriptive diagnostic.
3. Makefile split — release (`build/nvme.device`) and debug
   (`build/nvme.device.debug`) variants from a single source tree;
   `make deploy` / `make deploy-debug` both land at the same
   `$(DEPLOY_DIR)/nvme.device`.
4. Removed the four empty `src/exec_cmds/cmd_*.c` stub files (real
   dispatch lives in `unit_task.c`).  Resident priority `-60` → `0`
   to match `virtioscsi.device` for boot-drive compatibility.
5. Professional comments pass — module doc blocks for every source
   file, header-level doc blocks for every public include, NVMe 1.4
   spec-section citations in `include/nvme.h` register table.
6. Error-handling hardening — unified Linux-kernel `goto err:`
   unwind with `have_*` flags; new
   `src/nvme_status.c::NVMe_StatusToIOErr()` as the single
   CQE-status → `io_Error` mapper; timeouts on every poll loop.
7. Bounce buffer 4 KiB → 64 KiB.  `StartDMA`/`GetDMAList`/`EndDMA
   (DMAF_NoModify)` pattern to cache the physical address while
   leaving the buffer cacheable.  `CacheClearE(CACRF_ClearD)` before
   write-path doorbell, `CacheClearE(CACRF_InvalidateD)` before
   read-path `CopyMem`.  Mirrors virtioscsi.device.
8. PRP-list scatter-gather rewrite — walks the DMAEntry list
   correctly for fragmented >2-page transfers, writes the list page
   in place (no stack buffer), overflow returns `IOERR_BADLENGTH`
   cleanly.
9. IRQ → polling-mode fallback.  `ctrl->polling_mode` tracks
   `MapInterrupt`/`AddIntServer` failure; the existing yield-poll
   loop is the polling path.  Init logs the mode chosen.
10. **Multi-controller refactor** — biggest commit in the plan.  New
    `struct NVMeController` extracted from `NVMeBase`; each controller
    has its own admin queues, ISR, polling mode, and up to 8 units.
    `NVMeBase.controllers[4]` + flat `global_units[32]` lookup table.
    `DiscoverNVMe` loops `FDT_Index` to enumerate every NVMe PCI
    device.  Per-controller ISR `is_Data` points at its controller.
11. Memory-leak audit — new `include/nvme_leak.h` + `src/nvme_leak.c`
    with 10 debug-only counters (vec / dma / dmaentry / port /
    signal / library / interface / pcidev / resource / irq).  Every
    alloc is paired with a matching free + `NVME_LEAK_INC`/`DEC`;
    Expunge prints a table with `OK` / `LEAK` flags.  **Fixed a
    pre-existing leak**: `unit->io_sq` / `io_cq` and their DMA
    mappings were never released at Expunge.
12. `tests/test_nvme.c` rewritten with pass/fail counters; tests now
    include HD_SCSICMD INQUIRY, `NSCMD_TD_GETGEOMETRY64`, 64 KiB
    round-trip verify at offset 4 KiB with byte-keyed pseudo-random
    pattern, and `TD_READ64` at an offset > 4 GiB into the namespace.
13. Stats core — `include/nvme_stats.h` + `src/nvme_stats.c`.  New
    `NSCMD_NVME_GETSTATS` (0xA100) returns a versioned snapshot.  Hot-
    path counters in Submit/Harvest/ISR (bytes, per-opcode commands,
    error buckets, bounce/direct/PRP-list hits, inflight current/peak,
    MDTS splits, UNITBUSY rejections).  Latency measured in native
    PPC Time Base Register ticks (`mftb`/`mftbu`) scaled by
    `ExecBase->ex_EClockFrequency`.
14. SMART refresh behind `-DENABLE_SMART` (default on; disable via
    `make NO_SMART=1`).  Admin Get Log Page 0x02 at lazy cadence
    (30 s) from `NSCMD_NVME_GETSTATS` handler; results cached in
    `ctrl->smart_cache`.
15. `tests/nvme_stats.c` CLI monitor — one-shot / watch / summary
    modes; pretty-prints every snapshot field with byte-count and
    µs-from-ticks conversion.
16. Docs sync + `nvme.readme` + `amiupdate.yml` + Makefile `dist` /
    `dist-lha` targets — produces an AmiUpdate-ready `build/nvme.lha`
    containing both driver flavours, test program, stats CLI, plain-
    text readme, diskboot.config sample, and a generated `AutoInstall`
    script (via `../AmiUpdateIntegration/generate_autoinstall.py`).

### Platform support after the sweep

Single binary runs on any AmigaOS 4.1 FE platform with a working
PCIe bridge.  Runtime detection in `src/pci/platform_detect.c`
identifies the host; BAR0 MMIO probe gates init.  Original AmigaOne
XE/SE (Articia S) fails at the probe and aborts cleanly.

### Build size snapshot

| Build | Size |
|---|---|
| `build/nvme.device` (release, SMART on) | 73,732 B |
| `build/nvme.device.debug` | 78,740 B |
| `build/test_nvme` | 70,036 B |
| `build/nvme_stats` | 79,268 B |
| `build/nvme.lha` (full distribution) | 67,288 B |

Release binary is essentially the same size as v1.47 despite the
much larger feature set — the per-debug-counter macros, DLOG logs,
and leak-tracking helpers all evaporate in the release `-DDEBUG`-off
compile path.

---

## Session 6 — 2026-03-05

### NVMe drive mounts on Workbench! Debug cleanup and documentation

**v1.47**: Debug cleanup + DriveGeometry64 fix + documentation update
- **NVMe partition successfully mounts on Workbench** as "Empty" (SFS filesystem)
  - Mounter Task opens nvme.device unit 0, SFS reads blocks via CMD_READ
  - Requires VirtIO SCSI device present in QEMU config for diskboot.kmod to activate
- Fixed `NSCMD_TD_GETGEOMETRY64`: was filling `struct DriveGeometry` (wrong struct) —
  changed to `struct DriveGeometry64` from `<libraries/mounter.h>` (different layout:
  uint64 dg_TotalSectors, dg_BufMemTags pointer, no C/H/S fields)
- Stripped verbose debug output from all source files:
  - nvme_io.c: removed Data hex dumps, RDB/PART block decode, DMA cleanup messages
  - nvme_init.c: removed admin queue phys address dumps, identify buffer phys
  - unit_discovery.c: removed duplicate AnnounceDeviceTags message
  - BeginIO.c: reduced verbose log to concise DPRINTF (cmd, len, off)
- Reverted 40 `[NVME-DBG]` fprintf lines from QEMU `hw/nvme/ctrl.c`

### Key discoveries
- **diskboot.kmod activation**: diskboot.kmod only activates when it finds a VirtIO SCSI
  device in the system. Without one, nvme.device is ignored even if listed in diskboot.config.
  Adding `-device virtio-scsi-pci-non-transitional` (even with no drives) is sufficient.
- **`diskboot.config` format**: `devicename maxunits flags` (1=HD, 2=CD, 3=both).
  `nvme.device 1 1` required. File goes in `Devs/` or `S:/temp/` for QEMU VVFAT.
- **`struct DriveGeometry64` ≠ `struct DriveGeometry`**: completely different layouts.
  DriveGeometry64 has uint64 fields, no C/H/S. Filling the wrong struct causes mounter
  to read garbage geometry.
- **DebugPrintF `%llu` broken**: PPC varargs don't handle 64-bit printf — values print
  as garbage. Use split hi/lo 32-bit prints.

---

## Session 5 — 2026-03-05

### Admin commands working, I/O pipeline functional

**v1.11–v1.30**: Admin command completion fixed
- Admin CQ doorbell wasn't being written — controller never advanced CQ head
- CQ phase bit logic corrected (starts at 1, flips on wrap)
- Admin SQ tail wrap-around fixed
- DMA direction for SQ fixed: SQ is RAM→device = `DMA_ReadFromRAM`
- Identify Controller returns valid model string and MDTS
- Identify Namespace returns correct block count and LBA format
- Create I/O CQ/SQ admin commands succeed

**v1.31–v1.40**: I/O submit/harvest pipeline
- Bounce buffer DMA mapping and copy-back for non-MEMF_SHARED user buffers
- PRP list construction for multi-page transfers
- I/O CQ phase bit tracking per unit
- ISR installed via `pciDev->MapInterrupt()` + `IExec->AddIntServer()`
- ISR signals unit tasks on completion; Harvest drains CQ
- INTMS/INTMC masking: ISR masks all interrupts, Harvest unmasks after draining
- CMD_READ returns valid data from NVMe controller

**v1.41–v1.45**: HD_SCSICMD and NSD commands
- HD_SCSICMD handler: synthesizes SCSI responses for INQUIRY, TEST UNIT READY,
  READ CAPACITY 10, VPD pages 0x00/0x80/0x83
- Full NSD command set implemented (TD_READ64, TD_WRITE64, NSCMD_* variants,
  ETD_* variants, TD_FORMAT64, NSCMD_TD_FORMAT64, etc.)
- NSCMD_DEVICEQUERY returns complete supported command list

---

## Session 4 — 2026-03-05

### Doorbell write fix + first admin command attempt

**v1.10–v1.11**: Doorbell investigation continued
- Doorbell write at BAR0+0x1000 initially hung the CPU
- Root cause: AmigaOne-specific code was interfering with Pegasos2 MMIO
- After removing AmigaOne code, volatile pointer write to doorbell succeeded
- Admin command still times out — CQE all zeros (DMA physical address issue)

---

## Session 3 — 2026-03-05

### Pegasos2 target migration and boot testing

**v1.4**: First Pegasos2 boot test
- Removed `IMMU->MapMemory()` code (crashes in user mode)
- Added MMIO validation — CAP_LO must be non-zero or driver aborts
- **Result**: MMIO works on Pegasos2! CAP_LO = 0x0F0107FF (real NVMe capabilities)
- Controller reaches ready state (CSTS.RDY=1)
- Hangs during admin command (Identify Controller)

**v1.5**: CQ buffer zeroing fix
- `AVT_Clear, 0` does NOT clear MEMF_SHARED memory — changed to `AVT_ClearWithValue, 0`
- Removed `pciDev->InLong()`/`InByte()` diagnostic code (returns garbage `0xA5A55A5A` on Pegasos2 MMIO)
- Added debug prints for admin queue physical addresses and identify buffer
- **Result**: Still hangs during admin command

**v1.6**: DMA direction fix
- CQ used `DMA_ReadFromRAM` but device writes TO CQ — changed to `0`
- Identify buffer same fix — direction `0` for device→RAM
- EndDMA calls updated to match
- Added CQE diagnostic dump on timeout
- **Result**: Still hangs

**v1.7**: Doorbell write investigation
- Added DebugPrintF before and after doorbell write
- Reduced poll iterations from 5M to 500K for QEMU TCG speed
- **Result**: Hang is in the doorbell WRITE itself — "Writing doorbell" prints but "Doorbell written OK" never appears. `stwbrx` to BAR0+0x1000 hangs the CPU.

**v1.8**: Doorbell write alternatives
- Added INTMS test write (offset 0x0C) before doorbell — confirms low-offset writes work
- Tried volatile pointer write for doorbell instead of `stwbrx` — same hang
- **Result**: Both `stwbrx` and volatile pointer write hang at offset 0x1000

**v1.9**: Diagnostic cleanup
- Same code as v1.8 with minor cleanup

**v1.10**: AmigaOne code removal + Pegasos2 milestone
- Removed all AmigaOne-specific code (BAR0 force-assign, Articia-S error messages)
- Updated comments in nvme.h and nvme_device.h to remove AmigaOne references
- **Result**: Doorbell write NO LONGER hangs! Volatile pointer write works.
  CSTS still 0x01 (ready, no fatal error) after doorbell write.
  But admin command times out — CQE is all zeros. Controller never writes completion.
  Diagnosis: likely DMA physical address issue — controller can't find SQ/CQ buffers.

### Key discoveries
- **Pegasos2 MV64361 MMIO confirmed working**: `lwbrx`/`stwbrx` gives valid register values
- **AmigaOne Articia-S MMIO confirmed broken**: no MMIO access method works
- **NVMe on Pegasos2 BAR0**: Base=0x84200000, Size=16384, Flags=0x02 (Mem)
- **pciDev->InLong() on Pegasos2 MMIO**: returns garbage — only `lwbrx`/`stwbrx` works
- **DMA direction**: `DMA_ReadFromRAM` = RAM→device (writes); `0` = device→RAM (reads)
- **AVT_ClearWithValue vs AVT_Clear**: only `AVT_ClearWithValue, 0` actually zeroes MEMF_SHARED

---

## Session 2 — 2026-03-04

### Build fixes
- `UQUAD` → `uint64` throughout (`nvme.h`, `nvme_device.h`, `nvme_io.c`, `test_nvme.c`)
  — `UQUAD` is not defined in the AmigaOS 4 SDK; correct type is `uint64` from `<exec/types.h>`
- `ASOT_TASK` / `ASOTASK_*` not available — replaced with `IExec->CreateTaskTags()`
  (same API as virtioscsi.device)
- Added `src/compat.c` — provides `memset`/`memcpy` for `-nostartfiles` driver context;
  removes dependency on newlib (`INewlib` undefined in driver)
- Added `src/nvme/nvme_irq.c` to Makefile `SRC` list (was missing → linker errors for
  `InstallNVMeInterrupt`/`RemoveNVMeInterrupt`)
- Makefile: `test_nvme` target now depends on `$(TARGET)` to avoid parallel link race
- `DMA_WriteToRAM` does not exist — replaced with `0` (device→RAM transfers)
- `IOERR_NOMEMORY` does not exist — replaced with `IOERR_ABORTED`
- Fixed missing cast `(ULONG)inf->dma_list[1].PhysicalAddress`
- Removed unused `IExec` variable in `NVMeIO_Flush`
- `test_nvme.c` rewritten to use `IExec->` interface calls (AmigaOS 4 style) and
  `AllocVecTags(MEMF_SHARED)` instead of deprecated legacy `CreateMsgPort`/`AllocVec`
- Added `<stdlib.h>` to `test_nvme.c` for `strtol`

### Result: clean build — `build/nvme.device` and `build/test_nvme` produced with no errors

---

### Key TODOs implemented
- **`pci_discovery.c`**: fixed `iexec_dbg()` → `IExec->DebugPrintF()`
- **`unit_task.c`**: rewrote with correct AmigaOS 4 patterns:
  - `struct NVMeTaskStartMsg` for parent/task ready handshake
  - IExec retrieved via `(*(struct ExecBase **)4)->MainInterface` in task entry
  - `tc_UserData` set under `Forbid/Permit` before task becomes runnable
  - `IUtility->SNPrintf` for per-unit task name
  - `Forbid/Permit` yield loop in `UnitTask_Shutdown` (no `IExec->Delay`)
- **`unit_discovery.c`**: added mounter.library announce — opens `mounter.library`,
  calls `IMounter->AnnounceDeviceTags(DEVNAME, unit_num, TAG_END)` for each unit
- **`nvme_irq.c`**: implemented `pciDev->MapInterrupt()` + `IExec->AddIntServer()` +
  `IExec->RemIntServer()`, matching virtioscsi.device pattern exactly
- **`nvme_device.h`**: added `irq_vector` field to `NVMeBase`

---

## Session 1 — 2026-03-04

### Project created
- New project `projects/AmigaNVMeDevice/` created from scratch
- Git repository initialised with initial commit (34 files)

### Files created
- `Makefile` — ppc-amigaos-gcc, `-nostartfiles`, `-fno-tree-loop-distribute-patterns`
- `.gitignore`, `.clangd`
- `include/version.h` — `DEVNAME "nvme.device"`, `DEVVER 1`
- `include/nvme.h` — BAR0 register offsets, SQE/CQE structs, opcodes, Identify structs
- `include/nvme_device.h` — `NVMeBase`, `NVMeUnit`, `NVMeInflight` structs
- Sub-headers: `nvme_init.h`, `nvme_admin.h`, `nvme_io.h`, `nvme_irq.h`,
  `unit_discovery.h`, `unit_task.h`, `pci/pci_discovery.h`
- All `src/` files (device.c, Init.c, Open.c, Close.c, Expunge.c, BeginIO.c,
  unit_discovery.c, unit_task.c, nvme/*.c, pci/*.c, exec_cmds/*.c)
- `docs/nvme_boot_strategy.md`, `docs/architecture.md`
- `README.md`, `tests/test_nvme.c`

### Design decisions
- **Pegasos2 only (MV64361 bridge)**: `lwbrx`/`stwbrx` MMIO; no AmigaOne support
- **No new wb library**: post-Workbench enrichment via existing `blockdev.library`
- **Kickstart module boot**: `nvme.device` placed in `kickstart.zip/Kickstart/`
- **Single-phase driver**: `RTF_COLDSTART`, full I/O at boot — no second resident
