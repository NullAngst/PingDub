# PingDub AppImage icon fix

Fixes the CI failure `No valid .DirIcon` that started on 2026-10-29.

## Why it broke

appimagetool's continuous build changed on 2026-10-29 (upstream commit b082d2e).
It now builds `.DirIcon` only from a PNG or XPM named after the desktop file's
`Icon=` entry, ignores SVG, and refuses to package without one. PingDub only
shipped `pingdub.svg`, so packaging failed even though nothing in PingDub changed.

## What changed

- `packaging/build-appimage.sh`
  - Checks for `git`, `curl`, `python3` and `rsvg-convert` at the start, so a
    local run fails right away instead of after Qt and FFmpeg are built.
  - After linuxdeploy, renders `pingdub.svg` to a 256x256 PNG with
    `rsvg-convert`, puts it in `usr/share/icons/hicolor/256x256/apps/`, copies
    it to the AppDir root, and points `.DirIcon` at it.
- `.github/workflows/build.yml`
  - Installs `librsvg2-bin`, which provides `rsvg-convert` on Ubuntu.

The SVG stays the single source. The PNG is generated every build, so it
cannot drift.

## How to apply

1. Extract over your PingDub checkout: `tar -xf PingDub-icon-fix.tar --strip-components=1 -C /path/to/PingDub`
2. Commit and push. The workflow runs as usual.

For local builds, install rsvg-convert first. I'd use `sudo apt install librsvg2-bin`
on Kubuntu. It's `librsvg2-tools` on Fedora and openSUSE, `librsvg` on Arch,
or whichever package your distro ships it in.
