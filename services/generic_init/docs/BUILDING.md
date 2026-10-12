# Building generic_init

**TL;DR:** set the soong config `generic_init enabled`, add the
package for your variant, point `rdinit=` at it.

This page lists what to wire up. Builds and tests follow
`hardware/mainline/common/docs/WORKFLOW.md`.

## Modules

All in `Android.bp` of this directory.

| Module | Type | Variant | Use |
|--------|------|---------|-----|
| `generic_init_first_stage` | Static executable, `ramdisk: true` | Ramdisk | First stage for normal boot. Installed as `/system/bin/generic_init` in the ramdisk |
| `generic_init_second_stage.recovery` | Executable, `recovery: true` | Recovery | Same program for recovery images (built with `-DRECOVERY`) |
| `libgeneric_init` | Static library | Platform, recovery | The shared sources, used by the recovery binary |
| `generic_init_defaults` | `cc_defaults` | | Flags and libraries shared by the above |

Both executables use the stem `generic_init`.

The ramdisk variant has a reduced source list and
links statically, because the ramdisk has no libraries yet.

## Enabling it

| Setting | Where | Meaning |
|---------|-------|---------|
| `$(call soong_config_set_bool,generic_init,enabled,true)` | Device `device.mk` | Without it, every module here is disabled |
| `PRODUCT_PACKAGES += generic_init_first_stage` | Device `device.mk` | Install into the ramdisk |
| `PRODUCT_PACKAGES += generic_init_second_stage.recovery` | Device `device.mk` | Install into recovery |
| `BOARD_KERNEL_CMDLINE += rdinit=/system/bin/generic_init` | Device `BoardConfig.mk` | Start it as the first process |

`libzstd` is added to the static libraries only when the config is
enabled, because it is not visible to this soong namespace otherwise
and would break builds of devices that do not use `generic_init`.

## What else the ramdisk needs

| File | Purpose |
|------|---------|
| `/system/etc/ueventd.ramdisk.rc` | First run ueventd config. A device can copy `system/core/rootdir/ueventd.rc` |
| `/lib/modules/modules.load` (+ `_prefix`) | Modules to load before mounting |
| Firmware for those modules | In the directories named by the ueventd config |
| `fstab.generic_init.addon.*` | Optional addon fstabs, in the ramdisk |

The mounted system needs `/system/etc/ueventd.rc` for the second run.
The example device trees are `device/mainline/generic` (ramdisk) and
`device/apple/snowcastle` (ramdisk and recovery).

## Build flags

Set in `generic_init_defaults` and `generic_init_first_stage_defaults`.

| Flag | Default | In debuggable builds |
|------|---------|----------------------|
| `ALLOW_FIRST_STAGE_CONSOLE` | 0 | 1 |
| `ALLOW_LOCAL_PROP_OVERRIDE` | 0 | 1 |
| `ALLOW_PERMISSIVE_SELINUX` | 0 | 1 |
| `REBOOT_BOOTLOADER_ON_PANIC` | 0 | 1 |
| `WORLD_WRITABLE_KMSG` | 0 | 1 |
| `DUMP_ON_UMOUNT_FAILURE` | 0 | 1 |
| `SHUTDOWN_ZERO_TIMEOUT` | 0 | 1 in `eng` builds |
| `LOG_UEVENTS` | 0 | 0 |
| `FIRST_STAGE_INIT` | 1 (ramdisk variant) | |
| `INIT_FULL_SOURCES` | on (library variant) | |
| `HAVE_LIBUNWIND` | on (library variant) | |

The first stage console only exists in debuggable builds, because of
`ALLOW_FIRST_STAGE_CONSOLE`.

## Libraries

| Linked | From |
|--------|------|
| `libinit_flags`, `libueventd_flags` | The aconfig libraries of `system/core/init` |
| `libmodprobe`, `libfs_mgr`, `libselinux`, `libbase`, ... | Platform |
| `libgsi`, `liblp`, `libsnapshot_*`, `update_metadata-protos` | Ramdisk variant, for snapuserd and OTA handling |

The aconfig declarations were removed from `Android.bp` here; the
`*.aconfig` files left in the directory are unused copies.

## Check

- [ ] `m generic_init_first_stage` (and the recovery module, if used)
- [ ] The kernel command line has `rdinit=`
- [ ] The ramdisk contains `ueventd.ramdisk.rc` and the modules for the
      boot media
