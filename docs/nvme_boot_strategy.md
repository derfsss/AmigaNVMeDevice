# NVMe Boot Strategy for AmigaOS 4.1 on QEMU Pegasos2

## Overview

QEMU supports attaching NVMe drives (`-device nvme`) but the Pegasos2's
bboot firmware has no disk drivers and cannot boot directly from NVMe.
This document describes the workaround using `nvme.device` as a Kickstart
module and the QEMU command line to use.

---

## How AmigaOS Boots on QEMU Pegasos2

1. QEMU loads `bboot` as the kernel (`-kernel bboot`)
2. bboot loads Kickstart from a QEMU loader device: `-device loader,addr=0x600000,file=kickstart.zip`
3. bboot decompresses kickstart.zip into RAM and starts the PowerPC core
4. Kickstart scans resident modules, opens libraries, and starts mounter.library
5. mounter.library detects all device drivers and mounts filesystems

bboot never touches a disk — Kickstart is always delivered as an in-memory
initrd (the ZIP file). This means **any driver placed inside kickstart.zip
is available before filesystem mounting**, regardless of whether a disk
with the driver exists.

---

## Workaround: nvme.device as a Kickstart Module

### Step 1 — Build nvme.device

```sh
# From WSL2 (two separate docker calls):
wsl sh -c "docker run --rm -v /mnt/w/Code/amiga/antigravity:/src -w /src/projects/AmigaNVMeDevice walkero/amigagccondocker:os4-gcc11 make clean"
wsl sh -c "docker run --rm -v /mnt/w/Code/amiga/antigravity:/src -w /src/projects/AmigaNVMeDevice walkero/amigagccondocker:os4-gcc11 make -j\$(nproc) all"
```

Output: `build/nvme.device`

### Step 2 — Add nvme.device to kickstart.zip

Use the PowerShell update script to replace the driver in the Pegasos2 kickstart.zip:

```powershell
# From Windows (after copying build/nvme.device to S:\temp\):
powershell -File S:\temp\update_peg2_zip.ps1
```

Or manually: unpack kickstart.zip, add `nvme.device` to the `Kickstart/`
folder, add an entry to `Kickstart/kicklayout`, and repack.

### Step 3 — Create NVMe disk image

```sh
# Create a blank NVMe disk image (e.g. 256 MB for testing)
qemu-img create -f raw nvme_test.img 256M
```

### Step 4 — Create diskboot.config

`diskboot.kmod` must be told about nvme.device. Create a file `diskboot.config`
with the following entry appended:

```
nvme.device 1 1
```

Format: `devicename maxunits flags` (1=HD, 2=CD, 3=both).

Place this file where AmigaOS can find it at boot (e.g. `Devs/diskboot.config`
on the SYS: volume, or in an accessible VVFAT share for testing).

### Step 5 — QEMU command line for NVMe boot

**Critical**: A VirtIO SCSI device MUST be present in the QEMU config, even if
it has no drives attached. `diskboot.kmod` only activates its partition scan
pipeline when it detects a VirtIO SCSI controller. Without one, nvme.device is
ignored even if listed in diskboot.config.

```sh
qemu-system-ppc -M pegasos2 \
    -kernel bboot \
    -device loader,addr=0x600000,file=kickstart.zip \
    -device virtio-scsi-pci,id=scsi0 \
    -device nvme,drive=nvme0,serial=amiga-nvme-0 \
    -drive file=nvme_test.img,if=none,id=nvme0,format=raw \
    -m 512 \
    -serial stdio
```

Notes:
- The `-device virtio-scsi-pci` is required even with no SCSI drives — it triggers diskboot
- No separate small boot drive is needed — kickstart.zip is loaded from RAM
- The NVMe drive holds SYS: (the full AmigaOS installation)
- `serial=amiga-nvme-0` sets the NVMe controller serial number (visible in Identify Controller)
- Multiple NVMe drives can be attached: add another `-device nvme,...` + `-drive ...`
- For testing, add a VVFAT share with diskboot.config:
  `-drive file=fat:rw:S:/temp/,format=vvfat,if=none,id=vvfat -device virtio-blk-pci,drive=vvfat`

### Multiple NVMe controllers

```sh
-device nvme,drive=nvme0,serial=nvme-0 \
-drive file=disk0.img,if=none,id=nvme0,format=raw \
-device nvme,drive=nvme1,serial=nvme-1 \
-drive file=disk1.img,if=none,id=nvme1,format=raw
```

Each controller gets one AmigaOS unit (unit 0). Multiple controllers are
supported as multiple devices in the AmigaOS device list.

---

## Platform Notes

### Pegasos2 (MV64361) — supported

The Pegasos2's MV64361 PCI bridge transparently forwards CPU memory cycles
to PCI devices. NVMe BAR0 is memory-mapped at addresses like `0x84200000`.
Register access uses `lwbrx`/`stwbrx` PPC inline asm for LE↔BE byte swap.

### AmigaOne (Articia-S) — NOT supported

The AmigaOne's Articia-S PCI bridge does not forward MMIO cycles to PCI
devices. All MMIO access methods (volatile pointer, InByte/InLong, lwbrx/stwbrx,
IMMU->RemapMemory) have been tested and confirmed to fail. NVMe requires MMIO
for register access, so the AmigaOne cannot support NVMe.

---

## Troubleshooting

**nvme.device not found at boot:**
- Verify `nvme.device` is in the `Kickstart/` folder inside kickstart.zip
- Check `kicklayout` file — it may list modules explicitly
- Enable serial debug output (`-serial stdio`) to see DPRINTF messages

**NVMe controller not found (1B36:0010):**
- Verify QEMU command line includes `-device nvme,...`
- Confirm QEMU version supports nvme device emulation

**Filesystem not mounting:**
- Check that mounter.library announce is implemented in unit_discovery.c
- Ensure `diskboot.config` contains `nvme.device 1 1`
- Ensure a VirtIO SCSI device is present in QEMU config (diskboot.kmod requirement)
- Add explicit `MountList` entry for the NVMe unit if auto-mount fails
- Use `Info` command from AmigaOS shell to list mounted drives

**NVMe drive appears but shows wrong size:**
- Check that `NSCMD_TD_GETGEOMETRY64` fills `struct DriveGeometry64` (from
  `<libraries/mounter.h>`), NOT `struct DriveGeometry` (from `<devices/trackdisk.h>`)
- These are completely different structs with different field layouts
