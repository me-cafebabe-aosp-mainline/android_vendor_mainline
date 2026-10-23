# Differences from AOSP init

Every change against the base copy of `system/core/init` at
`android-17.0.0_r1`, grouped by purpose. Use it to review a change, or
to re-apply the changes on a newer base (see [MAINTENANCE.md](MAINTENANCE.md)).

**TL;DR:** 5 new files, about 20 files with real changes, the rest only
renamed paths or reformatted. Tests and unused modules are removed.

## New files

| File | Purpose |
|------|---------|
| `dynamic_mount_handler.{h,cpp}` | Finds block devices and generates the fstab. See [DYNAMIC_MOUNT.md](DYNAMIC_MOUNT.md) |
| `dynamic_mount_helpers.{h,cpp}` | Mounts the fstab: system-as-root switch, overlayfs |
| `iso_file.{h,cpp}`, `ventoy_image.{h,cpp}` | Finds an ISO booted as a file and attaches it to a loop device. See [DYNAMIC_MOUNT.md](DYNAMIC_MOUNT.md) |
| `loop_device.{h,cpp}`, `partition_table.{h,cpp}` | Attaches loop devices without waiting for ueventd, and finds the partitions of a partition table put on a partition. See [DYNAMIC_MOUNT.md](DYNAMIC_MOUNT.md) |
| `usb_gadget.{h,cpp}` | Makes the USB gadget of tethered boot in first stage, and `init.cpp` leaves the USB controller to it. See [USB_GADGET.md](USB_GADGET.md) |
| `coldboot_runner_noparallel.cpp` | Cold boot handled one uevent at a time |
| `docs/` | These documents |

## Changed behavior

| File | Change | Why |
|------|--------|-----|
| `first_stage_init.cpp` | New dynamic mount path with two ueventd runs; module loading by prefix list (compressed modules included) and from `/vendor/lib/modules`; `ExecuteVendorInitProgram()`; module loading never fatal; `/dev/block` created; no `clearenv()`; stdio set to `/dev/null` only before `execv`; `use_first_stage_mount` flag | Replace the first stage mount. See [BOOT_FLOW.md](BOOT_FLOW.md) |
| `ueventd.cpp`, `ueventd.h` | `ueventd_main(config, first_run)`; exit conditions; 30 s cap; per-uevent log on second run; cold boot every time; `SIGCHLD` left alone; modalias paths per run (the `<uname -r>` subdirectory if there is one); delay parameter | Run ueventd inside first stage. See [UEVENTD.md](UEVENTD.md) |
| `ueventd_parser.h` | Modalias handling on by default; 16 MiB uevent socket buffer | Needed without a `.rc` that enables them |
| `uevent_listener.{h,cpp}` | `Poll()` can restart its timeout on every uevent | The 5 s quiet rule |
| `coldboot.cpp`, `coldboot_runner.h` | Always use `ColdbootRunnerNoParallel` | `DynamicMountHandler` is not thread safe |
| `devices.{h,cpp}` | `HandleDevice()` takes the `Uevent`; block `add` calls `OnBlockDeviceAdd()`; missing SELinux label no longer skips the node; `mknod` on an existing node always fixes the mode | Feed the mount handler, and work before policy is loaded |
| `firmware_handler.cpp` | Removed the wait for `/dev/.booting` and the wait for `apexd`; `.zst` and `.xz` firmware is decompressed and `liblzma` is linked | Neither exists at this stage; the kernel does not decompress the sysfs fallback |
| `builtins.cpp` | `insmod` passes `MODULE_INIT_COMPRESSED_FILE` for `.gz`, `.xz` and `.zst` modules | Compressed kernel modules |
| `modalias_handler.{h,cpp}` | Optional delay before each module load | Find the module that crashes the boot |
| `util.h` | `IsCompressedKernelModule()` and a guarded `MODULE_INIT_COMPRESSED_FILE` | `insmod` and the first stage prefix list build without the compressed module support of `libmodprobe` |
| `util.cpp` | `TranslatePropName()`: `ro.board.platform`, `ro.hardware` and `ro.boot.*` read from boot parameters in `ExpandProps()`; `reboot_utils.h` always included; `HAVE_LIBUNWIND` guard | `.rc` expansion before properties exist |
| `reboot_utils.cpp` | `androidboot.init_fatal_pause`; `HAVE_LIBUNWIND` guard around the unwinder | Keep a crash readable; build without libunwind |
| `first_stage_console.{h,cpp}` | Mode 3 (`START_VENDOR_SH_BOOTSTRAP`) and a program argument; `setsid()` and `TIOCSCTTY` checked; waits for its child with `waitpid()` | Console from `/vendor/bin/bootstrap/sh`; fixes in the console code |
| `host_init_stubs.h` | Removed stubs for `SetFatalRebootTarget()` and `InitFatalReboot()` | They are real functions now |

## Build-only changes

| File | Change |
|------|--------|
| `Android.bp` | Only the `generic_init` modules remain; `generic_init` soong config guards `enabled` and `libzstd`; recovery and ramdisk variants (see [BUILDING.md](BUILDING.md)) |
| `service.cpp` | Code that needs `bionic_libc_platform_headers` is guarded by `HAVE_INVISIBLE_MODULES`, which is never defined: the signal profiler handling before `exec` and the MTE upgrade on crash |
| `init.cpp`, `property_service.cpp`, `subcontext_android.h` and others | Include paths changed from `system/core/init/...` to `vendor/mainline/services/generic_init/...` (for the generated `*.pb.h`) |
| `.clang-format` and many files | Formatted with the repository's `clang-format` (no behavior change) |

## Removed

| Removed | Why |
|---------|-----|
| Unit tests (`*_test.cpp`), `test_*` directories, `AndroidTest.xml`, `TEST_MAPPING` | Not built here |
| `fuzzer/`, `init_benchmarks`, `subcontext_benchmark.cpp` | Not built here |
| `libprefetch/` (Rust), the `parser/` directory (tokenizer sources and tests) | Not needed |
| `compare-bootcharts.py`, `grab-bootchart.sh`, `perfboot.py`, `extra_free_kbytes.sh`, `host_builtin_map.py` | Helpers not needed |
| Android.bp modules `init_second_stage`, `*.microdroid`, `init_system`, `init_vendor`, `host_init_verifier`, `libinit_host`, test libraries, `init` phony | Only the `generic_init` binaries are built from here |
| The `ueventd_flags` and `init_flags` aconfig declarations | The ones in `system/core/init` are used |

The `.aconfig` files, `host_init_verifier.cpp` and other sources that
are no longer built stay in the directory so a rebase stays simple.

## Not changed

| Area | Note |
|------|------|
| Init language and parser | Same, so `README.md` and `README.ueventd.md` still apply |
| Second stage (`init.cpp`, services, properties, subcontext) | Same code; normal boots run AOSP's `/system/bin/init` anyway |
| `selinux.cpp`, `switch_root.cpp`, `snapuserd_transition.cpp` | Same |
| `first_stage_mount*.cpp` | Same, but unused |
