#!/usr/bin/env bash
# Copies the organized repo into the flat build folder that make_jvm_title.sh expects (default ~/ps5-mc-java).
# It only adds / overwrites files (never deletes), so caches, backgrounds and other local assets in the build folder stay.
#   bash tools/sync.sh [--build-dir DIR] [--assets DIR]
#   --build-dir DIR   build folder (default ~/ps5-mc-java, or $BUILD_DIR)
#   --assets DIR      your own brand files: icon0.png pic0.png pic0.dds pic1.dds pic2.dds snd0.at9 (not in the repo; default ~/ps5-assets, or $ASSETS)
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-$HOME/ps5-mc-java}
ASSETS=${ASSETS:-$HOME/ps5-assets}
while [ $# -gt 0 ]; do case "$1" in
  --build-dir) BUILD_DIR=$2; shift 2;; --assets) ASSETS=$2; shift 2;;
  *) echo "unknown option $1 (see the header of tools/sync.sh)"; exit 1;; esac; done
mkdir -p "$BUILD_DIR/brand" "$BUILD_DIR/kit_launcher"
flat() { for d in "$@"; do [ -d "$REPO/$d" ] && find "$REPO/$d" -maxdepth 1 -type f -exec cp -f {} "$BUILD_DIR/" \; ; done; true; }
flat src/shim src/host src/java src/java/diag src/awtlite config tools/build tools/debug
mkdir -p "$BUILD_DIR/awtlite" "$BUILD_DIR/l2src" "$BUILD_DIR/tests"
cp -rf "$REPO/src/awtlite/classes/." "$BUILD_DIR/awtlite/"
cp -rf "$REPO/src/lwjgl2/l2src/." "$BUILD_DIR/l2src/"
cp -rf "$REPO/launcher/." "$BUILD_DIR/kit_launcher/"
cp -rf "$REPO/tests/." "$BUILD_DIR/tests/"
for f in icon0.png pic0.png pic0.dds pic1.dds pic2.dds snd0.at9; do
  if [ -f "$ASSETS/$f" ]; then cp -f "$ASSETS/$f" "$BUILD_DIR/brand/"; elif [ ! -f "$BUILD_DIR/brand/$f" ]; then echo "note: brand/$f not found in $ASSETS (the title keeps its default)"; fi
done
mkdir -p "$HOME/ps5probe" && cp -rf "$REPO/title/." "$HOME/ps5probe/"   # title boilerplate (identity in title/sce_sys/param.json); only adds / overwrites, generated files and your artwork stay
echo "synced $REPO -> $BUILD_DIR"
