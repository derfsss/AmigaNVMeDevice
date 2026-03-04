# NVMe Boot Strategy for AmigaOS 4.1 on QEMU AmigaOne

## Overview

QEMU supports attaching NVMe drives (`-device nvme`) but the AmigaOne's
bboot firmware has no disk drivers and cannot boot directly from NVMe.
This document describes the workaround using `nvme.device` as a Kickstart
module, a future U-Boot path, and the QEMU command line to use.

---

## How AmigaOS Boots on QEMU AmigaOne

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
cd /mnt/w/Code/amiga/antigravity/projects/AmigaNVMeDevice
docker run --rm -v /mnt/w/Code/amiga/antigravity:/src -w /src/projects/AmigaNVMeDevice \
    walkero/amigagccondocker:os4-gcc11 \
    make -j$(nproc) clean all
```

Output: `build/nvme.device`

### Step 2 — Add nvme.device to kickstart.zip

Unpack your existing `kickstart.zip`, add `nvme.device` to the `Kickstart/`
folder, and repack:

```sh
# Unpack
mkdir kickstart_tmp
cd kickstart_tmp
unzip ../kickstart.zip

# Add driver (copy into Kickstart folder alongside other modules)
cp /mnt/w/Code/amiga/antigravity/projects/AmigaNVMeDevice/build/nvme.device Kickstart/

# Edit Kickstart/kicklayout if needed to reference nvme.device
# (if kicklayout lists modules explicitly; otherwise all files in Kickstart/ are loaded)

# Repack
zip -r ../kickstart_new.zip .
cd ..
```

### Step 3 — Create NVMe disk image and install AmigaOS

```sh
# Create a blank NVMe disk image (e.g. 10 GB)
qemu-img create -f raw amigaos_nvme.img 10G
```

Boot AmigaOS with both a temporary install source (e.g. existing VirtIO SCSI)
and the blank NVMe image attached. Use AmigaOS Installer to install onto the
NVMe drive (it will appear as `DH0:` or `NVMe0:` once nvme.device is loaded).

### Step 4 — QEMU command line for NVMe boot

```sh
qemu-system-ppc -M amigaone \
    -kernel bboot \
    -device loader,addr=0x600000,file=kickstart_new.zip \
    -device nvme,drive=nvme0,serial=amiga-nvme-0 \
    -drive file=amigaos_nvme.img,if=none,id=nvme0,format=raw \
    -m 512 \
    -serial stdio
```

Notes:
- No separate small boot drive is needed — kickstart.zip is loaded from RAM
- The NVMe drive holds SYS: (the full AmigaOS installation)
- `serial=amiga-nvme-0` sets the NVMe controller serial number (visible in Identify Controller)
- Multiple NVMe drives can be attached: add another `-device nvme,...` + `-drive ...`

### Multiple NVMe namespaces

QEMU's nvme device supports multiple namespaces per controller:

```sh
-device nvme,drive=nvme0,serial=amiga-nvme-0,num_queues=8 \
-drive file=disk0.img,if=none,id=nvme0,format=raw
```

To add multiple NVMe controllers:

```sh
-device nvme,drive=nvme0,serial=nvme-0 \
-drive file=disk0.img,if=none,id=nvme0,format=raw \
-device nvme,drive=nvme1,serial=nvme-1 \
-drive file=disk1.img,if=none,id=nvme1,format=raw
```

Each controller gets one AmigaOS unit (unit 0). Multiple controllers are
supported as multiple devices in the AmigaOS device list.

---

## How This Mirrors the Pegasos2 FFS→SFS Pattern

On real Pegasos2 hardware, SmartFirmware boots from a small FFS partition
that contains only the firmware loader. The loader then mounts a larger SFS
partition that has the full AmigaOS installation.

The QEMU AmigaOne NVMe setup is analogous:
- **Small boot medium**: bboot + kickstart.zip loaded directly by QEMU (in RAM)
- **Large OS drive**: NVMe image containing SYS:

No physical small boot disk is needed in the QEMU case because bboot receives
kickstart.zip as a QEMU `-device loader` payload, not from disk.

---

## Future: True NVMe Boot via U-Boot

For booting a real AmigaOne from NVMe (or simulating this in QEMU), a
firmware update is needed.

### What U-Boot NVMe support requires

U-Boot has NVMe support via `CONFIG_NVME`, `CONFIG_NVME_PCI`, `CONFIG_CMD_NVME`
(see `drivers/nvme/` in U-Boot source). Commands: `nvme scan`, `nvme info`,
`nvme read addr blk cnt`.

### QEMU AmigaOne machine and U-Boot

The QEMU AmigaOne machine (`hw/ppc/amigaone.c`) supports an optional U-Boot ROM:

```sh
qemu-system-ppc -M amigaone -bios u-boot-amigaone.bin ...
```

To add NVMe boot support to this path:
1. Build U-Boot for PowerPC with NVMe enabled
2. Add an NVMe boot command to the U-Boot boot script
3. U-Boot loads bboot and kickstart.zip from the NVMe drive
4. bboot starts Kickstart (which includes nvme.device)

This would allow booting a real AmigaOne from NVMe without the `-kernel bboot`
workaround, but requires building and testing a custom U-Boot binary.

**Status: documented, not implemented in this project.**

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
- Add explicit `MountList` entry for the NVMe unit if auto-mount fails
- Use `Info` command from AmigaOS shell to list mounted drives
