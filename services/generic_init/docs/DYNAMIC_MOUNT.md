# Dynamic mount

How `generic_init` finds the Android partitions and mounts them, with no
fstab written in advance. It replaces AOSP's first stage mount.

Files: `dynamic_mount_handler.cpp` (find and decide) and
`dynamic_mount_helpers.cpp` (mount). For the parameters from a user's
point of view, see `device/mainline/generic/docs/boot-parameters.md`.

**TL;DR:** every block device that appears is probed once and
remembered. When everything required has been seen, ueventd quits, an
fstab is generated from what was found, and it is mounted.

## The three phases

| Phase | Function | When | What |
|-------|----------|------|------|
| Before | `OnPreBlockDevices()` | Before ueventd starts | Make the work directories, read the parameters and the addon fstabs |
| During | `OnBlockDeviceAdd()` | For each block device uevent | Probe the device, update the picture of what was found |
| Exit test | `CanQuitUeventd()` | After each uevent | Are all required pieces found? |
| After | `OnPostBlockDevices()` | After ueventd quit | Generate the fstab and mount |

Work directories, all under `/mnt/vendor/generic_init/`:

| Path | Use |
|------|-----|
| `try` | Temporary read-only mount used for probing |
| `android` | Mount of the device that holds the Android directory |
| `firmware` | Mount of the device that holds the firmware directory |
| `img` | RAM copies of images (`img_ram`, `imgs_ram`) |
| `overlay/<part>` | Mounts of overlay partitions and images |

## Parameters

Read with the same lookup as `.rc` expansion: kernel command line or
bootconfig. Unset means the default.

| Parameter | Default | Values |
|-----------|---------|--------|
| `androidboot.mount_system` | `std_parts` | `std_parts`, `blk_devices`, `imgs`, `imgs_ram` |
| `androidboot.mount_userdata` | `std_parts` | `std_parts`, `imgs`, `bind_mount_dir`, `tmpfs` |
| `androidboot.mount_overlay` | `disable` | `disable`, `std_parts`, `imgs`, `imgs_ram`, `dirs` |
| `androidboot.mount_firmware` | `all_possible_dirs` | `all_possible_dirs` (or `true`), `disable` (or `false`), `only_android_dir`, `img`, `img_ram` |
| `androidboot.android_dir` | none | Name of the Android directory; unset: `android` or `boot/android` |
| `androidboot.android_dir_dev` | none | The only device for the Android directory: `UUID=` (filesystem), `PARTUUID=` or `PARTLABEL=` (partition) |
| `androidboot.addon_fstab_suffix` | none | Comma separated suffixes |

An invalid value is logged and the default stays.

What each parameter makes the handler wait for:

| Setting | Needs to be found before ueventd may quit |
|---------|-------------------------------------------|
| `mount_system=std_parts` or `blk_devices` | A block device for every Android system partition (see below) |
| `mount_userdata=std_parts` | A block device for `userdata` and `metadata` (and `cache`, only if the system partition has a `/cache` directory) |
| `mount_firmware=all_possible_dirs` or `only_android_dir` | A block device with a firmware directory |
| Any image mode, `bind_mount_dir`, `mount_overlay=dirs` | A block device with the Android directory |
| `mount_overlay=std_parts` | Nothing more, but 5 more seconds unless every system partition found has an overlay partition. Overlay partitions are optional, so whether more will appear is unknown |
| `std_parts`: a system or userdata partition found only on other disks than the Android directory | 5 more seconds for one on the disk of the Android directory, then the other disk's is used. Not with `blk_devices`, where each disk may hold one partition |
| `imgs`, `tmpfs`, `disable` | Nothing for that part |

## Probing a block device

For every `add` uevent of a block device, except names starting with
`dm-`, `loop`, `ram` or `zram`:

1. Try APFS first: volumes 0 to 19 with `vol=N`. A volume counts only if
   it holds an Android directory.
2. Otherwise mount it read-only on `try`, testing in order: `ext4`, `f2fs`,
   `erofs`, `squashfs`, `vfat`, `exfat`, `ntfs`, `ntfs3`, `iso9660`, `udf`.
   Only `EINVAL` and `ENODEV` mean "try the next one". The driver must be
   in the ramdisk: exFAT and NTFS are what Windows tools format USB drives
   with when they copy the files of the live ISO.
3. Read the filesystem UUID, as blkid shows it: the UUID of `ext4`,
   `f2fs` and `erofs`, the volume ID of `vfat` and `exfat` (`XXXX-XXXX`),
   the serial number of `ntfs` and `ntfs3` (16 hex digits). For
   `androidboot.android_dir_dev=UUID=`.
4. Check what is inside (below).
5. Try a read-write remount, to know if it is writable.
6. Unmount.

Everything learned is stored per device and used by `UpdateMountInfo()`.

### An ISO booted as a file

Ventoy, or GRUB's `loopback`, boots an ISO file from a partition. Linux
sees the partition, not the ISO, so `iso_file.cpp` finds the file and
attaches it to a loop device, which is probed like any other device: the
ISO9660 filesystem of the live ISO.

- `androidboot.android_iso_uuid=<UUID>`: the UUID of the ISO, as GRUB and
  blkid show it (`YYYY-MM-DD-HH-mm-ss-hh`, from the modification date of
  its volume descriptor). The live ISO's GRUB passes it when its root is
  an ISO9660 filesystem. Every probed filesystem without an Android
  directory, but ISO9660 and UDF, is searched for `*.iso` files with that
  UUID, 3 directory levels deep and 20000 entries at most.
- Ventoy under UEFI says which file it booted, in the EFI variable
  `VentoyOsParam` (`ventoy_image.cpp`, efivarfs mounted for the time of
  reading it): its disk (the size, and the GUID Ventoy writes at offset
  `0x180` of its first sector), the partition and the path. That
  partition needs no search. Under BIOS, Ventoy leaves its parameters only
  in base memory, which `/dev/mem` reads as zeros.
- The partition stays mounted read-only on
  `/mnt/vendor/generic_init/iso`. Its driver must be in the ramdisk
  (exFAT, the default of Ventoy, and NTFS are).

Ventoy must boot the ISO as it is, in its "compatible" mode: otherwise it
changes the initrd for its own `/init`, which `rdinit=` does not run. The
live ISO asks for it with `VENTOY COMPATIBLE` in its publisher field
(`device/mainline/generic/build/tasks/20-liveisoimage.mk`).

### A partition table inside a partition

A disk image written to a partition (not to a disk) has partitions the
kernel does not see. A partition with no filesystem, but a GPT or MBR
(`partition_table.cpp`), is attached to a loop device with partition
scanning (`loop_device.cpp`), and the partitions it holds are probed like
any other, as devices of the same disk as the partition they are in.

- The GPT signature is looked for after the first 512 or 4096 bytes; that
  sector size is given to the loop device. An MBR needs the `0x55AA`
  signature and entries that fit in the device, and one that is not just
  the protection of a GPT.
- It is attached read-write, or read-only if it can not be opened so.
- Not for a whole disk (the kernel scans it), and not inside what was
  found this way: one level only.
- `PARTNAME` of a GPT inside is passed on by the kernel, so `std_parts`
  works with it.

### What is recognized

| Found | How |
|-------|-----|
| System partition | `/system/build.prop`, or `build.prop` or `etc/build.prop` with a `ro.<part>.build.id=` line (`blk_devices` mode) |
| Partitions inside | In a `system` device: `product`, `system_dlkm`, `system_ext`, `vendor`, and `vendor/{odm,odm_dlkm,vendor_dlkm}`. In a `vendor` device: `odm`, `odm_dlkm`, `vendor_dlkm`. Such partitions need no device of their own |
| `/cache` directory in `system` | Marks `cache` as needing a mount |
| Android directory | `androidboot.android_dir` only, if set. Otherwise `android`, then `boot/android` |
| Images | `*.img` files in the Android directory, typed by name (below) |
| Subdirectories | Directories in the Android directory, except `firmware` |
| Firmware directory | `<android dir>/firmware`, `usr/lib/firmware`, `lib/firmware`, `linux-firmware`, `firmware`. With `only_android_dir`, only the first |

Image names (without `.img`):

| Name | Type |
|------|------|
| `firmware`, `linux-firmware` | Firmware |
| `initrd`, `ramdisk` | Initrd (recognized, not mounted) |
| `system`, `system_ext`, `product`, `vendor`, `odm`, `*_dlkm` | System |
| `cache`, `userdata`, `metadata` | Userdata |
| `<system partition>_overlay`, for example `vendor_overlay` | Overlay |

### Which device wins

| Part | `std_parts` | `blk_devices` |
|------|-------------|---------------|
| System partitions | The uevent `PARTNAME` matches the Android partition name | `build.prop` inside tells which partition it is |
| Userdata partitions | `PARTNAME` is `cache`, `userdata` or `metadata` | n/a |
| Overlay partitions (`mount_overlay=std_parts`) | `PARTNAME` is `<system partition>_overlay` | The same |
| Android directory | The last device that has it. With `androidboot.android_dir_dev`, only that device | The same |
| Firmware directory | The device of the Android directory, if it has one. Otherwise the last device that has it | The same |

If two devices claim the same slot, a device on the disk of the Android
directory wins, since another installation may be on another disk.
Among the rest, the later uevent wins. Overlay partitions are only taken
from the disk of the Android directory. Without an Android directory,
the later uevent wins. A device with a `PARTNAME` ending in `_overlay` (of a system
partition) is never used for anything else, even with a `build.prop`.

## Generating the fstab

`OnPostBlockDevices(is_recovery_mode)` builds the fstab in this order,
and then calls `DynamicMountHelpers::MountPartitions()`.

| # | Source | Entries |
|---|--------|---------|
| 1 | Android directory device (if needed) | Mounted on `.../android` first |
| 2 | System partitions (`std_parts`, `blk_devices`) | One entry per partition per filesystem in `ext4`, `erofs`, `squashfs`. Writable `ext4` also gets a read-only entry as fallback |
| 3 | Userdata partitions (`std_parts`) | One entry per filesystem in `ext4`, `f2fs`. Only `/data` is required, the rest are `nofail` |
| 4 | Images | Each image gets a loop device (direct IO with 4096-byte sectors, read-only when the device is). System images: as in 2. Userdata images: as in 3. Firmware image: `/mnt/vendor/firmware`, `nofail` |
| 5 | `bind_mount_dir` | Bind mount of `userdata`, `cache`, `metadata` directories from the Android directory |
| 6 | `tmpfs` | tmpfs for `/cache`, `/data`, `/metadata` |
| 7 | Firmware directory | Bind mount on `/mnt/vendor/firmware`, `nofail` |
| 8 | Overlays | See [Overlays](#overlays) |
| 9 | Addon fstabs | Appended last |

In recovery mode only the firmware parts apply (4 and 7), and images
other than firmware are ignored.

### Overlays

`androidboot.mount_overlay` puts a read-only overlay above system
partitions. Every overlay is optional: a partition without one is mounted
as before, and an overlay that can not be used is logged and ignored.

| Value | Overlay of system partition `<part>` |
|-------|--------------------------------------|
| `disable` | None |
| `std_parts` | The block device with `PARTNAME` `<part>_overlay` |
| `imgs` | `<android dir>/<part>_overlay.img`, on a read-only loop device |
| `imgs_ram` | The same, copied to RAM first |
| `dirs` | The directory `<android dir>/<part>_overlay` |

It does not depend on `mount_system`: overlay images or directories work
with system partitions too.

- The root of an overlay is laid over the mount point of the partition:
  `system_overlay/bin/foo` replaces `/system/bin/foo`. For `system`, the
  `/system` directory, not the root (system-as-root): `overlay_remounter`
  mounts the overlays again in second stage init, which can not replace
  the root of a running process.
- Overlay partitions and images are mounted read-only on
  `.../overlay/<part>` while the fstab is generated, with the filesystems
  of system partitions (`ext4`, `erofs`, `squashfs`).
- An overlay entry (`lowerdir=<overlay>:/<part>`, read-only, `nofail`) is
  added before the addon fstab entries, so that addon overlays of the
  same mount point go on top of it. Like them, it is mounted after
  `SwitchRoot()`.
- With `dirs` the Android directory stays mounted.

Limits:

- overlayfs stacks at most two levels. An overlay with an addon overlay
  of the same mount point on top of it leaves no level for
  `adb remount`.
- `dirs` does not work on `vfat`: overlayfs refuses lower directories on
  filesystems with their own name hashing and comparison (also `ntfs3`
  with `nocase`). Use images there.
- Removing files of the partition (whiteouts, opaque directories) needs
  a filesystem with extended attributes, which `dirs` does not have on
  `ntfs`.
- The directories of an overlay give their mode, owner and SELinux label
  to the directories they are laid over, its root to the root of the
  partition: they must be like those of the partition (`0755`, `root`).
- Files need correct SELinux labels to be usable when enforcing.

Entries generated here are marked `first_stage_mount`. For several
entries with the same mount point, the first one that mounts wins.

### Copy to RAM

`img_ram` and `imgs_ram` copy the image into `.../img` (tmpfs) first and
loop-mount the copy. The boot media can then be removed. Userdata images
are never copied. `mount_overlay=imgs_ram` does the same for overlay
images.

### Releasing the boot media

After the fstab is generated, the Android directory mount is
unmounted if nothing uses it. Live boot users can remove the media. Not
with `mount_overlay=dirs`, whose directories are only used later.

## Mounting

`DynamicMountHelpers::MountPartitions()` works like AOSP's fstab mount
with some changes.

| Step | What |
|------|------|
| System as root | If `/system` is in the fstab it is mounted first, then `SwitchRoot("/system")` makes it the root |
| Normal entries | Mounted in order. A failure is tolerated for `nofail` and `formattable` entries, otherwise it stops the boot |
| `emmc` entries | Skipped |
| Overlayfs entries | Mounted after everything else. `upperdir` and `workdir` directories are created if missing |
| Overlay for `/system` | If neither `/system` nor `/` is in the fstab, `/` from `/proc/mounts` is used, then `fs_mgr_overlayfs_mount_all()` runs |

The overlayfs code is copied from `fs_mgr` with small edits.

## Addon fstabs

| Item | Detail |
|------|--------|
| Parameter | `androidboot.addon_fstab_suffix=a,b` |
| File | `fstab.generic_init.addon.<suffix>`, looked up in `/system/etc/` **of the ramdisk** |
| When read | In `OnPreBlockDevices()`, before any Android partition is mounted |
| Packaging | It must be in the ramdisk: `prebuilt_etc` with `ramdisk: true`, for example `fstab.generic_init.addon.basic`. A file that only exists in the Android system partition is never found |
| When mounted | Last, after all generated entries, in the same `MountPartitions()` call |
| Paths inside | Source and target paths refer to the mounted Android system (for example `/vendor/...` or `/system/...`), since that is what exists when they are mounted |
| Parsed with | The fs_mgr fstab parser, without the usual skipping of partitions |
| A missing or broken file | Logged (`Failed to load addon fstab`) and skipped; the boot goes on without it |
| Not used in | Recovery |

## Limits

| Limit | Detail |
|-------|--------|
| Block device removal | Not handled (a TODO in the source) |
| Probing cost | Every device is mounted once. Many large devices slow the boot |
| First match wins | Two disks with the same partition names pick the later one |
| Failure | Missing required pieces make init wait. A failed required mount is fatal |
