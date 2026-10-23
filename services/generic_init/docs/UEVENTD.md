# ueventd inside generic_init

`generic_init` carries its own copy of `ueventd` and runs it **twice**
during first stage init. This page covers how that differs from the
AOSP daemon.

Files: `ueventd.cpp`, `devices.cpp`, `modalias_handler.cpp`,
`firmware_handler.cpp`, `coldboot*.cpp`, `uevent_listener.cpp`.

**TL;DR:** the first run discovers what is needed to mount Android and
quits as soon as it has it. The second run, with the real system's
config, loads more modules and quits when things go quiet.

## Two runs

`ueventd_main(config, first_run)` in `ueventd.cpp`.

| | First run | Second run |
|-|-----------|------------|
| `first_run` | `true` | `false` |
| When | Before mounting | After mounting and `vendor_init` |
| Config | `/system/etc/ueventd.ramdisk.rc` (ramdisk) | `/system/etc/ueventd.rc` (the mounted system) |
| Modules from | `/lib/modules` | `/system/lib/modules` and `/vendor/lib/modules` |
| Stops when | `CanQuitUeventd()` says all required block devices exist | 5 seconds without a uevent, or at the first uevent after 30 seconds |
| Logs | Normal | Logs every uevent handled |

In recovery there is only the first kind of run, with the recovery
image's `/system/etc/ueventd.rc`.

### The stop conditions in detail

| Rule | Source |
|------|--------|
| First run: after each uevent, stop if `CanQuitUeventd(false)` is true | `main_loop()` |
| First run: if polling goes quiet for 5 s and things are still missing, log what is missing (`CanQuitUeventd(true)`) and keep waiting | `main_loop()` |
| Second run: `Poll()` returns after 5 s without uevents (the timer restarts on every uevent) | `UeventListener::Poll()` |
| Second run: the first uevent after 30 s stops the loop | `main_loop()` |

The 30 s deadline is checked only when a uevent arrives. A quiet
system stops earlier through the 5 s rule.

The cold boot also stops the first run early if everything required is
already found.

## Differences in behavior

| Area | AOSP | generic_init |
|------|------|--------------|
| Cold boot | Skipped if `ro.cold_boot_done` is set | Runs every time, one run per ueventd start |
| Cold boot handlers | Thread pool or subprocesses | `ColdbootRunnerNoParallel`: one by one, in order. `DynamicMountHandler` needs this |
| Main loop | Never returns | Returns `EXIT_SUCCESS` when done |
| `SIGCHLD` | Ignored after cold boot | Left alone (the code is commented out), so `secilc` can be waited for |
| Modalias handling | Off unless enabled in the `.rc` | **On** by default |
| Uevent socket buffer | 0 (kernel default) | 16 MiB by default |
| Missing SELinux label for a device | Device not created | Logged, device is still created |
| `mknod` on an existing node | Fix the label only if there is a context | Always fix the mode; fix the label if there is a context |
| Block device `add` | Normal handling | Also calls `DynamicMountHandler::OnBlockDeviceAdd()` |

The defaults for modalias handling and the socket buffer are in
`UeventdConfiguration` (`ueventd_parser.h`). A `.rc` file can still
change them.

## Modalias handler

`ModaliasHandler` loads a module for each uevent that has a `MODALIAS`.

| Item | Detail |
|------|--------|
| Search paths | First run `/lib/modules`; second run `/system/lib/modules`, `/vendor/lib/modules`. Each one is its `<uname -r>` subdirectory if there is one, as for first stage module loading (Debian kernels keep `modules.alias` and `modules.dep` there), otherwise the directory itself |
| Delay | `androidboot.modalias_handling_delay_ms=N` sleeps N ms before each module load |
| Use of the delay | Slow down loading to see which module crashes the boot |
| Where it applies | The serial handler and the thread pool path both honor it |

## Firmware handler

The AOSP waits for things to appear during boot. `generic_init` does not.

| Removed | Why |
|---------|-----|
| Wait for `/dev/.booting` to go away, retry every 100 ms | There is no late-init phase at this stage |
| Wait for `apexd.status=activated` and rerun the external handler | There is no apexd yet |

A firmware request is tried once against the directories in the
ueventd config (`firmware_directories`). If nothing matches, the error
is logged and the load fails. The kernel module that asked has to cope.

In each directory the file is tried as is, then with `.zst` and `.xz`
added, as the kernel does. The kernel does not decompress what the sysfs
fallback receives, so ueventd decompresses these two itself and streams
the result to the `data` node.

Firmware directories come from the config of each run, so the second
run can use directories that only exist after mounting
(`/vendor/firmware`, `/mnt/vendor/firmware`, ...).

## Block devices

`DeviceHandler::HandleDevice()` was changed to take the whole `Uevent`.
After the device node is made, a block device `add` is passed to
`DynamicMountHandler::OnBlockDeviceAdd()` with its path and symlinks.
The symlinks (`by-name`, `by-uuid`, ...) are made by the normal
ueventd code before that call.

## Properties in `.rc` files

Properties are not available yet, so `ExpandProps()` in `util.cpp`
reads boot parameters instead.

| Property written in `.rc` | Read from |
|---------------------------|-----------|
| `${ro.board.platform}` | `androidboot.board_platform` |
| `${ro.hardware}` | `androidboot.hardware` |
| `${ro.boot.<x>}` | `androidboot.<x>` |
| Anything else | Not expanded: an unset value with no default is an error |

This is what lets `/vendor/etc/ueventd.rc` import
`ueventd.${ro.hardware}.rc` at this stage.

## Other changes in the area

| Change | Where |
|--------|-------|
| `/dev/block` is created in first stage | `first_stage_init.cpp` |
| Fatal errors can pause instead of rebooting | `reboot_utils.cpp`, see [DEBUGGING.md](DEBUGGING.md) |
| Stack traces on fatal errors need `HAVE_LIBUNWIND`. It is set for the recovery variant, not for the static ramdisk variant | `reboot_utils.cpp`, `util.cpp`, `Android.bp` |
