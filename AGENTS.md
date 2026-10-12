# AGENTS.md - vendor/mainline

Agents must read this file before touching anything in this repository.

This repository is the home of the **map of the whole mainline repository
set**: read `docs/REPOSITORIES.md` to see how all the mainline repos
relate and where each kind of change goes.

## Read first

| Task | Read |
|------|------|
| Which repo, which docs | `docs/REPOSITORIES.md` |
| Review a change | `docs/review.md` |
| Work on `generic_init` | `services/generic_init/docs/README.md` |
| Standards, workflow | `hardware/mainline/common/docs/` |

## Hard rules

- No Android build, flashing or test without asking the human first
  (`hardware/mainline/common/docs/WORKFLOW.md`).
- Do not search from the AOSP tree root.
- Only add components here that need `//vendor:__subpackages__`
  visibility; otherwise use `device/mainline/common`.
- Keep `docs/REPOSITORIES.md` in sync when a repo is added or removed
  in the mainline `local_manifests`.
- Every new source file starts with the SPDX header.
