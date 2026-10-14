# AGENTS.md - generic_init

Agents must read this file before touching anything in this directory.

Part of the mainline repository set. Map of all repos:
`vendor/mainline/docs/REPOSITORIES.md`.

`generic_init` is a fork of AOSP `system/core/init` (base
`android-17.0.0_r1`) with a dynamic block device discovery and mount
path. It is a copy, so every difference is a future merge conflict.

## Read first

| Task | Read |
|------|------|
| Anything | `docs/README.md` |
| Boot sequence | `docs/BOOT_FLOW.md` |
| Mounting, fstab, parameters | `docs/DYNAMIC_MOUNT.md` |
| Uevents, modules, firmware | `docs/UEVENTD.md` |
| Build flags, modules | `docs/BUILDING.md` |
| Updating to a new Android version | `docs/MAINTENANCE.md` |
| What is already different from AOSP | `docs/DIFFERENCES_FROM_AOSP.md` |
| Parameters as a user sees them | `device/mainline/generic/docs/boot-parameters.md` |

## Hard rules

- No Android build, flashing or test without asking the human first
  (`hardware/mainline/common/docs/WORKFLOW.md`).
- Do not search from the AOSP tree root. AOSP init is at
  `system/core/init`; look only there.
- Keep changes against AOSP small and local. Prefer a guard or a
  parameter over deleting upstream code.
- Do not reformat files you do not change. Run `clang-format` on the
  files you touched only.
- Do not edit `README.md` and `README.ueventd.md` here; they are the
  AOSP init language docs. Document `generic_init` in `docs/`.
- A change to behavior updates the matching page in `docs/`, and
  `docs/DIFFERENCES_FROM_AOSP.md` when it adds or removes a difference
  from AOSP, in the same commit.
- Think about both the normal boot and the recovery boot.
- First stage runs before properties, SELinux policy and most of
  `/dev` exist. Do not call code that needs them.
- New files start with the SPDX header. Files copied from AOSP keep
  their Apache header.
- The ramdisk variant is a static executable with a reduced source
  list; a new source needed there must be added to its list in
  `Android.bp`.
- Do not make the build depend on modules that are invisible to this
  soong namespace without a guard (see `HAVE_INVISIBLE_MODULES` in
  `service.cpp`).

## Layout

| Path | What |
|------|------|
| `first_stage_init.cpp` | The whole first stage sequence |
| `dynamic_mount_handler.*`, `dynamic_mount_helpers.*` | Block device discovery and mounting |
| `iso_file.*`, `ventoy_image.*` | An ISO booted as a file (Ventoy, GRUB loopback), as a block device |
| `ueventd.cpp`, `devices.cpp`, `firmware_handler.cpp`, `modalias_handler.cpp`, `coldboot*.cpp` | ueventd, with the first run and second run logic |
| `first_stage_console.*` | The debug console |
| `Android.bp` | Modules `generic_init_first_stage`, `generic_init_second_stage.recovery`, `libgeneric_init` |
| everything else | AOSP init, nearly unchanged |
