# Boot flow

What `generic_init` does from the first instruction to the hand-over,
in terms of the source. Entry point: `FirstStageMain()` in
`first_stage_init.cpp`.

**TL;DR:** set up `/dev`, `/proc`, `/sys` → load ramdisk modules → run
ueventd until the needed block devices exist → mount them → run
`vendor_init` → load vendor modules → run ueventd again until quiet →
`exec /system/bin/init selinux_setup`.

## Where it sits

```
kernel ──► generic_init (ramdisk, static) ──► /system/bin/init selinux_setup
            first stage + ueventd + mounts      AOSP second stage, unchanged
```

| Item | Value |
|------|-------|
| Kernel parameter | `rdinit=/system/bin/generic_init` |
| Module | `generic_init_first_stage` (ramdisk), `generic_init_second_stage.recovery` (recovery) |
| `main()` | `first_stage_main.cpp` (ramdisk), `main.cpp` (recovery) |

## Normal boot

| # | Step | Function or file |
|---|------|------------------|
| 1 | Mount `/dev`, `/dev/pts`, `/proc`, `/sys`, `selinuxfs`, `/mnt`, `/debug_ramdisk`, `/second_stage_resources`. Create `/dev/kmsg`, `/dev/null`, ... | `FirstStageMain()` |
| 2 | Read `/proc/cmdline` and `/proc/bootconfig` | `FirstStageMain()` |
| 3 | Decide on the first stage console (debuggable builds only) | `FirstStageConsole()` |
| 4 | Load kernel modules from `/lib/modules` (ramdisk). Failures are logged, not fatal | `LoadKernelModules()` |
| 5 | Resume from hibernation if asked | `MaybeResumeFromHibernation()` |
| 6 | Console mode 2: start the console now | `StartConsole()` |
| 7 | Copy ramdisk props, handle `/force_debuggable` | as in AOSP |
| 8 | Create the temp mount directories, read the mount parameters | `DynamicMountHandler::OnPreBlockDevices()` |
| 9 | Run ueventd with `/system/etc/ueventd.ramdisk.rc` until enough block devices exist | `ueventd_main(config, first_run=true)` |
| 10 | Move `snapuserd` if needed, switch root to `/first_stage_ramdisk` | `PrepareSwitchRoot()`, `SwitchRoot()` |
| 11 | Build the fstab and mount everything | `DynamicMountHandler::OnPostBlockDevices()` |
| 12 | Run `/vendor/bin/vendor_init` if it exists | `ExecuteVendorInitProgram()` |
| 13 | Load kernel modules from `/vendor/lib/modules` | `LoadKernelModules()` |
| 14 | Run ueventd with `/system/etc/ueventd.rc` (from the mounted system) until quiet | `ueventd_main(config, first_run=false)` |
| 15 | Console mode 3: start `/vendor/bin/bootstrap/sh` | `StartConsole()` |
| 16 | Free the old ramdisk if the root changed | `FreeRamdisk()` |
| 17 | `exec /system/bin/init selinux_setup` | `FirstStageMain()` |

Steps 8 to 14 are what replace the AOSP first stage mount. The details
are in [DYNAMIC_MOUNT.md](DYNAMIC_MOUNT.md) and [UEVENTD.md](UEVENTD.md).

## Recovery boot

A boot is a recovery boot when `IsRecoveryMode()` is true and
`androidboot.force_normal_boot=1` is not set.

| Step | What |
|------|------|
| Load ramdisk modules | Same as above, using `modules.load.recovery` if it exists |
| `OnPreBlockDevices()` | Same |
| ueventd | `/system/etc/ueventd.rc` of the recovery image, `first_run=true` |
| `OnPostBlockDevices(true)` | Only mounts firmware (and images of firmware); no system or userdata |
| `vendor_init` | `/system/bin/vendor_init`, not the vendor one |
| After that | No vendor module load, no second ueventd |

## Boot modes and module lists

`GetBootMode()` picks the list file in the module directory:

| Boot mode | Selected by | List file | Falls back to |
|-----------|-------------|-----------|---------------|
| Normal | default | `modules.load` | |
| Recovery | recovery mode | `modules.load.recovery` | `modules.load` |
| Charger | `androidboot.mode=charger` | `modules.load.charger` | `modules.load` |

## Loading modules

`LoadKernelModules(boot_mode, strict, ..., base_dir)` is called with
`/lib/modules` first and `/vendor/lib/modules` later, both with
`strict=false`.

| Rule | Behavior |
|------|----------|
| Directory | A release specific `<uname -r><page size suffix>` directory wins, with no fallback. Otherwise matching `major.minor` directories, then the base directory |
| List file | As in the table above |
| Prefix list | `<list file>_prefix` (for example `modules.load_prefix`) lists name prefixes. Every `.ko` (or `.ko.zst`, `.ko.xz`, `.ko.gz`) in the base directory whose name starts with a prefix is loaded, with its aliases |
| Compression | A module ending in `.gz`, `.xz` or `.zst` is loaded with `MODULE_INIT_COMPRESSED_FILE`, so the kernel (`CONFIG_MODULE_DECOMPRESS`) decompresses it. `modules.dep` and `modules.load` have to name the compressed files |
| Parallel | `androidboot.load_modules_parallel` as in AOSP |
| Timing | The time taken is exported in `kEnvInitModuleDurationMs` |

## `vendor_init`

| Boot | Program |
|------|---------|
| Normal | `/vendor/bin/vendor_init` |
| Recovery | `/system/bin/vendor_init` |

It is run only if it exists, and init waits for it. Use it for
preparations such as generating firmware files.

## The old first stage mount

`first_stage_mount*.cpp` are still compiled. They are used only when
the global `use_first_stage_mount` is true, and nothing sets it. In
practice the dynamic path above is always taken.

## Hand-over

| Item | Detail |
|------|--------|
| Program | `/system/bin/init`, argument `selinux_setup` |
| Standard output and error | `/dev/kmsg`; stdio is set to `/dev/null` just before `execv` |
| HWASAN | `HWASAN_OPTIONS` is set when built with it |

## Boot parameters used here

| Parameter | Used for |
|-----------|----------|
| `androidboot.force_normal_boot` | Force a normal boot instead of recovery |
| `androidboot.mode=charger` | Charger module list |
| `androidboot.load_modules_parallel*` | Parallel module loading |
| `androidboot.first_stage_console` | See [DEBUGGING.md](DEBUGGING.md) |
| `androidboot.hibernation_resume_device` | Resume from hibernation |
| `androidboot.init_fatal_pause` | See [DEBUGGING.md](DEBUGGING.md) |
| `androidboot.mount_*`, `android_dir`, `addon_fstab_suffix` | See [DYNAMIC_MOUNT.md](DYNAMIC_MOUNT.md) |
| `androidboot.modalias_handling_delay_ms` | See [UEVENTD.md](UEVENTD.md) |
