# nvme.device — AmigaOS 4.1 NVMe Device Driver

Native AmigaOS 4.1 Final Edition block-device driver for NVMe controllers
on PCIe.  A single binary runs on every AmigaOS 4.1 FE platform with a
working PCIe bridge.

**Current release: v1.61 (2026-04-12)** — validated end-to-end on QEMU
Pegasos2: boots to DOS, partitioned via Media Toolbox, formatted with
SFS/00, benchmarked with AmigaDiskBench (sustained 265–279 MB/s),
SMART reported via the `HD_SCSICMD` ATA PASS-THROUGH path.

## Status

All 16 commits of the modernization plan (`docs/modernization_plan.md`)
are implemented, plus post-modernization bug-fixes through v1.61.

## Platforms

One binary, runtime-detected:

| Platform        | Bridge              | Status |
|-----------------|---------------------|--------|
| QEMU Pegasos2   | MV64361             | **Tested end-to-end** |
| Pegasos II      | MV64361             | Should work — same bridge as QEMU |
| Sam440ep        | AMCC 440EP          | Expected to work |
| Sam460ex        | AMCC 460EX          | Expected to work |
| X1000           | PA6T Nemo           | Expected to work |
| X5000           | QorIQ P5020 / P5040 | Expected to work |
| A1222 "Tabor"   | QorIQ P1022         | Expected to work |
| AmigaOne 500    | -                   | Expected to work |
| AmigaOne XE/SE  | Mai Logic Articia S | **Unsupported** — bridge does not forward MMIO; driver aborts cleanly |

## Features

- **Multi-controller**: up to 4 NVMe devices × 8 namespaces each (32 units total)
- **Async I/O pipeline**: NVME_MAX_INFLIGHT=16 slots per unit, per-unit I/O task
- **Shared INTx handling** with per-controller ISR + yield-poll fallback
- **64 KiB pre-pinned bounce buffer** per inflight slot, full PRP1/PRP2/PRP-list support
- **MDTS chunking** for transfers > controller max
- **TD_READ64/WRITE64** and every NSD 64-bit command
- **HD_SCSICMD synthesis**: INQUIRY, READ CAPACITY 10, and **ATA PASS-THROUGH SMART** — AmigaDiskBench's SMART tab displays live NVMe telemetry (temperature, power-on-hours, wear%, spare%, etc.) translated into ATA attribute format
- **LOG SENSE** page 0x00 / 0x2F for SPC-4 health-reporting tools
- **Live statistics** via `NSCMD_NVME_GETSTATS (0xA100)` — byte counts, latencies, per-path hits, SMART.  CLI monitor `nvme_stats` bundled
- **Debug build coexists with release** — `nvme.device.debug` is swap-in with zero behaviour change except verbose serial logging
- **Resident priority 0** (boot-drive compatible, matches `virtioscsi.device`)
- **MMU cache-inhibit + guarded** on BAR0 for correct MMIO on real hardware
- **Startup banner** is the only always-on serial output; release build emits nothing else during normal operation

## Building

Via Docker from WSL2 or any Linux host:

```sh
docker run --rm -v /mnt/w/Code/amiga/antigravity:/src \
    -w /src/projects/AmigaNVMeDevice \
    walkero/amigagccondocker:os4-gcc11 \
    sh -c "make clean && make -j$(nproc) all"
```

Targets:

| make target | produces |
|---|---|
| `make` / `make all` | release + debug drivers, `test_nvme`, `nvme_stats` |
| `make release` | release only (`build/nvme.device`) |
| `make debug` | debug only (`build/nvme.device.debug`) |
| `make deploy` | copy release to `$(DEPLOY_DIR)` (default `/mnt/s/temp`) |
| `make deploy-debug` | deploy debug binary (lands as `nvme.device`) |
| `make dist` | stage release+debug+readme+AutoInstall in `build/dist/nvme/` |
| `make dist-lha` | wrap the staging dir as `build/nvme.lha` (AmiUpdate-ready) |
| `make clean` | remove `build/` |

Optional compile flags:

- `make NO_SMART=1 …` — build without SMART log-page refresh.
- `DEPLOY_DIR=/other/path make deploy` — redirect deploy copies.

## Quick start on QEMU Pegasos2

```sh
qemu-system-ppc -M pegasos2 \
    -kernel bboot \
    -initrd kickstart.zip \
    -device nvme,drive=nvme0,serial=amiga-nvme-0 \
    -drive file=nvme_test.img,if=none,id=nvme0,format=raw \
    -device virtio-scsi-pci-non-transitional,id=scsi0 \
    -drive file=virtioscsi_test.img,if=none,id=vd0,format=raw \
    -device scsi-hd,drive=vd0,bus=scsi0.0,channel=0,scsi-id=0,lun=0 \
    -m 2048M \
    -serial stdio
```

`diskboot.config` inside `kickstart.zip/Kickstart/` must contain
`nvme.device 1 1` or diskboot.kmod won't scan the controller.  See
`docs/nvme_boot_strategy.md` for the full boot procedure.

## Runtime tools

**`nvme_stats`** — CLI snapshot of the live stats interface:

```
nvme_stats           # one-shot snapshot of unit 0
nvme_stats 1         # snapshot of unit 1
nvme_stats -w 0 2    # watch unit 0, refresh every 2s
nvme_stats -s        # one-line summary of every unit
```

**`test_nvme`** — 10-step functional tester (INQUIRY, geometry, read,
write-verify, 64 KiB round-trip, TD_READ64 high-offset, etc.).

**AmigaDiskBench** — works unmodified; SMART tab shows real NVMe data
thanks to the ATA PASS-THROUGH translation path.

## SMART surface

The driver publishes SMART data through two mechanisms:

1. **Custom, native, zero-loss**: `NSCMD_NVME_GETSTATS (0xA100)` returns
   a `struct NVMeStats` with all NVMe health fields plus live
   driver counters.  Used by `nvme_stats`.
2. **SCSI-ATA translation**: `HD_SCSICMD` with CDB opcode `0x85` or `0xA1`
   (ATA PASS-THROUGH 16 / 12), ATA command `0xB0` (SMART), sub-command
   `0xD0` (READ DATA) or `0xD1` (READ THRESHOLDS) returns a 512-byte ATA
   SMART block synthesized live from NVMe Log Page 0x02.  Used by
   AmigaDiskBench and any SMART tool that speaks SAT.  LOG SENSE (0x4D)
   pages 0x00 and 0x2F also work.

The in-driver SMART cache refreshes on demand every 30 seconds.

## Distribution

`make dist-lha` produces `build/nvme.lha`, an AmiUpdate-ready archive:

```
nvme/
├── AutoInstall               (generated from amiupdate.yml)
├── nvme.device               (release build)
├── nvme.device.debug         (verbose debug build — optional rename-to-install)
├── test_nvme                 (10-step functional test)
├── nvme_stats                (CLI monitor)
├── nvme.readme               (plain-text end-user docs)
└── diskboot.config.sample    (example Kickstart: config)
nvme.readme                   (duplicate at archive root for os4depot)
```

## Source layout

```
src/
  device.c              Resident struct, manager interface vectors
  Init.c                Per-controller init loop, banner, IRQ install,
                        unit discovery
  Open.c / Close.c      Unit task lifecycle (lazy start on first open)
  Expunge.c             Full reverse-order release + leak report
  BeginIO.c             Command dispatch (inline, held, async, stats)
  unit_discovery.c      Per-controller namespace enumeration + mounter
  unit_task.c           Per-unit async I/O task, SCSI dispatch, chunking
  nvme/
    nvme_init.c         Controller reset/enable/identify
    nvme_admin.c        Admin command wrappers + SMART refresh
    nvme_io.c           I/O submit/harvest + PRP build
    nvme_irq.c          Per-controller INTx ISR + install/remove
  pci/
    pci_discovery.c     FDT_Index loop over NVMe controllers
    platform_detect.c   Host bridge table + BAR0 MMIO probe
  scsi_cmds/
    scsi_ata_passthrough.c  CDB 0x85/0xA1 → NVMe SMART translation
    scsi_log_sense.c        CDB 0x4D (pages 0x00 / 0x2F)
  nvme_mmu.c            BAR0 cache-inhibit + guarded setup
  nvme_status.c         Unified NVMe-status → io_Error mapper
  nvme_stats.c          NSCMD_NVME_GETSTATS handler + snapshot builder
  nvme_leak.c           Debug-only alloc/free counters + Expunge report
  compat.c              memset/memcpy for -nostartfiles
include/
  nvme.h                Register offsets (NVMe 1.4 citations), SQE/CQE
  nvme_device.h         NVMeBase, NVMeController, NVMeUnit, MMIO helpers
  nvme_scsi.h           scsi_cmds/ prototypes
  nvme_admin.h          Admin command prototypes
  nvme_init.h           Controller init / cleanup
  nvme_io.h             Submit / flush / harvest
  nvme_irq.h            IRQ install / remove
  nvme_stats.h          NVMeStats wire struct + TBR helpers
  nvme_status.h         Status mapper prototype
  nvme_mmu.h            MMU helper prototype
  nvme_leak.h           Debug leak counters
  nvme_platform.h       Platform enum + MMIO probe prototype
  nvme_debug.h          DPRINTF macros
  version.h             DEVNAME / version / build stamp
tests/
  test_nvme.c           10-step functional test program
  nvme_stats.c          CLI monitor for the stats interface
docs/
  modernization_plan.md The 16-commit plan (authoritative)
  architecture.md       Architecture + protocol reference
  nvme_boot_strategy.md QEMU / kickstart.zip boot setup
  history.md            Session-by-session changelog
amiupdate.yml           AmiUpdate installer configuration
nvme.readme             Plain-text readme bundled in the LHA
diskboot.config.sample  Example Kickstart: diskboot.config entry
```

## References

- NVMe Base Specification 1.4: https://nvmexpress.org/specifications/
- AmigaOS SDK: `W:/Code/amiga/antigravity/docs/sdk/`
- Reference drivers:
  - `../VirtualSCSIDevice/` — virtioscsi.device, same block-driver pattern
  - `../VirtIOGPU/` — MMIO / MMU / endian patterns
  - `../Init/` — AmigaOS 4 project conventions
- AmiUpdate integration: `../AmiUpdateIntegration/`
