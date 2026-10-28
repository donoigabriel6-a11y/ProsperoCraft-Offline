#!/usr/bin/env bash
# Build ProsperoCraft and create an exFAT disk image with the .pkg file
# for USB installation on PS5.
#
# Requirements:
# - PS5 SDK installed (~/.deps/native/ps5-payload-sdk or via tools/setup-native-dependencies.sh)
# - Java 17+ JDK
# - Python 3
# - clang/clang++ 18+
# - For exFAT image: mtools, dosfstools, parted (or use raw image)
#
# Usage:
#   bash build_exfat.sh [options]
#
# Options:
#   --skip-build      Skip the build step (use existing dist/)
#   --size SIZE       exFAT image size in MB (default: 512)
#   --output FILE     Output image file (default: ProsperoCraft-<version>.img)
#   --help            Show this help

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TITLE_DIR="$SCRIPT_DIR/title"
REPO_ROOT="$SCRIPT_DIR"

# Defaults
SKIP_BUILD=false
IMAGE_SIZE_MB=512
OUTPUT_FILE=""

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-build) SKIP_BUILD=true; shift ;;
        --size) IMAGE_SIZE_MB="$2"; shift 2 ;;
        --output) OUTPUT_FILE="$2"; shift 2 ;;
        --help|-h)
            sed -n '2,20p' "$0" | sed 's/^# //'
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

# Get version from param.json
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

echo "=== ProsperoCraft exFAT Image Builder ==="
echo "Title ID: $TITLE_ID"
echo "Version: $VERSION"
echo "App Name: $APP_NAME"
echo "Output: $OUTPUT_FILE"
echo "Image size: ${IMAGE_SIZE_MB}MB"
echo ""

# Step 1: Build the project (creates dist/$TITLE_ID.ffpkg)
if [[ "$SKIP_BUILD" == false ]]; then
    echo "=== Step 1: Building .pkg file ==="
    cd "$TITLE_DIR"

    # Check for required tools
    if ! command -v make >/dev/null; then
        echo "Error: make not found" >&2
        exit 1
    fi
    if ! command -v python3 >/dev/null; then
        echo "Error: python3 not found" >&2
        exit 1
    fi

    # Build the .pkg file
    make ffpkg

    echo "=== Build complete ==="
    echo ""
fi

# Step 2: Verify .pkg exists
PKG_FILE="$TITLE_DIR/dist/${TITLE_ID}.ffpkg"
if [[ ! -f "$PKG_FILE" ]]; then
    echo "Error: .pkg file not found at $PKG_FILE" >&2
    echo "Run without --skip-build or ensure build completed successfully" >&2
    exit 1
fi

PKG_SIZE=$(stat -c%s "$PKG_FILE")
echo "Found .pkg: $PKG_FILE (${PKG_SIZE} bytes)"

# Step 3: Create exFAT disk image
echo ""
echo "=== Step 2: Creating exFAT disk image ==="

# Check if we have the tools to create exFAT
if command -v mkfs.exfat >/dev/null 2>&1; then
    MKFS_EXFAT="mkfs.exfat"
elif command -v mkfs.vfat >/dev/null 2>&1; then
    # Fallback to FAT32 if exFAT not available (but exFAT is preferred for >4GB files)
    MKFS_EXFAT="mkfs.vfat -F 32"
    echo "Warning: exFAT tools not found, using FAT32 (4GB file size limit)"
else
    echo "Error: No filesystem formatting tool found (mkfs.exfat or mkfs.vfat)" >&2
    echo "Install exfatprogs or dosfstools" >&2
    exit 1
fi

# Create raw image file
echo "Creating ${IMAGE_SIZE_MB}MB raw image..."
dd if=/dev/zero of="$OUTPUT_FILE" bs=1M count="$IMAGE_SIZE_MB" status=progress

# Format as exFAT
echo "Formatting as exFAT..."
$MKFS_EXFAT "$OUTPUT_FILE"

# Create mount point and mount
MOUNT_POINT=$(mktemp -d)
echo "Mounting image at $MOUNT_POINT..."

# Use loop device
LOOP_DEV=$(sudo losetup --find --show "$OUTPUT_FILE")
sudo partprobe "$LOOP_DEV" 2>/dev/null || true
sleep 1

# Mount the loop device
sudo mount "$LOOP_DEV" "$MOUNT_POINT"

# Copy .pkg file to image
echo "Copying .pkg to image..."
sudo cp "$PKG_FILE" "$MOUNT_POINT/${TITLE_ID}.pkg"

# Create a README on the USB
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
- PS5 with homebrew enabled (exploit required)
- The app will appear on the home screen as "ProsperoCraft"

For offline/demo mode:
- Select "Play Offline" in the launcher
- Choose a profile from "Who is playing?"
- If signed in with Microsoft account that doesn't own Minecraft, demo mode activates automatically

Source: https://github.com/donoigabriel6-a11y/ProsperoCraft-Offline
EOF

sudo cp /tmp/README_USB.txt "$MOUNT_POINT/README.txt"

# Unmount
echo "Unmounting..."
sync
sudo umount "$MOUNT_POINT"
sudo losetup -d "$LOOP_DEV"
rmdir "$MOUNT_POINT"

echo ""
echo "=== Done! ==="
echo "exFAT image created: $OUTPUT_FILE"
echo ""
echo "To use:"
echo "1. Write to USB drive: sudo dd if=$OUTPUT_FILE of=/dev/sdX bs=4M status=progress"
echo "   (replace /dev/sdX with your USB device)"
echo "2. Insert USB into PS5"
echo "3. Settings > System > System Software > Install Package Files > Install from USB Drive"
echo ""
echo "Image size: $(ls -lh "$OUTPUT_FILE" | awk '{print $5}')"