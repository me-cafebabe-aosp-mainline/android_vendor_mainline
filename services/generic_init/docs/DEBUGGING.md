# Debugging generic_init

**TL;DR:** read the kernel log for lines starting with the names below,
and use `androidboot.init_fatal_pause=true` to keep a crash on screen.

For shell access, serial consoles and adb during a crash loop, see
`device/mainline/generic/docs/debugging.md`. This page covers what is
specific to `generic_init`.

## Parameters for debugging

| Parameter | Effect |
|-----------|--------|
| `androidboot.init_fatal_pause=true` | On a fatal error init calls `pause()` instead of rebooting, so the log stays readable |
| `androidboot.modalias_handling_delay_ms=N` | Sleep N ms before each module load; find the module that crashes the boot |
| `androidboot.first_stage_console=N` | See below. Debuggable builds only |
| `androidboot.init_fatal_reboot_target`, `androidboot.init_fatal_panic` | As in AOSP |

### First stage console

`androidboot.first_stage_console=N`, in the command line or bootconfig.
Needs a debuggable build (`ALLOW_FIRST_STAGE_CONSOLE`) and a `console=`
on the kernel command line.

| N | Meaning in generic_init |
|---|-------------------------|
| 0 | Off |
| 1 | No effect here (it only had a meaning for the AOSP first stage mount) |
| 2 | Start the console right after the ramdisk modules are loaded, before any mount. Uses `/system/bin/sh` from the ramdisk |
| 3 | Start the console after everything is mounted and vendor modules are loaded. Uses `/vendor/bin/bootstrap/sh` |

The console first runs `/first_stage.sh` if it exists, then the shell.
When the shell exits, the boot goes on. Mode 3 needs a shell at
`/vendor/bin/bootstrap/sh` (a device tree can package one, for example
as `sh_vendor_bootstrap`).

## Log lines to look for

Everything goes to the kernel log (`dmesg`, or the serial console).

| Line | Meaning |
|------|---------|
| `generic init first stage started!` | First stage is running |
| `generic_init ueventd started!` | A ueventd run began |
| `Parameter mount_system is unset` | The default is used |
| `Block device <dev> detected as system partition <p>` | A system partition was recognized |
| `Block device <dev> have android directory: <dir>` | An Android directory was found |
| `Block device <dev> have firmware directory: <dir>` | A firmware directory was found |
| `Block device <dev> can be mounted as read-write` / `read-only` | Result of the probe |
| `Set block device for system partition <p> to <dev>` | A slot is filled |
| `CanQuitUeventd: Missing block device for ...` | ueventd is still waiting for this |
| `Exit generic_init ueventd` | First run done |
| `Fstab entry: blk_device=... mount_point=... fs_type=...` | The generated fstab, one line per entry |
| `Handle uevent ACTION=... DEVPATH=...` | Second run only: every uevent |
| `Deadline reached, exiting ueventd main loop` | Second run hit the 30 s cap |
| `Delay for N ms before ...` | Module load delay is active |
| `console shell exited` | A first stage console ended |

## Symptoms

| Symptom | Likely cause | Check |
|---------|--------------|-------|
| `CanQuitUeventd: Missing block device for system partition X` repeats | Storage driver not loaded, or the partition is not named `X` | Ramdisk modules, `PARTNAME` of the partition, `androidboot.mount_system` |
| `Missing block device for userdata partition` | Same, for `userdata` or `metadata` | `androidboot.mount_userdata` |
| `Missing block device for android dir` | No device has the directory | `androidboot.android_dir` (only that name when set), directory name (`android`, `boot/android`) |
| `Missing block device for firmware dir` | No firmware directory found | `androidboot.mount_firmware=disable` if none is needed |
| `No known filesystem detected for <dev>` | Filesystem driver missing | Add the module to the ramdisk |
| `Failed to mount partitions` (fatal) | A required entry failed | The `Fstab entry:` lines above it |
| `Failed to determine android partition from build.prop` | `blk_devices` mode cannot tell which partition it is | The `ro.<part>.build.id` lines |
| `Firmware image was not found` (fatal) | `mount_firmware=img` without a firmware image | Android directory contents |
| Boot stops after a crash and never reboots | `init_fatal_pause=true` is set | Read the log, then remove it |
| A module crashes the boot | Unknown which | `modalias_handling_delay_ms` and read the last `Delay for` line |
| `secilc` fails with `ECHILD` on a GSI | `SIGCHLD` was ignored by ueventd | Fixed by leaving `SIGCHLD` alone, see [UEVENTD.md](UEVENTD.md) |

## Reading a boot

1. Find `generic init first stage started!`.
2. Follow the `Block device ...` lines: what was found, and where.
3. Look for the last `CanQuitUeventd` line before `Exit generic_init ueventd`.
4. Read the `Fstab entry:` lines: that is what was mounted.
5. After that the lines come from AOSP init, `selinux_setup` first.
