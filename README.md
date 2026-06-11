# nvme.device

A native AmigaOS 4.1 Final Edition block-device driver for NVMe SSDs on PCIe — partition, format, mount, and boot from NVMe namespaces.

**Status:** Beta (v1.68) — validated end-to-end under QEMU (Pegasos2, SAM460ex, AmigaOne, including boot-from-NVMe); real-hardware confirmation pending on the X1000 and X5000, the only models with a free PCIe slot for an NVMe drive.

> ⚠️ **Beta — actively under development.** Expect bugs and rough
> edges; do not rely on it for anything important. Use at your own
> risk.

---

## Overview

`nvme.device` exposes each NVMe namespace as a standard
trackdisk-compatible AmigaOS unit. One binary supports every
AmigaOS 4.1 FE platform whose PCI/PCIe bridge forwards CPU memory
cycles.

NVMe controllers are discovered by PCI class code (`0x010802`), so any
vendor's drive is found without a device-ID table. The driver reads
the controller's capabilities (`CAP.MPSMIN`, `CAP.MQES`, `CAP.TO`,
MDTS) and adapts its page size, queue depths, and timeouts to the
hardware. If no NVMe device is present, the driver declines to load
cleanly and the system boots normally.

### Supported platforms

| Platform | Bridge | Status |
|---|---|---|
| QEMU Pegasos2 | Marvell MV64361 | Tested end-to-end, boots from NVMe |
| QEMU SAM460ex | AMCC 460EX | Tested end-to-end |
| QEMU AmigaOne | Mai Logic Articia S (emulated) | Tested end-to-end (driver repairs the OS's half-programmed 64-bit BAR) |
| AmigaOne X1000 | PA6T "Nemo" | Expected to work — has a free PCIe slot for an NVMe adapter card; awaiting confirmation |
| AmigaOne X5000 | QorIQ P5020/P5040 | Expected to work — has a free PCIe slot for an NVMe adapter card; awaiting confirmation |
| Other real machines (Pegasos II, Sam440ep, Sam460ex, A1222, AmigaOne XE/SE) | various | **Not applicable** — no available PCIe slot for an NVMe drive (PCI/AGP only, or the PCIe lanes are already occupied), so the driver has nothing to find; it declines to load cleanly |

The X1000/X5000 use the same runtime-detected code paths validated
under QEMU; they await confirmation on real hardware (an NVMe SSD in
a PCIe M.2 adapter card).

---

## Features

- **Boots AmigaOS from NVMe** — resident Kickstart module, priority 0,
  recognised by `diskboot.kmod` as a boot-drive candidate
- **Multi-controller, multi-namespace** — up to 4 controllers ×
  8 namespaces (32 units), flat unit numbering
- **512-byte and 4K-native namespaces** — logical block size taken from
  Identify Namespace, honoured throughout the I/O path
- **Asynchronous I/O pipeline** — 16 in-flight commands per unit, served
  by a per-unit task; doorbell writes batched across bursts
- **Adaptive DMA strategy** — small or unaligned transfers go through
  pre-pinned bounce buffers; aligned medium/large transfers use direct
  DMA on the caller's buffer with a pre-allocated scatter-gather pool
- **Full PRP support** — PRP1 / PRP2 / PRP-list construction up to 2 MiB
  per command, with transparent chunking above the controller's MDTS
- **TRIM** — SCSI UNMAP translated to NVMe Dataset Management
  (Deallocate), the first native NVMe TRIM for AmigaOS 4.1
- **64-bit addressing** — `TD_READ64`/`TD_WRITE64` and the complete NSD
  64-bit command set; namespaces beyond 4 GiB fully usable
- **HD_SCSICMD synthesis** — INQUIRY (with the drive's real model,
  serial, and firmware from Identify), READ CAPACITY(10/16),
  SYNCHRONIZE CACHE(10), MODE SENSE/SELECT page 0x08 (write-cache
  toggle via NVMe Set Features), LOG SENSE, and ATA PASS-THROUGH SMART
- **Live SMART telemetry** — NVMe health log translated to ATA SMART
  attributes on demand, so existing SMART-aware tools (e.g.
  AmigaDiskBench's SMART tab) work unmodified
- **Statistics interface** — `NSCMD_NVME_GETSTATS` returns per-unit
  command counts, byte totals, latency, path hits, and SMART fields;
  the bundled `nvme_stats` CLI displays them
- **Interrupt-driven with polling fallback** — per-controller INTx ISR
  that behaves correctly on shared interrupt lines; systems without a
  usable interrupt run in polling mode automatically
- **Quiet in production** — the release build prints a one-line startup
  banner and nothing else; a verbose debug build (`nvme.device.debug`)
  is a drop-in swap

---

## Requirements

- AmigaOS 4.1 Final Edition
- An NVMe controller reachable over PCI/PCIe (class code `0x010802`)
- For booting from NVMe: a boot environment that loads Kickstart
  modules before disk access (see
  [docs/nvme_boot_strategy.md](docs/nvme_boot_strategy.md))

---

## QEMU Setup

QEMU's `pegasos2` machine plus its standard `nvme` device model is the
reference test environment:

```sh
qemu-img create -f raw nvme_test.img 5G

qemu-system-ppc -M pegasos2 \
    -kernel bboot \
    -initrd kickstart.zip \
    -device nvme,drive=nvme0,serial=amiga-nvme-0 \
    -drive file=nvme_test.img,if=none,id=nvme0,format=raw \
    -device virtio-scsi-pci,id=scsi0 \
    -m 2048M \
    -serial stdio
```

Notes:

- `kickstart.zip` must contain `nvme.device`, a Kicklayout entry, and a
  `diskboot.config` with an `nvme.device` line (see Installation).
- `diskboot.kmod` only activates its scan pipeline when it finds at
  least one controller it recognises, so keep a VirtIO SCSI controller
  on the command line even with no drives attached.
- Full details, including booting AmigaOS *from* the NVMe disk, are in
  [docs/nvme_boot_strategy.md](docs/nvme_boot_strategy.md).

---

## Installation

1. Copy `nvme.device` into `SYS:Kickstart/` (or into the `Kickstart/`
   drawer of your Kickstart image for QEMU setups).
2. Add a line to the Kicklayout in use:

   ```
   MODULE Kickstart/nvme.device
   ```

3. Add an entry to `Kickstart/diskboot.config` so `diskboot.kmod`
   scans the device for bootable partitions:

   ```
   nvme.device 1 1
   ```

   (`1 1` = one unit, mount as hard disk.  Raise the unit count for
   multi-namespace or multi-controller setups — see
   `diskboot.config.sample`.)

4. Reboot.  Namespaces appear as `nvme.device` units 0…n and can be
   partitioned with Media Toolbox as usual.

Alternatively, for non-boot use the driver can simply be placed in
`DEVS:` and is loaded on first `OpenDevice()`.

### Bundled tools

**`test_nvme [unit]`** — functional test suite: device query, geometry
(32- and 64-bit), INQUIRY, single-block and 64 KiB round-trips, a
64-bit high-offset read, a 6 MiB transfer that exercises the >MDTS
chunking path, and negative tests proving misaligned requests are
rejected.  Prints `PASS`/`FAIL` per step and a summary line.

**`nvme_stats`** — live statistics monitor:

```
nvme_stats           # one-shot snapshot of unit 0
nvme_stats 1         # snapshot of unit 1
nvme_stats -w 0 2    # watch unit 0, refresh every 2 s
nvme_stats -s        # one-line summary of every unit
```

---

## Building from Source

The driver builds with the AmigaOS 4 gcc 11 cross-toolchain.  The
simplest route is the publicly available Docker image
[`walkero/amigagccondocker`](https://hub.docker.com/r/walkero/amigagccondocker):

```sh
docker run --rm -v "$(pwd):/src" -w /src \
    walkero/amigagccondocker:os4-gcc11 \
    sh -c "make clean && make -j$(nproc) all"
```

| make target | produces |
|---|---|
| `make` / `make all` | release + debug drivers, `test_nvme`, `nvme_stats` |
| `make release` | release only (`build/nvme.device`) |
| `make debug` | debug only (`build/nvme.device.debug`) |
| `make deploy` | copy release binaries to `$(DEPLOY_DIR)` (default `./deploy`) |
| `make dist` | stage a release drawer in `build/dist/nvme/` |
| `make dist-lha` | pack the staging drawer as `build/nvme.lha` |
| `make clean` | remove `build/` |

Optional flags: `NO_SMART=1` drops the SMART log-page path;
`DEPLOY_DIR=…` redirects deploy copies.

---

## Project Structure

```
src/
  device.c              Resident struct, manager interface vectors
  Init.c                Library init: PCI discovery, per-controller
                        bring-up, unit discovery
  Open.c / Close.c      Unit open/close, lazy unit-task lifecycle
  Expunge.c             Reverse-order teardown + debug leak report
  BeginIO.c             Command dispatch (inline / held / async / stats)
  unit_discovery.c      Namespace enumeration + mounter announcement
  unit_task.c           Per-unit async I/O task, SCSI synthesis,
                        MDTS chunking
  nvme/
    nvme_init.c         Controller reset / enable / identify
    nvme_admin.c        Admin commands + SMART refresh
    nvme_io.c           I/O submit / harvest, PRP construction
    nvme_irq.c          Per-controller INTx ISR
  pci/
    pci_discovery.c     Class-code controller enumeration, BAR0 setup
    platform_detect.c   Host-bridge identification + MMIO probe
  scsi_cmds/            HD_SCSICMD handlers (SMART, LOG SENSE, UNMAP,
                        MODE SENSE/SELECT)
  nvme_mmu.c            BAR0 cache-inhibit + guarded MMU attributes
  nvme_status.c         NVMe status → io_Error mapping
  nvme_stats.c          Statistics snapshot assembly
  nvme_leak.c           Debug-only resource counters
  compat.c              Freestanding memset/memcpy (-nostartfiles)
include/                Public + internal headers (nvme.h carries the
                        NVMe 1.4 register/section citations)
tests/
  test_nvme.c           Functional test program
  nvme_stats.c          Statistics CLI
docs/
  architecture.md       Architecture and protocol reference
  nvme_boot_strategy.md Booting AmigaOS with/from NVMe under QEMU
  history.md            Detailed development changelog
```

### Architecture notes

- Each unit is served by its own task; all queue state is single-task
  owned, so the I/O path needs no locks.  Admin commands serialise
  under a per-controller semaphore and are polled (admin interrupts
  stay masked to avoid IRQ storms on shared INTx lines).
- NVMe SQE/CQE structures are little-endian; the big-endian PPC host
  accesses them via `stwbrx`/`lwbrx` per-dword swaps.
- SMART data is cached in-driver and refreshed on demand, at most
  every 30 s, via admin Get Log Page 0x02.

---

## SMART

Two query paths are provided:

1. **Native** — `NSCMD_NVME_GETSTATS` (`0xA100`) returns a versioned
   `struct NVMeStats` (see `include/nvme_stats.h`) with the full NVMe
   health log plus live driver counters.
2. **SCSI-ATA translation** — `HD_SCSICMD` ATA PASS-THROUGH (CDB
   `0x85`/`0xA1`, ATA command `0xB0`, sub-commands `0xD0`/`0xD1`)
   returns a 512-byte ATA SMART block synthesized from NVMe Log Page
   `0x02`, covering temperature, power-on hours, power cycles, unsafe
   shutdowns, media errors, spare capacity, and wear.  LOG SENSE pages
   `0x00`/`0x2F` are also answered.

---

## Validation

Continuously tested under QEMU (TCG) with the standard `nvme` device
model:

- Full functional suite on Pegasos2, SAM460ex, and AmigaOne, including
  64-bit offsets, >MDTS chunked transfers, and alignment-rejection
  checks
- Multi-controller (2 controllers) and multi-namespace configurations,
  including a 4K-native namespace
- **Boot from NVMe**: a system disk image attached as the NVMe drive
  boots to Workbench with no other bootable media present
- No-hardware case on every machine: the driver aborts init cleanly
  and the system boots normally
- AmigaOne XE (Articia S): the driver detects the non-forwarding
  bridge and declines, leaving the system fully functional
- Benchmark-validated with AmigaDiskBench; SMART tab shows live NVMe
  telemetry

---

## Documentation

- [docs/history.md](docs/history.md) — full development changelog.
- [docs/architecture.md](docs/architecture.md) — architecture and
  protocol reference.
- [docs/nvme_boot_strategy.md](docs/nvme_boot_strategy.md) — booting
  AmigaOS with/from NVMe under QEMU.

Recent milestones:

- **1.68** — code-review hardening: held-request handling in
  `TD_ADDCHANGEINT`/`TD_REMOVE` (no more silent overwrite or orphaned
  requests), explicit 16-bit NLB ceiling guard on the submit path
- **1.67** — real-hardware readiness: PCI class-code discovery,
  CAP-derived limits (MPSMIN/MQES/TO), INQUIRY from Identify data,
  chunked-path fix, block-alignment validation
- **1.66** — TRIM (UNMAP→DSM), READ CAPACITY(16), SYNCHRONIZE CACHE,
  write-cache toggle via Mode Page 0x08
- **1.65** — bounce-path copy optimisation, 2 MiB MDTS cap
- **1.62** — alignment-aware DMA selection, DMAEntry pooling, doorbell
  batching

## References

- [NVM Express Base Specification 1.4](https://nvmexpress.org/specifications/)
- [AmigaOS 4.1 SDK](https://www.hyperion-entertainment.com/) (Hyperion Entertainment)
- [QEMU NVMe device documentation](https://www.qemu.org/docs/master/system/devices/nvme.html)
- [walkero/amigagccondocker](https://hub.docker.com/r/walkero/amigagccondocker) — AmigaOS 4 cross-compile Docker images

---

## Development

This project was created with help from [ClaudeAI](https://claude.ai)
(Anthropic) — writing the C code, designing the architecture, and
debugging hardware-level issues against the AmigaOS 4.1 SDK — with a
human developer directing, reviewing, and testing the result.

## License

Copyright © 2026. All rights reserved.
