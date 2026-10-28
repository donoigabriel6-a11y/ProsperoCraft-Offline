#!/usr/bin/env bash
# Build ProsperoCraft using the official build script and create exFAT image
# This wraps tools/build.sh (which does sync + make_jvm_title.sh) then packages as exFAT
#
# Usage:
#   bash build_full_exfat.sh <PC_IP> <PS5_IP> [--size SIZE_MB] [--output FILE] [-- build-options...]
#
# Example:
#   MSA_CLIENT_ID=xxx bash build_full_exfat.sh 192.168.1.100 192.168.1.200 -- jit gl mc launcher sdk100 nativeui

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT="$SCRIPT_DIR"

# Parse arguments
PC_IP=""
PS5_IP=""
IMAGE_SIZE_MB=512
OUTPUT_FILE=""
BUILD_OPTS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --size) IMAGE_SIZE_MB="$2"; shift 2 ;;
        --output) OUTPUT_FILE="$2"; shift 2 ;;
        --) shift; BUILD_OPTS=("$@"); break ;;
        *)
            if [[ -z "$PC_IP" ]]; then
                PC_IP="$1"
            elif [[ -z "$PS5_IP" ]]; then
                PS5_IP="$1"
            else
                echo "Unknown argument: $1" >&2
                exit 1
            fi
            shift
            ;;
    esac
done

if [[ -z "$PC_IP" || -z "$PS5_IP" ]]; then
    echo "Usage: $0 <PC_IP> <PS5_IP> [--size SIZE_MB] [--output FILE] [-- build-options...]"
    echo ""
    echo "Example:"
    echo "  MSA_CLIENT_ID=xxxx bash $0 192.168.1.100 192.168.1.200 -- jit gl mc launcher sdk100 nativeui"
    exit 1
fi

# Default build options
if [[ ${#BUILD_OPTS[@]} -eq 0 ]]; then
    BUILD_OPTS=(jit gl mc launcher sdk100 nativeui)
fi

echo "=== Full Build + exFAT Image ==="
echo "PC IP: $PC_IP"
echo "PS5 IP: $PS5_IP"
echo "Build options: ${BUILD_OPTS[*]}"
echo "Image size: ${IMAGE_SIZE_MB}MB"
echo ""

# Step 1: Run the official build script (sync + build + deploy)
echo "=== Step 1: Running tools/build.sh ==="
MSA_CLIENT_ID="${MSA_CLIENT_ID:-}" bash "$REPO_ROOT/tools/build.sh" "$PC_IP" "$PS5_IP" -- "${BUILD_OPTS[@]}"

# Step 2: Find the .pkg file and create exFAT image
# The build deploys to PS5, but also leaves artifacts in ~/ps5probe/dist/
TITLE_DIR="$REPO_ROOT/title"
TITLE_ID=$(python3 -c "
import json
with open('$TITLE_DIR/sce_sys/param.json') as f:
    print(json.load(f)['titleId'])
")
VERSION=$(python3 -c "
import json
with open('$TITLE_DIR/sce_sys/param.json') as f:
    print(json.load(f)['contentVersion'])
")
APP_NAME=$(python3 -c "
import json
with open('$TITLE_DIR/sce_sys/param.json') as f:
    print(json.load(f)['localizedParameters']['en-US']['titleName'])
")

if [[ -z "$OUTPUT_FILE" ]]; then
    OUTPUT_FILE="${APP_NAME}-${VERSION}.img"
fi

BUILD_DIR="${BUILD_DIR:-$HOME/ps5-mc-java}"
PKG_FILE="$BUILD_DIR/dist/${TITLE_ID}.ffpkg"

if [[ ! -f "$PKG_FILE" ]]; then
    # Check alternative locations
    for f in "$HOME/ps5probe/dist/${TITLE_ID}.ffpkg" "$REPO_ROOT/title/dist/${TITLE_ID}.ffpkg"; do
        if [[ -f "$f" ]]; then
            PKG_FILE="$f"
            break
        fi
    done
fi

if [[ ! -f "$PKG_FILE" ]]; then
    echo "Error: .pkg file not found. Searched:"
    echo "  $BUILD_DIR/dist/${TITLE_ID}.ffpkg"
    echo "  $HOME/ps5probe/dist/${TITLE_ID}.ffpkg"
    echo "  $REPO_ROOT/title/dist/${TITLE_ID}.ffpkg"
    exit 1
fi

echo ""
echo "=== Step 2: Creating exFAT image ==="
echo "Found .pkg: $PKG_FILE"

bash "$REPO_ROOT/build_exfat.sh" --skip-build --size "$IMAGE_SIZE_MB" --output "$OUTPUT_FILE"

echo ""
echo "=== Complete! ==="
echo "Deployed to PS5 at $PS5_IP"
echo "exFAT image: $OUTPUT_FILE"