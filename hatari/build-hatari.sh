#!/bin/bash
# Build Hatari with SidecarTridge Multi-device support (EmuMD).
# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-2.0-or-later
#
#   build-hatari.sh [hatari source folder]
#
# EmuMD's fork of Hatari (Hatari 2.6.1 with Multi-device support, on
# its emumd branch) is fetched at the commit below into a per-user cache
# ($EMUMD_CACHE, else ~/.cache/emumd/hatari-fork), shared by every
# project and kept out of EmuMD itself (often a submodule). It is checked
# out again whenever that commit changes; later runs just rebuild.
# (~/.cache/emumd/hatari is older EmuMDs' patched Hatari, left to them.)
# EmuTOS 1.4 (256k, every language) is downloaded alongside, into the
# cache's emutos-256k-1.4 folder. Prints the path of the hatari binary
# last.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
CACHE=${EMUMD_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/emumd}
SRC=${1:-$CACHE/hatari-fork}
HATARI_GIT=${HATARI_GIT:-https://github.com/neilrackett/hatari.git}
# The fork's emumd branch, as this version of EmuMD was made for.
HATARI_COMMIT=${HATARI_COMMIT:-0d8586bc64395136816e8d7b9c565ba616600ed8}
EMUTOS_VERSION=${EMUTOS_VERSION:-1.4}
EMUTOS="$CACHE/emutos-256k-$EMUTOS_VERSION"

sha1() { if command -v shasum >/dev/null; then shasum -a 1; else sha1sum; fi | cut -d' ' -f1; }

# Hatari's sources, at that commit.
if [ ! -d "$SRC/.git" ]; then
    mkdir -p "$SRC"
    git -C "$SRC" init -q
fi
if [ "$(git -C "$SRC" rev-parse -q --verify 'HEAD^{commit}' || true)" != "$HATARI_COMMIT" ]; then
    git -C "$SRC" fetch -q --depth 1 "$HATARI_GIT" "$HATARI_COMMIT" >&2
    git -C "$SRC" checkout -q -f --detach FETCH_HEAD >&2
    git -C "$SRC" clean -fdq -- src >&2
fi

# Both sides include emumd_plugin.h: Hatari's copy must be this one.
if ! cmp -s "$ROOT/include/emumd_plugin.h" "$SRC/src/includes/emumd_plugin.h"; then
    echo "build-hatari.sh: Hatari's src/includes/emumd_plugin.h differs from" \
         "EmuMD's include/emumd_plugin.h; make them the same, in the fork too" >&2
    exit 1
fi

# A free TOS to boot it with; unpacked aside first, so an interrupted
# download is tried again next time.
if [ ! -d "$EMUTOS" ]; then
    mkdir -p "$CACHE"
    TMP=$(mktemp -d "$CACHE/.emutos.XXXXXX")
    trap 'rm -rf "$TMP"' EXIT
    echo "Downloading EmuTOS $EMUTOS_VERSION" >&2
    curl -fsSL -o "$TMP/emutos.zip" \
        "https://downloads.sourceforge.net/project/emutos/emutos/$EMUTOS_VERSION/emutos-256k-$EMUTOS_VERSION.zip"
    python3 -m zipfile -e "$TMP/emutos.zip" "$TMP"
    mv "$TMP/emutos-256k-$EMUTOS_VERSION" "$EMUTOS"
fi

# On a Mac with Xcode (whose ibtool builds Hatari's menus), Hatari is an
# app bundle with its menu bar; otherwise a plain program, used through its
# shortcuts.
ARCH_ARGS=()
BUNDLE=0
if [ "$(uname -s)" = "Darwin" ]; then
    ARCH_ARGS=(-DCMAKE_OSX_ARCHITECTURES="$(uname -m)")
    if xcrun --find ibtool >/dev/null 2>&1; then
        BUNDLE=1
    fi
fi
cmake -S "$SRC" -B "$SRC/build" -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_OSX_BUNDLE=$BUNDLE ${ARCH_ARGS[@]+"${ARCH_ARGS[@]}"} >&2
cmake --build "$SRC/build" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" >&2

# Only the Hatari just built, so mdfw can't run one left from before. An
# app bundle gets a plain hatari beside it as well, a launcher for the
# bundle, because EmuMDs from before the bundle look only there, and
# projects pin those. (A script, not a link, so that the bundle is run by
# its own path and finds its resources.)
if [ $BUNDLE = 1 ]; then
    HATARI="$SRC/build/src/Hatari.app/Contents/MacOS/Hatari"
    printf '%s\n' '#!/bin/sh' \
        'exec "$(dirname "$0")/Hatari.app/Contents/MacOS/Hatari" "$@"' \
        > "$SRC/build/src/hatari"
    chmod +x "$SRC/build/src/hatari"
else
    HATARI="$SRC/build/src/hatari"
    rm -rf "$SRC/build/src/Hatari.app"
fi

# What it was built from, for mdfw run to compare (same files, same order).
cat "$HERE/build-hatari.sh" "$ROOT/include/emumd_plugin.h" | sha1 > "$SRC/.emumd-stamp"
echo "EmuTOS $EMUTOS_VERSION: $EMUTOS/etos256uk.img (and other languages)" >&2
echo "$HATARI"
