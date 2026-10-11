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

## Parameters

Read with the same lookup as `.rc` expansion: kernel command line or
bootconfig. Unset means the default.

| Parameter | Default | Values |
|-----------|---------|--------|
| `androidboot.mount_system` | `std_parts` | `std_parts`, `blk_devices`, `imgs`, `imgs_ram` |
| `androidboot.mount_userdata` | `std_parts` | `std_parts`, `imgs`, `bind_mount_dir`, `tmpfs` |
| `androidboot.mount_firmware` | `all_possible_dirs` | `all_possible_dirs` (or `true`), `disable` (or `false`), `only_android_dir`, `img`, `img_ram` |
| `androidboot.android_dir` | none | Directory name to look for first |
| `androidboot.addon_fstab_suffix` | none | Comma separated suffixes |

An invalid value is logged and the default stays.

What each parameter makes the handler wait for:

| Setting | Needs to be found before ueventd may quit |
|---------|-------------------------------------------|
| `mount_system=std_parts` or `blk_devices` | A block device for every Android system partition (see below) |
| `mount_userdata=std_parts` | A block device for `userdata` and `metadata` (and `cache`, only if the system partition has a `/cache` directory) |
| `mount_firmware=all_possible_dirs` or `only_android_dir` | A block device with a firmware directory |
| Any image mode, `bind_mount_dir` | A block device with the Android directory |
| `imgs`, `tmpfs`, `disable` | Nothing for that part |

## Probing a block device

For every `add` uevent of a block device, except names starting with
`dm-`, `loop`, `ram` or `zram`:

1. Try APFS first: volumes 0 to 19 with `vol=N`. A volume counts only if
   it holds an Android directory.
2. Otherwise mount it read-only on `try`, testing in order: `ext4`, `f2fs`,
   `erofs`, `squashfs`, `vfat`, `ntfs`, `iso9660`, `udf`. Only `EINVAL` and
   `ENODEV` mean "try the next one".
3. Check what is inside (below).
4. Try a read-write remount, to know if it is writable.
5. Unmount.

Everything learned is stored per device and used by `UpdateMountInfo()`.

### What is recognized

| Found | How |
|-------|-----|
| System partition | `/system/build.prop`, or `build.prop` or `etc/build.prop` with a `ro.<part>.build.id=` line (`blk_devices` mode) |
| Partitions inside | In a `system` device: `product`, `system_dlkm`, `system_ext`, `vendor`, and `vendor/{odm,odm_dlkm,vendor_dlkm}`. In a `vendor` device: `odm`, `odm_dlkm`, `vendor_dlkm`. Such partitions need no device of their own |
| `/cache` directory in `system` | Marks `cache` as needing a mount |
| Android directory | `androidboot.android_dir`, then `android`, then `boot/android` |
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

### Which device wins

| Part | `std_parts` | `blk_devices` |
|------|-------------|---------------|
| System partitions | The uevent `PARTNAME` matches the Android partition name | `build.prop` inside tells which partition it is |
| Userdata partitions | `PARTNAME` is `cache`, `userdata` or `metadata` | n/a |
| Android directory, firmware directory | The last device that has it | The same |

If two devices claim the same slot, the later uevent replaces the
earlier one.

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
| 8 | Addon fstabs | Appended last |

In recovery mode only the firmware parts apply (4 and 7), and images
other than firmware are ignored.

Entries generated here are marked `first_stage_mount`. For several
entries with the same mount point, the first one that mounts wins.

### Copy to RAM

`img_ram` and `imgs_ram` copy the image into `.../img` (tmpfs) first and
loop-mount the copy. The boot media can then be removed. Userdata images
are never copied.

### Releasing the boot media

After the fstab is generated, the Android directory mount is
unmounted if nothing uses it. Live boot users can remove the media.

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
