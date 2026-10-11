# OpenBIOS for AutoBleem

This repository is a fork of [pcsx-redux/nugget](https://github.com/pcsx-redux/nugget), where OpenBIOS lives.
AutoBleem bundles the BIOS it builds with every package except the PlayStation Classic's: the PlayStation
emulator uses it when the player has no BIOS of their own, in place of the emulator's built-in (HLE) one.

## Branches

- `main` - upstream's `main`, unchanged.
- `develop` - the default branch: upstream plus the AutoBleem changes below. CI builds it into the rolling
  `nightly` pre-release; a `v*` tag is a release.

## What differs from upstream

Only the shell (the boot screen OpenBIOS shows before a game):

- `shell/ab_main.c` - the shell's `main()` with the AutoBleem boot screen; `shell/main.c` stays as upstream has it.
- `shell/ab_splash.c`, `shell/ab_splash.h` - draws the screen: a gouraud background, light rays, palette
  layers moved by keyframed timelines, OpenBIOS's own cube beside "powered by OpenBIOS".
- `shell/ab_assets/` - the design: the layers (PNG), `layers.json`, `keyframes.csv`, `autobleem-blip.mod`.
- `shell/ab_assets.c` - written by `python shell/ab_mkassets.py` (needs Pillow) from `shell/ab_assets/`.
- `shell/blip.hit` - the tune, written by `python shell/ab_modconv.py shell/ab_assets/autobleem-blip.mod shell/blip.hit`.
- `shell/Makefile` - `ab_main.c`, `ab_splash.c` and `ab_assets.c` in place of `main.c` and `hull.c`.
- `VERSION`, `ci/build.sh`, `.github/workflows/autobleem.yml`, this file.

Building with `AB_SPLASH_DEMO` defined in `shell/ab_main.c` walks every state of the screen (idle, the
"no disc" look, idle again, then the disc) whatever the drive says - for looking at it in an emulator.

## The base

Upstream `main` merged last: `8a129f0498131ecefb27f5eb5eee30d016d0fe45` (2026-10-11).

## Merging upstream (once a month)

    git fetch upstream
    git switch main && git merge --ff-only upstream/main && git push origin main
    git switch develop && git merge --no-ff main

Conflicts can only come from `shell/Makefile` (keep our source list) and `shell/blip.hit` (keep ours). Then:

1. build (`ci/build.sh` in the image named there) and boot a game with the new `openbios.bin` in pcsx-abnxt -
   the boot screen, the game starting, and `AB_SPLASH_DEMO` for the other states;
2. raise the minor version in `VERSION`, update "The base" above;
3. push develop (CI makes the nightly), tag `v<VERSION>` for a release.

Upstream has no versions of its own; ours start at 1.0.0 and count our releases.
