#!/usr/bin/env bash
# Builds PingDub-x86_64.AppImage: latest Qt 6 (aqtinstall), latest FFmpeg,
# libsoxr and zvbi from source, PingDub itself, the tests, then packages with
# linuxdeploy + linuxdeploy-plugin-qt and the static type 2 AppImage runtime.
#
# CI runs this inside ubuntu:22.04. It runs on a desktop too:
#   bash packaging/build-appimage.sh
# Environment:
#   WORK=dir            build folder (default: build-appimage)
#   USE_SYSTEM_QT=1     use the qmake6 on PATH instead of downloading Qt
#   SKIP_TESTS=1        skip ctest
#   SKIP_DEPS=1         reuse FFmpeg, libsoxr and zvbi already in $WORK/prefix
#   JOBS=n              parallel jobs (default: nproc)
#   UPDATE_INFO=...     zsync update string (default: gh-releases-zsync for NullAngst/PingDub)
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORK=${WORK:-$ROOT/build-appimage}
PREFIX=$WORK/prefix
SRC=$WORK/src
JOBS=${JOBS:-$(nproc)}
UPDATE_INFO=${UPDATE_INFO:-"gh-releases-zsync|NullAngst|PingDub|latest|PingDub-*x86_64.AppImage.zsync"}
export ARCH=x86_64
export PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig:$PREFIX/lib/x86_64-linux-gnu/pkgconfig:${PKG_CONFIG_PATH:-}
export LD_LIBRARY_PATH=$PREFIX/lib:$PREFIX/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}
export PATH=$PREFIX/bin:$PATH
mkdir -p "$PREFIX" "$SRC"

step() { printf '\n=== %s ===\n' "$*"; }

# Newest release tag of a git repo, matching a regex, by version sort.
latest_tag() {
    git ls-remote --tags --refs "$1" | sed 's|.*refs/tags/||' | grep -E "$2" | sort -V | tail -1
}

fetch_tag() {
    local url=$1 tag=$2 dir=$3
    rm -rf "$dir"
    git -c advice.detachedHead=false clone -q --depth 1 --branch "$tag" "$url" "$dir"
}

# --- Build tools ---------------------------------------------------------------
# Current Qt needs a newer CMake than distros like Ubuntu 22.04 ship, so the
# latest CMake and aqtinstall go into a venv here. A venv also keeps pip clear
# of PEP 668 on newer distros.
step "Build tools"
TOOLS=$WORK/tools
[ -x "$TOOLS/bin/python3" ] || python3 -m venv "$TOOLS"
"$TOOLS/bin/python3" -m pip install --quiet --upgrade pip
"$TOOLS/bin/python3" -m pip install --quiet --upgrade cmake aqtinstall
export PATH=$TOOLS/bin:$PATH
hash -r
cmake --version | head -1

# --- 4. Qt ---------------------------------------------------------------------
step "Qt 6"
if [ "${USE_SYSTEM_QT:-0}" = "1" ]; then
    QMAKE=$(command -v qmake6 || command -v qmake)
    QT_DIR=$("$QMAKE" -query QT_INSTALL_PREFIX)
else
    QT_VER=$(python3 -m aqt list-qt linux desktop --spec 6 --latest-version)
    # Qt 6.7 renamed the desktop arch from gcc_64 to linux_gcc_64.
    QT_ARCH=$(python3 -m aqt list-qt linux desktop --arch "$QT_VER" | tr ' ' '\n' | grep -E '^(linux_)?gcc_64$' | head -1)
    printf 'Qt %s (%s)\n' "$QT_VER" "$QT_ARCH"
    python3 -m aqt install-qt linux desktop "$QT_VER" "$QT_ARCH" --outputdir "$WORK/qt"
    QT_DIR=$(find "$WORK/qt/$QT_VER" -mindepth 1 -maxdepth 1 -type d | head -1)
    QMAKE=$QT_DIR/bin/qmake
fi
printf 'Qt prefix: %s\n' "$QT_DIR"
# The Wayland and SVG pieces ship with the base install; fail early if not.
QT_PLUGINS=$("$QMAKE" -query QT_INSTALL_PLUGINS)
ls "$QT_PLUGINS"/platforms/libqwayland*.so >/dev/null 2>&1 || { printf 'Qt has no Wayland platform plugin\n' >&2; exit 1; }
ls "$QT_PLUGINS"/platforms/libqxcb.so >/dev/null
export QMAKE
# linuxdeploy resolves libraries through the loader path, and a downloaded Qt
# sits outside it. Appended so the FFmpeg in $PREFIX still wins over any
# system copy when Qt is the distro's.
QT_LIBS=$("$QMAKE" -query QT_INSTALL_LIBS)
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$QT_LIBS

if [ "${SKIP_DEPS:-0}" != "1" ]; then
# --- 5. FFmpeg ---------------------------------------------------------------
step "FFmpeg"
FF_TAG=$(latest_tag https://github.com/FFmpeg/FFmpeg.git '^n[0-9]+(\.[0-9]+)+$')
printf 'FFmpeg %s\n' "$FF_TAG"
fetch_tag https://github.com/FFmpeg/FFmpeg.git "$FF_TAG" "$SRC/ffmpeg"
(
    cd "$SRC/ffmpeg"
    # LGPL build with only what PingDub uses. ffprobe is built for the tests.
    # The MJPEG encoder writes the contact sheets; the framemd5 muxer is the
    # reference the checksum test compares the .framemd5 sidecars against.
    ./configure --prefix="$PREFIX" --enable-shared --disable-static --disable-everything \
        --disable-programs --enable-ffprobe --disable-doc --disable-autodetect --disable-network --disable-debug \
        --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
        --enable-encoder=ffv1,utvideo,ffvhuff,huffyuv,rawvideo,pcm_s16le,pcm_s24le,pcm_s32le,flac,mjpeg \
        --enable-decoder=ffv1,utvideo,ffvhuff,huffyuv,rawvideo,pcm_s16le,pcm_s24le,pcm_s32le,flac,mjpeg \
        --enable-muxer=matroska,avi,framemd5 --enable-demuxer=matroska,avi --enable-protocol=file
    make -j"$JOBS"
    make install
)

# --- 6. libsoxr and zvbi -------------------------------------------------------
step "libsoxr"
SOXR_URL=https://git.code.sf.net/p/soxr/code
SOXR_TAG=$(latest_tag "$SOXR_URL" '^[0-9]+(\.[0-9]+)+$' 2>/dev/null || true)
if [ -z "$SOXR_TAG" ]; then
    # SourceForge down or unreachable: the maintainer's GitHub mirror has the same tags.
    SOXR_URL=https://github.com/chirlu/soxr.git
    SOXR_TAG=$(latest_tag "$SOXR_URL" '^[0-9]+(\.[0-9]+)+$')
fi
printf 'libsoxr %s from %s\n' "$SOXR_TAG" "$SOXR_URL"
fetch_tag "$SOXR_URL" "$SOXR_TAG" "$SRC/soxr"
cmake -S "$SRC/soxr" -B "$SRC/soxr/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF -DWITH_OPENMP=OFF -DWITH_LSR_BINDINGS=OFF \
    -DBUILD_SHARED_LIBS=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build "$SRC/soxr/build"
cmake --install "$SRC/soxr/build"

step "zvbi"
ZVBI_TAG=$(latest_tag https://github.com/zapping-vbi/zvbi.git '^v[0-9]+(\.[0-9]+)+$')
printf 'zvbi %s\n' "$ZVBI_TAG"
fetch_tag https://github.com/zapping-vbi/zvbi.git "$ZVBI_TAG" "$SRC/zvbi"
(
    cd "$SRC/zvbi"
    ./autogen.sh >/dev/null 2>&1 || autoreconf -fi
    ./configure --prefix="$PREFIX" --disable-static --without-x --disable-dvb --disable-proxy --disable-nls \
        --without-doxygen --disable-examples --disable-tests >/dev/null
    make -j"$JOBS" -C src
    make -C src install
    make install-pkgconfigDATA 2>/dev/null || install -m644 zvbi-0.2.pc "$PREFIX/lib/pkgconfig/"
)
fi

# --- 7. PingDub ------------------------------------------------------------------
step "PingDub"
BUILD=$WORK/build
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$QT_DIR;$PREFIX" -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" \
    -DPINGDUB_LTO=ON -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$BUILD" -j"$JOBS"

# --- 8. Tests --------------------------------------------------------------------
if [ "${SKIP_TESTS:-0}" != "1" ]; then
    step "Tests"
    (cd "$BUILD" && PD_FFPROBE="$PREFIX/bin/ffprobe" ctest --output-on-failure -j"$JOBS")
fi

# --- 9. AppDir with linuxdeploy ---------------------------------------------------
step "AppDir"
TOOLS=$WORK/tools
mkdir -p "$TOOLS"
for t in linuxdeploy/linuxdeploy/linuxdeploy-x86_64.AppImage \
         linuxdeploy/linuxdeploy-plugin-qt/linuxdeploy-plugin-qt-x86_64.AppImage \
         AppImage/appimagetool/appimagetool-x86_64.AppImage; do
    f=$TOOLS/${t##*/}
    [ -s "$f" ] || curl -fsSL -o "$f" "https://github.com/${t%/*}/releases/download/continuous/${t##*/}"
    chmod +x "$f"
done
# Containers have no FUSE: run the tool AppImages extracted.
export APPIMAGE_EXTRACT_AND_RUN=1
APPDIR=$WORK/AppDir
rm -rf "$APPDIR"
DESTDIR=$APPDIR cmake --install "$BUILD"
# Wayland and X11 platform plugins both go in. The list comes from what this Qt
# ships: Qt 6.10 merged libqwayland-egl.so and libqwayland-generic.so into one
# libqwayland.so, and older Qt still has the split pair.
PLATFORMS=$(cd "$QT_PLUGINS/platforms" && printf '%s\n' libqwayland*.so libqxcb.so | paste -sd ';')
printf 'Platform plugins: %s\n' "$PLATFORMS"
export EXTRA_PLATFORM_PLUGINS=$PLATFORMS
export EXTRA_QT_MODULES="svg"
# Libraries that must come from the host: a bundled alsa-lib cannot load the
# host's ALSA plugins (pipewire PCM, .asoundrc); the rest are tied to the
# host's drivers, D-Bus and udev.
# The Qt plugin runs its own deployment pass; it reads the same list from the environment.
export LINUXDEPLOY_EXCLUDED_LIBRARIES="libasound.so*;libudev.so*;libGL.so*;libEGL.so*;libdbus-1.so*;libGLX*;libOpenGL.so*;libGLdispatch.so*"
EXCLUDES=(--exclude-library 'libasound.so*' --exclude-library 'libudev.so*' --exclude-library 'libGL.so*'
          --exclude-library 'libEGL.so*' --exclude-library 'libdbus-1.so*' --exclude-library 'libGLX*'
          --exclude-library 'libOpenGL.so*' --exclude-library 'libGLdispatch.so*')
"$TOOLS/linuxdeploy-x86_64.AppImage" --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/pingdub" \
    --desktop-file "$APPDIR/usr/share/applications/io.github.nullangst.PingDub.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/io.github.nullangst.PingDub.svg" \
    "${EXCLUDES[@]}" \
    --plugin qt

# The Wayland platform plugin is useless without its shell, decoration and
# graphics integration plugins. linuxdeploy-plugin-qt normally brings them;
# copy any it skipped. Their RPATH ($ORIGIN/../../lib) already fits the AppDir,
# and their Qt dependencies came in with libqwayland.
for d in wayland-shell-integration wayland-decoration-client wayland-graphics-integration-client; do
    if [ -d "$QT_PLUGINS/$d" ] && [ ! -d "$APPDIR/usr/plugins/$d" ]; then
        printf 'Adding missing Qt plugin folder %s\n' "$d"
        mkdir -p "$APPDIR/usr/plugins"
        cp -a "$QT_PLUGINS/$d" "$APPDIR/usr/plugins/"
    fi
done
ls "$APPDIR"/usr/plugins/platforms/

# Fail if a host-only library slipped in.
BAD=$(find "$APPDIR" \( -name 'libasound.so*' -o -name 'libudev.so*' -o -name 'libGL.so*' -o -name 'libEGL.so*' \
    -o -name 'libdbus-1.so*' -o -name 'libc.so*' -o -name 'libc-*.so' -o -name 'ld-linux*' \) -print)
if [ -n "$BAD" ]; then
    printf 'Host-only libraries found in the AppDir:\n%s\n' "$BAD" >&2
    exit 1
fi
printf 'AppDir check passed: no libasound, libudev, libGL, libEGL, libdbus-1 or libc inside.\n'

# --- 10. and 11. AppImage with the static runtime and update info --------------------
step "AppImage"
VERSION=$(git -C "$ROOT" describe --tags --always 2>/dev/null | sed 's/^v//' || printf 'dev')
export VERSION
OUT=$ROOT/PingDub-x86_64.AppImage
rm -f "$OUT" "$OUT.zsync"
# appimagetool from AppImage/appimagetool uses the static type 2 runtime, so
# the host needs no libfuse2.
(cd "$ROOT" && "$TOOLS/appimagetool-x86_64.AppImage" -u "$UPDATE_INFO" "$APPDIR" "$OUT")
[ -f "$OUT.zsync" ] || [ -f "$ROOT/PingDub-x86_64.AppImage.zsync" ] || { command -v zsyncmake >/dev/null && (cd "$ROOT" && zsyncmake -u PingDub-x86_64.AppImage PingDub-x86_64.AppImage); }
ls -la "$OUT"*
printf '\nDone: %s\n' "$OUT"
