#!/usr/bin/env bash
# All-in-one build script for ProsperoCraft on WSL/Linux
# Run this in WSL (Ubuntu 22.04+) - it installs everything and builds the .pkg
#
# Usage:
#   bash build_all_in_one.sh [options]
#
# Options:
#   --skip-deps        Skip dependency installation (assumes already set up)
#   --skip-jdks        Skip JDK builds (assumes already built)
#   --size SIZE_MB     exFAT image size (default: 512)
#   --output FILE      Output image name (default: ProsperoCraft-<version>.img)
#   --no-exfat         Don't create exFAT image, just build .pkg
#   --ps5-ip IP        PS5 IP for deploy (default: 127.0.0.1 - no deploy)
#   --msa-client-id ID Microsoft client ID (or set MSA_CLIENT_ID env var)
#   --clean            Clean build (removes previous build artifacts)
#   --help             Show this help

set -euo pipefail

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info()  { echo -e "${BLUE}[INFO]${NC} $*"; }
log_ok()    { echo -e "${GREEN}[OK]${NC} $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*"; }

# Defaults
SKIP_DEPS=false
SKIP_JDKS=false
IMAGE_SIZE_MB=512
OUTPUT_FILE=""
CREATE_EXFAT=true
PS5_IP="127.0.0.1"
MSA_CLIENT_ID="${MSA_CLIENT_ID:-}"
CLEAN_BUILD=false
BUILD_OPTS=(jit gl mc launcher sdk100 nativeui)

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-deps) SKIP_DEPS=true; shift ;;
        --skip-jdks) SKIP_JDKS=true; shift ;;
        --size) IMAGE_SIZE_MB="$2"; shift 2 ;;
        --output) OUTPUT_FILE="$2"; shift 2 ;;
        --no-exfat) CREATE_EXFAT=false; shift ;;
        --ps5-ip) PS5_IP="$2"; shift 2 ;;
        --msa-client-id) MSA_CLIENT_ID="$2"; shift 2 ;;
        --clean) CLEAN_BUILD=true; shift ;;
        --help|-h)
            sed -n '2,25p' "$0" | sed 's/^# //'
            exit 0
            ;;
        *) log_error "Unknown option: $1"; exit 1 ;;
    esac
done

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT="$SCRIPT_DIR"
TITLE_DIR="$REPO_ROOT/title"
BUILD_DIR="${BUILD_DIR:-$HOME/ps5-mc-java}"

# Get version info
VERSION=$(python3 -c "
import json
with open('$TITLE_DIR/sce_sys/param.json') as f:
    print(json.load(f)['contentVersion'])
")
TITLE_ID=$(python3 -c "
import json
with open('$TITLE_DIR/sce_sys/param.json') as f:
    print(json.load(f)['titleId'])
")
APP_NAME=$(python3 -c "
import json
with open('$TITLE_DIR/sce_sys/param.json') as f:
    print(json.load(f)['localizedParameters']['en-US']['titleName'])
")

if [[ -z "$OUTPUT_FILE" ]]; then
    OUTPUT_FILE="${APP_NAME}-${VERSION}.img"
fi

echo "=========================================="
echo "  ProsperoCraft All-in-One Build Script"
echo "=========================================="
echo "Version: $VERSION"
echo "Title ID: $TITLE_ID"
echo "Build dir: $BUILD_DIR"
echo "PS5 IP: $PS5_IP"
echo "Image size: ${IMAGE_SIZE_MB}MB"
echo "Create exFAT: $CREATE_EXFAT"
echo "Clean build: $CLEAN_BUILD"
echo ""

# Check WSL/Linux
if ! grep -qi microsoft /proc/version 2>/dev/null && [[ "$(uname -s)" != "Linux" ]]; then
    log_error "This script must run in WSL or Linux"
    exit 1
fi
log_ok "Running on $(uname -s)"

# Check for required commands
check_cmd() {
    if ! command -v "$1" >/dev/null; then
        log_error "Missing command: $1"
        return 1
    fi
    return 0
}

# Step 1: Install system dependencies
if [[ "$SKIP_DEPS" == false ]]; then
    log_info "Step 1/7: Installing system dependencies..."

    # Update and install packages
    export DEBIAN_FRONTEND=noninteractive
    sudo apt-get update -qq
    sudo apt-get install -y -qq \
        git curl wget python3 python3-venv python3-pip \
        build-essential clang-18 lld-18 llvm-18 \
        cmake ninja-build pkg-config \
        libssl-dev libffi-dev zlib1g-dev \
        openjdk-17-jdk-headless \
        exfatprogs dosfstools mtools \
        ccache patchelf \
        jq 2>&1 | tail -5

    # Set clang-18 as default
    sudo update-alternatives --install /usr/bin/clang clang /usr/bin/clang-18 100 2>/dev/null || true
    sudo update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-18 100 2>/dev/null || true
    sudo update-alternatives --install /usr/bin/ld.lld ld.lld /usr/bin/ld.lld-18 100 2>/dev/null || true

    log_ok "System dependencies installed"
else
    log_info "Step 1/7: Skipping system dependencies (--skip-deps)"
fi

# Step 2: PS5 Payload SDK
if [[ "$SKIP_DEPS" == false ]]; then
    log_info "Step 2/7: Setting up PS5 Payload SDK..."

    SDK_DIR="$HOME/.deps/native/ps5-payload-sdk"
    if [[ ! -d "$SDK_DIR/.git" ]]; then
        mkdir -p "$SDK_DIR"
        git -C "$SDK_DIR" init --quiet
        git -C "$SDK_DIR" remote add origin https://github.com/ps5-payload-dev/sdk.git
        # Try master first (default for this repo), fallback to main
        git -C "$SDK_DIR" fetch --quiet --depth 1 origin master 2>/dev/null \
            || git -C "$SDK_DIR" fetch --quiet --depth 1 origin main
        git -C "$SDK_DIR" checkout --quiet --detach FETCH_HEAD
    fi

    if [[ ! -f "$SDK_DIR/target/bin/clang" ]]; then
        log_info "Building PS5 SDK (this takes a few minutes)..."
        cd "$SDK_DIR"
        # Try different build script names/locations
        if [[ -f "./build.sh" ]]; then
            ./build.sh 2>&1 | tail -10
        elif [[ -f "./build_sdk.sh" ]]; then
            ./build_sdk.sh 2>&1 | tail -10
        elif [[ -f "./tools/build.sh" ]]; then
            ./tools/build.sh 2>&1 | tail -10
        elif [[ -f "./scripts/build.sh" ]]; then
            ./scripts/build.sh 2>&1 | tail -10
        else
            log_error "No build script found in PS5 SDK"
            log_info "Available files: $(ls -la)"
            exit 1
        fi
    fi

    log_ok "PS5 SDK ready at $SDK_DIR"
else
    log_info "Step 2/7: Skipping PS5 SDK (--skip-deps)"
fi

# Step 3: OpenGL SDK
if [[ "$SKIP_DEPS" == false ]]; then
    log_info "Step 3/7: Setting up OpenGL SDK 1.0.2..."

    GLSDK_DIR="$HOME/glsdk/ps5-opengl-sdk-1.0.2/sdk"
    if [[ ! -f "$GLSDK_DIR/lib/libps5_opengl_core33.a" ]]; then
        mkdir -p "$HOME/glsdk"
        cd "$HOME/glsdk"
        curl -fL -o ps5-opengl-sdk-1.0.2.tar.gz \
            https://github.com/blackbearreloaded/ps5-opengl/releases/download/v1.0.2/ps5-opengl-sdk-1.0.2.tar.gz
        tar xzf ps5-opengl-sdk-1.0.2.tar.gz
        rm ps5-opengl-sdk-1.0.2.tar.gz
    fi

    log_ok "OpenGL SDK ready at $GLSDK_DIR"
else
    log_info "Step 3/7: Skipping OpenGL SDK (--skip-deps)"
fi

# Step 4: Build PS5 JDKs
if [[ "$SKIP_JDKS" == false ]]; then
    log_info "Step 4/7: Building PS5 JDKs (this takes 20-40 minutes)..."

    JDK_DIRS=(
        "$HOME/ps5jvm"
        "$HOME/ps5jvm21"
        "$HOME/ps5jvm25"
    )

    NEED_JDK=false
    for d in "${JDK_DIRS[@]}"; do
        if [[ ! -f "$d/whole-hotspot.a" ]]; then
            NEED_JDK=true
            break
        fi
    done

    if [[ "$NEED_JDK" == true ]]; then
        # Check if build_jdk.sh exists (first in repo, then in PS5 SDK, then download)
        BUILD_JDK_SCRIPT=""
        if [[ -f "$REPO_ROOT/tools/build_jdk.sh" ]]; then
            BUILD_JDK_SCRIPT="$REPO_ROOT/tools/build_jdk.sh"
        elif [[ -f "$SDK_DIR/tools/build_jdk.sh" ]]; then
            BUILD_JDK_SCRIPT="$SDK_DIR/tools/build_jdk.sh"
        else
            log_warn "build_jdk.sh not found locally, downloading from PS5 SDK..."
            curl -fL -o "$REPO_ROOT/tools/build_jdk.sh" \
                https://raw.githubusercontent.com/ps5-payload-dev/sdk/master/tools/build_jdk.sh
            chmod +x "$REPO_ROOT/tools/build_jdk.sh"
            BUILD_JDK_SCRIPT="$REPO_ROOT/tools/build_jdk.sh"
        fi

        for v in 17 21 25; do
            TARGET_DIR="$HOME/ps5jvm$([[ $v == 17 ]] && echo "" || echo "$v")"
            if [[ ! -f "$TARGET_DIR/whole-hotspot.a" ]]; then
                log_info "Building JDK $v..."
                JDKV=$v bash "$REPO_ROOT/tools/build_jdk.sh" 2>&1 | tail -15
            else
                log_ok "JDK $v already built"
            fi
        done
    else
        log_ok "All JDKs already built"
    fi
else
    log_info "Step 4/7: Skipping JDK builds (--skip-jdks)"
fi

# Step 5: Sync sources
log_info "Step 5/7: Syncing sources to build directory..."
bash "$REPO_ROOT/tools/sync.sh" --build-dir "$BUILD_DIR"
log_ok "Sources synced to $BUILD_DIR"

# Step 6: Clean if requested
if [[ "$CLEAN_BUILD" == true ]]; then
    log_info "Cleaning previous build..."
    cd "$BUILD_DIR" && make clean 2>/dev/null || true
    rm -rf "$BUILD_DIR/dist" "$BUILD_DIR/build"
    log_ok "Clean done"
fi

# Step 7: Build the project
log_info "Step 6/7: Building ProsperoCraft (.pkg)..."
cd "$BUILD_DIR"

export MSA_CLIENT_ID="$MSA_CLIENT_ID"
export BUILD_JOBS=$(nproc)
export USE_CCACHE=1

# Build command
BUILD_CMD=(
    bash "$REPO_ROOT/tools/build.sh"
    "$PS5_IP" "$PS5_IP"
    --build-dir "$BUILD_DIR"
    -- "${BUILD_OPTS[@]}"
)

log_info "Running: ${BUILD_CMD[*]}"
"${BUILD_CMD[@]}"

log_ok "Build complete! .pkg created at $BUILD_DIR/dist/${TITLE_ID}.ffpkg"

# Step 8: Create exFAT image
if [[ "$CREATE_EXFAT" == true ]]; then
    log_info "Step 7/7: Creating exFAT disk image..."

    PKG_FILE="$BUILD_DIR/dist/${TITLE_ID}.ffpkg"
    if [[ ! -f "$PKG_FILE" ]]; then
        # Check alternative locations
        for f in "$HOME/ps5probe/dist/${TITLE_ID}.ffpkg" "$TITLE_DIR/dist/${TITLE_ID}.ffpkg"; do
            if [[ -f "$f" ]]; then
                PKG_FILE="$f"
                break
            fi
        done
    fi

    if [[ ! -f "$PKG_FILE" ]]; then
        log_error ".pkg file not found"
        exit 1
    fi

    # Create exFAT image
    log_info "Creating ${IMAGE_SIZE_MB}MB exFAT image..."
    dd if=/dev/zero of="$OUTPUT_FILE" bs=1M count="$IMAGE_SIZE_MB" status=progress

    if command -v mkfs.exfat >/dev/null; then
        mkfs.exfat "$OUTPUT_FILE"
    else
        log_warn "exFAT tools not found, using FAT32 (4GB limit)"
        mkfs.vfat -F 32 "$OUTPUT_FILE"
    fi

    # Mount and copy
    MOUNT_POINT=$(mktemp -d)
    LOOP_DEV=$(sudo losetup --find --show "$OUTPUT_FILE")
    sudo partprobe "$LOOP_DEV" 2>/dev/null || true
    sleep 1
    sudo mount "$LOOP_DEV" "$MOUNT_POINT"

    sudo cp "$PKG_FILE" "$MOUNT_POINT/${TITLE_ID}.pkg"

    # README
    cat > /tmp/README_USB.txt <<EOF
ProsperoCraft for PS5
=====================
Title ID: $TITLE_ID
Version: $VERSION
Build date: $(date)

Installation:
1. Insert this USB drive into your PS5
2. Go to Settings > System > System Software > Install Package Files
3. Select "Install from USB Drive"
4. Choose "${TITLE_ID}.pkg" and install

Requirements:
- PS5 with homebrew enabled
- The app appears as "ProsperoCraft"

Offline/Demo mode:
- Select "Play Offline" in launcher
- Choose profile from "Who is playing?"
- Microsoft account without Minecraft = auto demo mode

Source: https://github.com/donoigabriel6-a11y/ProsperoCraft-Offline
EOF
    sudo cp /tmp/README_USB.txt "$MOUNT_POINT/README.txt"

    sync
    sudo umount "$MOUNT_POINT"
    sudo losetup -d "$LOOP_DEV"
    rmdir "$MOUNT_POINT"

    log_ok "exFAT image created: $OUTPUT_FILE"
    IMAGE_PATH=$(realpath "$OUTPUT_FILE")
fi

# Summary
echo ""
echo "=========================================="
echo "  BUILD COMPLETE!"
echo "=========================================="
echo "Title ID: $TITLE_ID"
echo "Version: $VERSION"
echo ".pkg file: $BUILD_DIR/dist/${TITLE_ID}.ffpkg"
if [[ "$CREATE_EXFAT" == true ]]; then
    echo "exFAT image: $IMAGE_PATH"
    echo ""
    echo "To install on PS5:"
    echo "  1. Write image to USB: sudo dd if=$IMAGE_PATH of=/dev/sdX bs=4M status=progress conv=fsync"
    echo "     (replace /dev/sdX with your USB device)"
    echo "  2. Insert USB into PS5"
    echo "  3. Settings → System → System Software → Install Package Files → Install from USB Drive"
    echo "  4. Select ${TITLE_ID}.pkg"
fi
echo ""
echo "To deploy directly to PS5 (if PS5_IP was set):"
echo "  bash $REPO_ROOT/tools/deploy.sh deploy PS5_HOST=$PS5_IP"
echo ""