# Maintenance

**TL;DR:** `generic_init` is a copy, not a patch set. To move to a new
Android version, diff AOSP init between the old and the new tag, apply
that diff here, and keep the local changes listed in
[DIFFERENCES_FROM_AOSP.md](DIFFERENCES_FROM_AOSP.md).

## Facts

| Item | Value |
|------|-------|
| Base | `android-17.0.0_r1` `system/core/init`, imported by commit `Copy android-17.0.0_r1 system/core/init/` |
| Upstream | `system/core/init` (listed in `device/mainline/generic/docs/maintenance.md`) |
| Older history | The `lineage-23.2` branch of `device/mainline/generic` has the history from before the move to this repository |
| Commit order here | Copy, cleanup, rename, `generic_init` features, fixes. `git log --oneline -- services/generic_init` shows it |

## Update to a new Android version

| # | Step |
|---|------|
| 1 | Find the base tag in the import commit message |
| 2 | In `system/core/init`, list what changed between the old and the new tag, with the same file names |
| 3 | Apply those changes here. Paths in includes change from `system/core/init/` to `vendor/mainline/services/generic_init/` |
| 4 | For files this fork changed, merge by hand, using [DIFFERENCES_FROM_AOSP.md](DIFFERENCES_FROM_AOSP.md) as the list |
| 5 | Update `Android.bp`: new sources go to `init_common_sources` or `init_device_sources` and to the ramdisk list if first stage needs them |
| 6 | Update the base tag in this page and in the README |
| 7 | Build and boot (agents only after asking, `hardware/mainline/common/docs/WORKFLOW.md`) |

### Where changes are likely to conflict

| File | Local change |
|------|--------------|
| `first_stage_init.cpp` | The whole boot sequence after module loading |
| `ueventd.cpp` | The main loop and `ueventd_main()` signature |
| `devices.cpp` | `HandleDevice()` signature |
| `firmware_handler.cpp` | Removed waiting code |
| `util.cpp` | `TranslatePropName()` in `ExpandProps()` |
| `reboot_utils.cpp` | `init_fatal_pause` |
| `Android.bp` | Almost everything |

### Easy to miss

| Item | Detail |
|------|--------|
| `LINT.IfChange` in `Android.bp` | `GLOBAL_HWASAN_OPTIONS` must stay in sync with `system/core/rootdir/Android.bp` |
| New modules AOSP init depends on | Add them to `static_libs` or `shared_libs`, and check they are visible to this soong namespace |
| Code needing invisible modules | Guard it, as done in `service.cpp` with `HAVE_INVISIBLE_MODULES`, or add the dependency if it becomes visible |
| `libinit_flags`, `libueventd_flags` | Come from `system/core/init`; check the names still exist |
| New `first_stage_mount` behavior | Not used here, but `first_stage_mount.cpp` must still compile |
| Cold boot and thread pools | New code that assumes parallel handlers must not break `ColdbootRunnerNoParallel` |

## Checklist for a change in this directory

- [ ] Update the matching page in `docs/`
- [ ] Keep the change small and local; every extra diff is another
      conflict at the next rebase
- [ ] Prefer a guard or an option over deleting upstream code
- [ ] Do not format files you did not change
- [ ] Mention the effect on both normal and recovery boots
