# The mainline repositories

A map of every repository that belongs to the mainline device tree
set, for humans and AI sessions that start in one repo and need the
whole picture.

**TL;DR:** shared code goes in the common repos, SoC vendor code in
`<vendor>-common`, device code in device trees. Read the `AGENTS.md`
of the repo you work in, then the docs it points to.

## Layers

```
 device trees      basic_x86_64_pc, xiaomi/*, apple/snowcastle, virt/*
      |
 family trees      e.g. xiaomi/mi7150-mainline (shared by its devices)
      |
 SoC vendor        device/mainline/<vendor>-common   (now: qcom-common)
      |
 common            device/mainline/common  +  hardware/mainline/common
      |
 platform          AOSP / LineageOS, kernel configs, third party deps
```

`device/mainline/generic` is a separate tree on top of `common`, for
any device whose kernel can boot and whose hardware works through
drivers (`generic_init` finds the partitions, `hardware_detect` the
hardware).

## Core repositories

| Path | Role | Docs |
|------|------|------|
| `device/mainline/common` | Common device tree, `optional/` module switches, sepolicy, `libinit` | `docs/` (bringup guide), `README.md`, `optional/README.md` |
| `hardware/mainline/common` | Mainline HALs (`interfaces/`) and tools; holds the shared code, commit and review standards | `docs/`, per-HAL `README.md` and `AGENTS.md` |
| `vendor/mainline` | Components needing `//vendor:__subpackages__` visibility (`generic_init`, `fbkeyboard`), hwdb | `docs/` (this map, review), `services/generic_init/docs/` |
| `kernel/mainline/configs` | Kernel config fragments and defconfigs | `fragments/*/README` |
| `device/mainline/generic` | One image for many machines, runtime hardware detection | `docs/` |
| `packages/apps/MainlineGenericSystemInstaller` | Installer of the generic tree (app, daemon, scripts) and its disk image | `README.md`, `AGENTS.md` |

## SoC vendor repositories

| Path | Role | Docs |
|------|------|------|
| `device/mainline/qcom-common` | Qualcomm layer: `soc/<family>/`, daemons packaging | `docs/` |
| `hardware/mainline/qcom` | Qualcomm daemons and libraries (`rmtfs`, `qrtr`, `pd-mapper`, `tqftpserv`, `hexagonrpc`, ...) | per component |
| `external/lk2nd` | Qualcomm bootloader | upstream |

More `<vendor>-common` trees may be added; follow the same pattern.

## Extension points (`-ext`)

`device/mainline/common` and `device/mainline/qcom-common` include a
matching `-ext` tree when one exists, so you can add your own extras.
No `-ext` repository is provided in the organization, and the common
trees never depend on one. See
`device/mainline/common/docs/EXTENSIONS.md`.

| Your tree | Extends |
|-----------|---------|
| `device/mainline/common-ext` | `device/mainline/common` |
| `device/mainline/qcom-common-ext` | `device/mainline/qcom-common` |

## Device trees

| Path | What |
|------|------|
| `device/pc/basic_x86_64_pc` | Minimal x86_64 PC tree; boots on hardware and in QEMU |
| `device/xiaomi/mi7150-mainline` | Family tree (SM7150), device dir `davinci_mainline` |
| `device/xiaomi/mi710-mainline` | Family tree (SDM710) |
| `device/xiaomi/pyxis_mainline` | Thin device on `mi710-mainline` |
| `device/xiaomi/mi89xx-mainline` | Family tree (MSM89xx), several devices and kernel forks |
| `device/apple/snowcastle` | Apple devices (HoolockLinux kernels, m1n1) |
| `device/virt/*` | Virtual machines: `virt-common`, `virtio-common`, `vboxware`, `virtio_*` |

## Dependencies (not edited here)

| Group | Repos |
|-------|-------|
| `mainline_external` | Mesa, libdrm, alsa-lib, alsa-ucm-conf, glib, libqmi, libssc, ... under `external/` |
| HAL helpers | `drm_hwcomposer-upstream`, `minigbm-upstream`, `libdisplay-info-upstream`, `linux-firmware-mainline` |
| `mainline_dep_baylibre` | libcamera, libyaml, libyuv_chromium (not forked) |
| `mainline_kernel` | Kernel sources, see `conditional/kernel` in the manifests |
| `mainline_proprietary` | Vendor blobs |

## Where does my change go?

| Change | Repo |
|--------|------|
| One device | That device tree |
| Many devices, any SoC | `device/mainline/common` |
| One SoC vendor | `<vendor>-common` |
| A HAL | `hardware/mainline/common`, selected in `device/mainline/common/optional/` |
| Needs `//vendor:__subpackages__` | `vendor/mainline` |
| Kernel config option | `kernel/mainline/configs` |
| Only you need it | Your own `-ext` tree |

The common repos stay SoC vendor neutral.

## Manifests, branches, review

| Topic | Fact |
|-------|------|
| Hosting | All mainline repositories are in the GitHub organization `me-cafebabe-aosp-mainline` (manifest remote `Mainline`). Dependencies come from their own upstreams |
| Manifests | A `local_manifests` repo with `mainline/*.xml`, groups start with `mainline` |
| Branch | `lineage-24.0` is current; older Android versions have their own branches |
| Review | Pull requests on GitHub, see `docs/review.md` |
| Standards | `hardware/mainline/common/docs/` |

## Where the docs live

| You want | Read |
|----------|------|
| Bring up a device | `device/mainline/common/docs/README.md` |
| Qualcomm specifics | `device/mainline/qcom-common/docs/README.md` |
| Work on the common repos | `device/mainline/common/docs/DEVELOPING.md` |
| Code style, commits, workflow, scope | `hardware/mainline/common/docs/` |
| Add a HAL | `hardware/mainline/common/docs/WIRING_A_HAL.md` |
| Review a change | `vendor/mainline/docs/review.md`, `hardware/mainline/common/docs/REVIEW.md` |
| Generic tree | `device/mainline/generic/docs/` |
| The `generic_init` init program | `vendor/mainline/services/generic_init/docs/README.md` |
| The installer of the generic tree | `packages/apps/MainlineGenericSystemInstaller/README.md` |

## Rules for every session

- Do not build, flash or run tests; the human does.
- Do not search from the AOSP tree root.
- Read the repo's `AGENTS.md` before touching it.
- Keep device specifics in device trees and vendor specifics in vendor trees.
