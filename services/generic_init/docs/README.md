# generic_init

An init program for devices that cannot describe their hardware in
advance. It is a fork of AOSP `system/core/init` with extra features.

**TL;DR:** `generic_init` runs as the first process in the ramdisk. It
loads modules, discovers block devices as they appear, builds the
fstab on the fly, mounts Android, and then hands over to the normal
AOSP `/system/bin/init`.

## Where to start

| You want to... | Read |
|----------------|------|
| Follow the boot step by step | [BOOT_FLOW.md](BOOT_FLOW.md) |
| Understand how partitions are found and mounted | [DYNAMIC_MOUNT.md](DYNAMIC_MOUNT.md) |
| Understand the uevent side (modules, firmware, devices) | [UEVENTD.md](UEVENTD.md) |
| Build it, or add it to a device tree | [BUILDING.md](BUILDING.md) |
| Find out why a boot is stuck | [DEBUGGING.md](DEBUGGING.md) |
| See exactly what differs from AOSP | [DIFFERENCES_FROM_AOSP.md](DIFFERENCES_FROM_AOSP.md) |
| Update it for a new Android version | [MAINTENANCE.md](MAINTENANCE.md) |

## What it is

| Fact | Value |
|------|-------|
| Based on | AOSP `system/core/init` at tag `android-17.0.0_r1` |
| Lives in | `vendor/mainline/services/generic_init` |
| Binary name | `generic_init` (stem of the modules below) |
| Runs as | `rdinit=/system/bin/generic_init` in the ramdisk, or in recovery |
| Replaces | AOSP `init_first_stage` and the first stage mount |
| Includes | A copy of `ueventd`, wired into the boot sequence |
| Does not replace | The second stage: `/system/bin/init` still runs as usual |

## How the device trees use it

All of them start it with `rdinit=/system/bin/generic_init`. What
differs is which features they use:

| Tree | What it uses |
|------|--------------|
| `device/mainline/generic` | Everything, because the hardware is unknown: partitions are found at boot, the Android directory can be on any disk or in an ISO, `androidboot.mount_*` is chosen per install method, and an addon fstab from the ramdisk adds the rest |
| `device/samsung/ss8916` | The images of the Android directory on the `userdata` partition (`androidboot.android_dir_dev=PARTLABEL=userdata`, `mount_system=imgs`, `mount_userdata=bind_mount_dir`) and the firmware from its `firmware` subdirectory (`mount_firmware=only_android_dir`), so the phone's partition table stays untouched. The other partitions come from a normal fstab |
| `device/apple/snowcastle` | Two layouts, chosen by `SNOWCASTLE_PARTITION_SCHEME`: `apfs` takes images and firmware from the Android directory with `tmpfs` as userdata, and `normal` uses the standard partitions (`std_parts`). It is optional (`SNOWCASTLE_USE_GENERIC_INIT`), and the firmware generation script reads the directory that `generic_init` mounted |

## What it adds to AOSP init

| Feature | Where |
|---------|-------|
| Block device discovery and fstab generation | `dynamic_mount_handler.cpp` |
| Mounting with system-as-root and overlayfs | `dynamic_mount_helpers.cpp` |
| ueventd running inside first stage init, with exit conditions | `ueventd.cpp` |
| Module loading by prefix list, from ramdisk and from vendor | `first_stage_init.cpp` |
| `vendor_init` hook before the real init | `first_stage_init.cpp` |
| Property expansion from `androidboot.*` while parsing `.rc` | `util.cpp` |
| First stage console from `/vendor/bin/bootstrap/sh` | `first_stage_console.cpp` |
| Pause on fatal error, for reading logs | `reboot_utils.cpp` |
| Delay before loading each module, for finding crashers | `modalias_handler.cpp` |

## Related docs

| Doc | What |
|-----|------|
| `device/mainline/generic/docs/booting-process.md` | The boot sequence as a user sees it |
| `device/mainline/generic/docs/boot-parameters.md` | Every `androidboot.*` parameter |
| `device/mainline/generic/docs/installation.md` | Where to put the Android files |
| `device/mainline/generic/docs/debugging.md` | Shell access and logging tips |
| `README.md` and `README.ueventd.md` in the parent directory | The init language and ueventd syntax. Copied from AOSP and still valid |

This folder documents how `generic_init` is built and works inside.
The pages above document how to use it.
