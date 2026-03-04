# nvme.device — AmigaOS 4.1 NVMe Device Driver

Native AmigaOS 4.1 Final Edition device driver for QEMU's NVMe controller emulation.
Targets the **QEMU AmigaOne machine** (`-M amigaone`) first.

## Status

- [x] Project skeleton and build system
- [ ] PCI discovery (1B36:0010)
- [ ] Controller initialisation (CAP, CC, admin queues)
- [ ] Identify Controller / Namespace
- [ ] I/O queue creation
- [ ] Block read / write (polling path)
- [ ] mounter.library announce
- [ ] Interrupt handler (INTx)
- [ ] I/O pipeline (async inflight slots)
- [ ] 64-bit LBA support (TD_READ64/WRITE64)
- [ ] blockdev.library integration testing

## Hardware

- **Target**: QEMU AmigaOne (`-M amigaone`, Articia-S PCI bridge)
- **PCI device**: `1B36:0010` (Red Hat QEMU NVMe)
- **Register access**: `pciDev->InLong/OutLong()` (I/O port method — works on Articia-S)
- **DMA**: `StartDMA/GetDMAList` with `MEMF_SHARED` buffers

## Building

```sh
# From WSL2:
cd /mnt/w/Code/amiga/antigravity/projects/AmigaNVMeDevice
docker run --rm \
    -v /mnt/w/Code/amiga/antigravity:/src \
    -w /src/projects/AmigaNVMeDevice \
    walkero/amigagccondocker:os4-gcc11 \
    make -j$(nproc) clean all
```

Output: `build/nvme.device`

## QEMU Command Line

```sh
qemu-system-ppc -M amigaone \
    -kernel bboot \
    -device loader,addr=0x600000,file=kickstart.zip \
    -device nvme,drive=nvme0,serial=amiga-nvme-0 \
    -drive file=amigaos_nvme.img,if=none,id=nvme0,format=raw \
    -m 512 \
    -serial stdio
```

See `docs/nvme_boot_strategy.md` for full boot setup instructions.

## Architecture

### Device interface

Standard AmigaOS trackdisk-compatible device. Implements:
- `CMD_READ`, `CMD_WRITE`, `CMD_UPDATE` (NVMe Flush)
- `TD_GETGEOMETRY`, `TD_READ64`, `TD_WRITE64`
- `NSCMD_TD_READ64`, `NSCMD_TD_WRITE64`, `NSCMD_DEVICEQUERY`

### NVMe protocol

- Admin queue pair (shared) for controller init and namespace management
- Per-namespace I/O queue pair (one per AmigaOS unit)
- PRP-based data transfer (physical region pages)
- Polling CQ phase bit for completion detection
- INTx interrupt support (polling fallback)

### Two-phase I/O

- **Boot time** (`RTF_COLDSTART`): Full NVMe I/O available immediately
- **Post-Workbench**: Use `blockdev.library` for richer async I/O, scatter-gather

### Source layout

```
src/
  device.c          — Resident struct, interface vectors
  Init.c            — PCI, admin queues, controller enable, identify
  Open.c / Close.c  — Unit task lifecycle
  Expunge.c         — Controller shutdown, resource cleanup
  BeginIO.c         — Command dispatch (inline + async queue)
  unit_discovery.c  — Namespace enumeration, mounter.library announce
  unit_task.c       — Per-unit async I/O task, inflight pipeline
  nvme/
    nvme_init.c     — Controller init sequence
    nvme_admin.c    — Admin commands (identify, create queues)
    nvme_io.c       — I/O submit/harvest
    nvme_irq.c      — Interrupt handler
  pci/
    pci_discovery.c — PCI scan (1B36:0010), BAR0 setup
  exec_cmds/
    cmd_*.c         — Per-command helpers
include/
  nvme.h            — NVMe register defs, SQE/CQE structs, opcodes
  nvme_device.h     — NVMeBase, NVMeUnit structs
  nvme_cmds.h       — Supported command documentation
tests/
  test_nvme.c       — Basic R/W/geometry test
```

## References

- NVMe Base Specification: https://nvmexpress.org/specifications/
- OSDev NVMe wiki: https://wiki.osdev.org/NVMe
- QEMU NVMe emulation: https://qemu-project.gitlab.io/qemu/system/devices/nvme.html
- QEMU PCI IDs: https://www.qemu.org/docs/master/specs/pci-ids.html
- AmigaOS SDK: `W:/Code/amiga/antigravity/docs/sdk/`
- virtioscsi.device (reference driver): `../VirtualSCSIDevice/`
