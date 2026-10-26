#!/usr/bin/env bash
# One command: sync the repo into the build folder and build + deploy the title.
#   MSA_CLIENT_ID=<your Azure app id> bash tools/build.sh <PC ip> <PS5 ip> [--build-dir DIR] [--assets DIR] [-- make_jvm_title.sh options]
# <PC ip> receives the log (UDP 9999), <PS5 ip> is the console the title is deployed to. Default options: jit gl mc launcher sdk100 nativeui vk
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
[ $# -ge 2 ] || { echo "usage: MSA_CLIENT_ID=<id> bash tools/build.sh <PC ip> <PS5 ip> [--build-dir DIR] [--assets DIR] [-- options]"; exit 1; }
PC=$1; PS5=$2; shift 2
SYNC=(); OPTS=()
while [ $# -gt 0 ]; do case "$1" in
  --build-dir) BUILD_DIR=$2; SYNC+=(--build-dir "$2"); shift 2;; --assets) SYNC+=(--assets "$2"); shift 2;;
  --) shift; OPTS=("$@"); break;; *) echo "unknown option $1"; exit 1;; esac; done
[ ${#OPTS[@]} -gt 0 ] || OPTS=(jit gl mc launcher sdk100 nativeui vk)
BUILD_DIR=${BUILD_DIR:-$HOME/ps5-mc-java}
bash "$REPO/tools/sync.sh" "${SYNC[@]}"
rm -f "$HOME/ps5probe/build/obj/src/jvm_host.cpp.o"   # force the host to rebuild
cd "$BUILD_DIR"
exec bash make_jvm_title.sh "$PC" "$PS5" "${OPTS[@]}"
