# nvme.device — Changelog

## Session 12 — 2026-06-10

### v1.67 — Real-hardware readiness + chunked-path correctness

Full code-review pass with one goal: make the driver work on **any**
AmigaOS 4.1 installation with **any** NVMe SSD, and guarantee a clean
no-op when no NVMe hardware is present.

Hardware-enablement changes:

- **PCI class-code discovery** (`pci_discovery.c`): controllers are now
  matched on class 0x01 / subclass 0x08 / progif 0x02 via
  `FDT_Class`/`FDT_ClassMask` (same pattern as usb2's HCD scan) instead
  of QEMU's hardcoded `1B36:0010` vendor/device pair.  Every real
  Samsung/WD/Kingston/etc. SSD is now found; QEMU's controller still
  matches because it reports the same class triple.  The real VID/DID
  is read from config space for diagnostics.
- **CC.MPS honours CAP.MPSMIN** (`nvme_init.c`): `NVME_CC_DEFAULT`
  bakes in MPS(0); a controller with MPSMIN > 0 would have refused the
  enable.  The enable write now ORs in `NVME_CC_MPS(mpsmin)`.
- **Ready-poll budget scales with CAP.TO**: real silicon may take many
  seconds to come ready; the fixed 5M-iteration poll now grows to
  `(TO+1) × 1M` iterations when CAP.TO demands it.
- **I/O queue depth clamped to CAP.MQES**: a controller with
  MQES+1 < 64 no longer fails Create CQ/SQ; depth is clamped
  per-controller (and the controller is rejected outright if it cannot
  cover the 16 inflight slots + 1).
- **PCI INTx-disable bit cleared** at device enable — some firmwares
  leave it set, which would have silently killed INTx delivery.
- **BAR0 size sanity check** (≥ 0x2000) before the MMIO probe.
- **SCSI INQUIRY reports real Identify data**: vendor "NVMe", product
  = Identify model number, revision = firmware revision; VPD 0x80
  serial = controller serial + namespace suffix; VPD 0x83 T10
  designator built from the model.  Multi-digit NSIDs now format
  correctly.

Correctness fixes:

- **Chunked (>MDTS) path could swallow an unrelated IORequest**: the
  per-chunk completion wait redirected `mn_ReplyPort` to the unit's own
  port and discarded the reply with `GetMsg` — but `GetMsg` pops the
  port HEAD, which could be a *new* request that arrived mid-chunk,
  hanging its caller forever.  The path now uses the same
  `suppress_reply` mechanism as `NVMeIO_SubmitAndWait`; no port
  juggling, no message loss.  The poll loop also gained the
  Forbid/Permit yield (QEMU TCG needs the guest to yield to post CQEs)
  and the timeout path now releases the slot's direct-DMA pin and
  fixes the inflight counter instead of leaking both.
- **Block-alignment validation** in `NVMeIO_SubmitNoRing`: misaligned
  io_Offset / io_Length used to truncate silently to whole blocks while
  still reporting `io_Actual == io_Length`; they are now rejected with
  `IOERR_BADADDRESS` / `IOERR_BADLENGTH`.  NULL io_Data with a non-zero
  length is rejected too.
- **Expunge teardown order**: controllers are quiesced (IRQ removed,
  CC.SHN shutdown completed) *before* the unit I/O CQ/SQ backing pages
  are freed — previously a still-enabled controller could in principle
  DMA into freed memory.
- **`StartDMA` return values checked** in `unit_discovery.c` (a zero
  entry count previously flowed into `AllocSysObjectTags`/`GetDMAList`).
- NULL `scsi_Data` guards added to INQUIRY and READ CAPACITY(10)/(16).

Verified via AmigaQemuTests on Pegasos2 (new project configs
`AmigaNVMeDevice.json` / `AmigaNVMeDevice_NoDevice.json`):

- With `-device nvme`: full `test_nvme` functional suite — **0 failed**
  (banner v1.67, discovery, geometry, read, write+verify, 64 KiB
  round-trip, TD_READ64 high-offset on the 5 GB image).
- Without any NVMe device: Kickstart loads the module, Init finds no
  controllers, unwinds and returns NULL; the system boots normally and
  all standard DOS tests pass.  `OpenDevice("nvme.device")` fails
  cleanly as expected.

Note for future sessions: the "AVT_Clear does not clear" gotcha in
CLAUDE.md is wrong — `exec/exectags.h` defines `AVT_Clear`,
`AVT_ClearWithValue`, and `AVT_ClearValue` as the *same* tag
(TAG_USER+6), so `AVT_Clear, 0` and `AVT_ClearWithValue, 0` are
identical.

## Session 11 — 2026-04-12 (night)

### v1.66 — SCSI feature surface expanded: TRIM, RC16, SYNC CACHE, MODE 0x08

Not a performance commit — a **feature-completeness** commit.  A
survey of `../AmigaBlockDevLibrary` identified four SCSI sub-commands
that the library (and any SAT-aware tool) needed from our
`HD_SCSICMD` surface but which `nvme.device` rejected with CHECK
CONDITION.  All four are now implemented and dispatched from
`handle_scsi_cmd` in `unit_task.c`:

**1. SYNCHRONIZE CACHE(10) — CDB 0x35**.  Inline handler.  Builds an
NVMe Flush SQE and blocks the unit task on the I/O queue via the new
`NVMeIO_SubmitAndWait` primitive until the controller replies.  NVMe
Flush is device-wide (no per-LBA semantic), so any range translates
honestly to a whole-namespace flush — the client sees GOOD on success
or CHECK CONDITION + MEDIUM ERROR on failure.  ~30 LOC.

**2. READ CAPACITY(16) — CDB 0x9E, service action 0x10**.  Inline
handler.  32-byte response with full 64-bit last LBA and 32-bit
block size; zeroed P_TYPE / PROT_EN / LBPRZ / LBPPBE / LALBA fields
since we're not advertising protection information or logical-to-
physical block exponent tricks.  Future-proofs us for > 2 TiB
namespaces which are spec-legal and increasingly common.  ~50 LOC.

**3. UNMAP (TRIM) — CDB 0x42**.  New handler in
`src/scsi_cmds/scsi_unmap.c`.  Parses the SPC-4 UNMAP parameter list
(header + up to 256 16-byte block descriptors), translates each
descriptor from SCSI format `{LBA:8, blocks:4, reserved:4}` to NVMe
format `{cattr:4, nlb:4, slba:8}` (little-endian on the wire) into
the unit's pre-pinned `dsm_buf`, and issues a single NVMe Dataset
Management command (opcode 0x09) with `AD=1` (Deallocate attribute).
The handler blocks the unit task via `NVMeIO_SubmitAndWait`, then
translates the NVMe status into SCSI `scsi_Status`.

Safety gates:
- `ctrl->onc_dsm` (ONCS bit 2, parsed from Identify Controller byte
  520 at init) — refuse with CHECK CONDITION / ILLEGAL REQUEST if
  the controller hasn't advertised DSM support.
- `unit->dsm_buf` allocated — if the per-unit 4 KiB pinned range
  buffer failed to allocate at `UnitTask_Start`, the whole TRIM path
  stays disabled on that unit.
- CDB ANCHOR bit rejected (we don't support anchoring).
- Range count capped at `NVME_DSM_MAX_RANGES = 256` — NVMe's single-
  command limit.  Longer lists could be split into multiple DSM
  commands; not wired since no client we care about issues more.

First NVMe TRIM implementation in the Amiga ecosystem.

**4. MODE SENSE / MODE SELECT page 0x08 (Caching)**.  New handler in
`src/scsi_cmds/scsi_mode.c`.  Handles the 6-byte (CDB 0x1A / 0x15)
and 10-byte (CDB 0x5A / 0x55) variants; translates between SCSI page
0x08 byte 2 bit 2 (WCE — Write Cache Enable) and NVMe feature 0x06
(Volatile Write Cache) via `NVMe_SetFeature` / `NVMe_GetFeature`
admin helpers.  Other mode pages get a zero-filled response of the
right shape, or CHECK CONDITION on SENSE; other pages on SELECT are
silently accepted.

State tracked on `NVMeController.vwc_enabled`, seeded from
Identify byte 525 (VWC bit 0) at init, updated on every successful
Set Features call.

#### Plumbing added for the four handlers

- `NVMeIO_SubmitAndWait` in `src/nvme/nvme_io.c` — the synchronous
  I/O primitive the SCSI handlers use.  Reserves an inflight slot,
  builds the SQE, advances the shadow tail + rings the doorbell,
  then poll-harvests with `Forbid`/`Permit` yields under a 5-second
  budget.  The slot is flagged `suppress_reply` so the normal
  Harvest path does bookkeeping (status translation, latency sample,
  slot release) but does NOT `ReplyMsg` the IORequest — the caller
  owns the reply so it can translate the NVMe status into SCSI
  fields first.
- `NVMe_SetFeature` / `NVMe_GetFeature` in `src/nvme/nvme_admin.c`
  — generic wrappers around `NVMe_AdminCmd` for opcodes 0x09 and
  0x0A, used for VWC toggle today.
- `NVMeInflight::suppress_reply` — new per-slot flag; cleared on
  every slot-lifecycle entry to protect against stale true values
  leaking from a timeout into a later I/O.
- `NVMeUnit::dsm_buf` + `dsm_phys` — pre-pinned 4 KiB DMA buffer
  allocated alongside the bounce / PRP-list pages at
  `UnitTask_Start`, freed at shutdown, used only by the UNMAP
  handler.
- `NVMeController::onc_dsm`, `vwc_present`, `vwc_enabled` — parsed
  from Identify Controller bytes 520 and 525.  The on-wire struct
  stops short of those offsets, so the code reads raw bytes out of
  the identify buffer; single-byte reads are endian-neutral.

#### Benchmark impact

Host-side variance dominated the post-v1.66 AmigaDiskBench run
(`nvme_adb7.txt`): reads dropped ~23 % vs v1.65, but none of the
v1.66 changes affect the hot path in a way that would account for
that swing.  Struct growth is trivial (~12 bytes added across the
inflight array), and the extra `suppress_reply` check adds ~40 ns
per completion.  Cold QEMU host page cache at run start is the
most-likely explanation; the SequentialRead 4 KiB cell has moved
±30 % across three runs of nearly-identical code.

For features the benchmark does exercise, v1.66 is a clean add:
- AmigaDiskBench itself issues none of the new CDBs.
- The v1.65 HeavyLifter / Legacy wins are carried forward intact.
- blockdev.library consumers, SAT tools, and filesystems that drop
  TRIM hints now get real behaviour instead of CHECK CONDITION.

### Commit

Build size: 74 204 B release, 81 704 B debug.  Clean build, no
warnings.  Version bump 1.65 → 1.66.

## Session 10 — 2026-04-12 (late evening)

### v1.63 → v1.65 — second perf sweep + learnings from two dead-ends

After Session 9 shipped v1.62's three structural changes (alignment-aware
bounce selection, DMAEntry pool, doorbell batching), a cross-device
benchmark vs `virtioscsi.device` suggested nvme.device was still 1.3–6.7×
slower at 128 KiB+.  Architectural deep-dive of virtioscsi revealed that
most of its tricks were **already in our driver** after v1.62; the one
clear remaining gap was the CPU memcpy through the bounce.

Five candidate optimisations were designed and implemented as v1.63:

1. **`IExec->CopyMem` in the bounce path** — replaces compat.c's byte-by-
   byte memcpy (we build `-nostartfiles` so newlib's memcpy isn't linked).
   Exec's CopyMem is the canonical optimised block copy; fits the hot
   path (write Submit and read Harvest) in ~4 lines.

2. **`NVME_MAX_INFLIGHT` 16 → 32** — bigger pipeline depth for multi-
   client bursts.

3. **User-buffer DMA pin cache** — speculative one-slot cache keeping
   a `StartDMA` pin alive across I/Os so repeated use of the same user
   buffer wouldn't pay StartDMA/EndDMA every time.

4. **Hybrid-poll window** — 512-iter spin-Harvest after the SQ doorbell
   ring to catch fast completions inline before falling into the outer
   yield-poll.

5. **MDTS soft-cap 1 MiB → 2 MiB** — lifts the synthetic cap when MDTS=0,
   bounded by our single-PRP-list-page capacity (512 × 4 KiB = 2 MiB).

#### v1.63 crashed on first test

AmigaDiskBench's Sprinter-1-MiB test DSI-faulted inside
`CacheClearE`'s `dcbi` loop — DAR pointed at an unmapped user page.
Root cause: **change 3 (pin cache) is fundamentally unsafe on AmigaOS
4**.  `StartDMA`/`EndDMA` are *cache-maintenance* operations, not
page-pinning — the OS exposes no signal when the caller frees their
buffer.  ADB moved from the 256 KiB test to the 1 MiB test, freed the
256 KiB buffer, and our next `dma_cache_evict → EndDMA(freed_pages)`
ran `dcbi` across unmapped memory.

Lesson, saved as a memory:  any design that tries to hold a `StartDMA`
pin across I/O boundaries is a non-starter on this OS.  The DMAEntry
*pool* works (it pre-allocates `struct DMAEntry` arrays, not buffer
pins); the pin *cache* doesn't.

v1.64 shipped with change 3 reverted but 1, 2, 4, 5 kept.

#### v1.64 passed correctness but read performance regressed badly

Re-run of the benchmark showed SequentialRead −13 % and Random4KRead
−11 % vs v1.62, wiping out the read wins we'd already banked.  Net
across all suites was slightly worse than baseline despite clear wins
on HeavyLifter (+8 %).

Root cause: **change 4 (hybrid-poll) is actively harmful under QEMU TCG**.
QEMU runs guest CPU and device emulation on a single cooperative
thread.  The spin-Harvest after the doorbell ring monopolises guest
CPU exactly during the window when QEMU needs it to progress the NVMe
DMA.  `Forbid()`/`Permit()` in the outer yield-poll loop gives QEMU
that CPU; hybrid-poll delays it by up to 512 iterations.  The worst
regressions were on read paths where completion latency dominates and
QEMU CPU starvation is most painful.

Secondary observation: **change 2 (NVME_MAX_INFLIGHT 32) did nothing**
for single-client AmigaDiskBench workloads.  The event loop never had
more than a handful of messages queued at once.  Net cost was
doubling the per-unit bounce + pool memory footprint for no
observable benefit.

Both change 2 and change 4 were reverted.  v1.65 kept only change 1
(CopyMem) and change 5 (MDTS cap).

#### v1.65 benchmark result — six of seven suites up +7 %–+12 % vs v1.61

| suite          | v1.61 → v1.65 total | Δ |
|---|---:|---:|
| HeavyLifter    | 442 → 480  | **+8.6 %**  |
| Legacy         | 428 → 473  | **+10.5 %** |
| Sequential     | 808 → 792  | −2.1 %      |
| Random4K       | 1171 → 1299 | **+11.0 %** |
| SequentialRead | 1377 → 1482 | **+7.6 %**  |
| Random4KRead   | 1106 → 1243 | **+12.3 %** |
| MixedRW70/30   | 1164 → 1277 | **+9.7 %**  |

Best single-cell wins (vs v1.61):

- Random4K 64 K: **85 → 117 MB/s (+37 %)**
- Random4KRead 64 K: 96 → 120 MB/s (+25 %)
- SequentialRead 64 K: 128 → 159 MB/s (+24 %)
- Legacy 1 M: 104 → 114 MB/s (+9 %)

The one remaining regression is Sequential writes at 16–64 KiB —
aligned multi-page transfers on that suite take the direct-DMA path,
which pays a write-side cache flush (`dcbf + sync`) in `StartDMA` that
turns out to be expensive on fresh CPU-dirtied lines.  HeavyLifter
and Legacy tolerate this fine (warm-buffer pattern); Sequential
doesn't.  A write-side asymmetric heuristic could recover it but
hasn't been implemented — the −2 % suite-total cost is acceptable
against +9 % gains everywhere else.

### Final v1.65 changeset on top of v1.61

Kept from v1.62 (Session 7's performance sweep):

- Alignment-aware bounce selection (`should_use_bounce()` in `nvme_io.c`).
- Pre-allocated DMAEntry pool per inflight slot.
- `NVMeIO_SubmitNoRing` + `NVMeIO_RingSQ` split with event-loop doorbell
  batching.

New in v1.65:

- `IExec->CopyMem` on the bounce hot path, replacing `compat.c memcpy`.
- MDTS soft-cap raised 1 MiB → 2 MiB (matches single-PRP-list-page
  capacity).

Version bump 1.61 → 1.65, build stamp refreshed.

## Session 9 — 2026-04-12 (evening)

### v1.62 — Performance: alignment-aware DMA path, DMAEntry pool, SQ doorbell batching

Driven by the cross-device AmigaDiskBench session in `nvme_adb2.txt`
(RAM Disk vs `nvme.device` vs `virtioscsi.device`).  At block sizes
≤ 64 KiB the two drivers were within 10 % of each other; at 128 KiB
and above `virtioscsi.device` pulled 1.3–6.7× ahead depending on the
test.  Root-cause analysis identified three architectural causes, all
addressed in this revision:

**1. Alignment-aware bounce selection.**  The previous decision was a
straight `byte_length ≤ NVME_BOUNCE_SIZE → bounce`, which forced every
transfer up to 64 KiB through a full-size memcpy even when the user
buffer was page-aligned and would have been trivially DMA-able.  The
new heuristic in `nvme_io.c:should_use_bounce` keeps bounce for small
transfers (< `NVME_DIRECT_MIN_PAGES × page_size`, nominally 2 pages)
and for unaligned buffers, but sends aligned medium transfers through
direct DMA — skipping the memcpy entirely.

**2. Pre-allocated DMAEntry pool.**  The direct-DMA path was calling
`AllocSysObjectTags(ASOT_DMAENTRY)` and `FreeSysObject` on every I/O.
Every unit now owns one DMAEntry array per inflight slot, sized for
the controller's worst-case fragmentation
(`max_transfer_bytes / page_size + 1` entries).  `NVMeIO_Submit`
reuses these in place of per-I/O allocation; a per-I/O fallback is
retained for the vanishingly-rare case where an I/O's StartDMA
reports more fragments than the pool can hold.  Pool lifetime is tied
to `UnitTask_Start`/`UnitTask_Shutdown`, symmetrical with the bounce
buffers and PRP-list pages.

**3. SQ doorbell batching.**  `NVMeIO_Submit` was split into
`NVMeIO_SubmitNoRing` (reserve slot + build SQE + advance shadow tail)
and `NVMeIO_RingSQ` (publish the tail to the device).  The unit task's
event loop snapshots the SQ tail before draining the message port,
dispatches every queued message via the no-ring primitive, and then
rings exactly once at the end.  For a burst of N user I/Os the device
now sees one doorbell write instead of N.  Single-request workloads
(most of the AmigaDiskBench suites) are unchanged; multi-client
bursts see the saving.  `NVMeIO_Submit` is retained as a convenience
wrapper for paths that want immediate doorbell visibility (Flush, the
chunked path's per-chunk poll).

Additional hygiene in the same commit:

- `struct NVMeInflight` now snapshots the user-buffer pointer and
  length at Submit time.  The Harvest copy-back uses the snapshot
  rather than re-reading `req->io_Data` / `req->io_Length`, which
  makes the bounce path safe against callers that mutate their
  `IORequest` between Submit and Harvest — e.g. the MDTS chunked path.
- The direct-path "which flavour is this DMAEntry array" marker is
  encoded in the top bit of `inf->dma_flags`; the extraction in
  `NVMeIO_Harvest` masks it off before calling `EndDMA`.
- Version bump 1.61 → 1.62, build date stamp refreshed by the
  Makefile on every compile.

Validation plan for this revision: re-run the full AmigaDiskBench
suite against `nvme.device.debug`, compare NVME-column numbers
against the v1.61 reference in `nvme_adb2.txt`.  Expected wins at
8–64 KiB block sizes (memcpy now skipped on aligned transfers) and
at 128 KiB–1 MiB (DMAEntry pool eliminates AllocSysObject per I/O).

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
