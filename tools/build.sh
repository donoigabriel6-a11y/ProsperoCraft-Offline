#!/usr/bin/env bash
# One command: sync the repo into the build folder and build + deploy the title.
#   MSA_CLIENT_ID=<your Azure app id> bash tools/build.sh <PC ip> <PS5 ip> [make_jvm_title.sh options]
# Default options: jit gl mc launcher sdk100 nativeui vk
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-$HOME/ps5-mc-java}
[ $# -ge 2 ] || { echo "usage: MSA_CLIENT_ID=<id> bash tools/build.sh <PC ip> <PS5 ip> [options]"; exit 1; }
PC=$1; PS5=$2; shift 2
OPTS=${*:-jit gl mc launcher sdk100 nativeui vk}
bash "$REPO/tools/sync.sh"
rm -f "$HOME/ps5probe/build/obj/src/jvm_host.cpp.o"   # force the host to rebuild
cd "$BUILD_DIR"
exec bash make_jvm_title.sh "$PC" "$PS5" $OPTS
