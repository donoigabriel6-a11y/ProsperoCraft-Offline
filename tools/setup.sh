#!/usr/bin/env bash
# One-time setup of the external parts the build needs, at the versions this project was built with.
# (The payload SDK, the OpenGL SDK 1.0.2 and the PS5_Mesa headers are installed separately: see docs/BUILDING.md.)
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
get() {  # get <url> <dir> <commit>
  if [ ! -d "$2/.git" ]; then git clone "$1" "$2"; fi
  git -C "$2" fetch -q origin && git -C "$2" checkout -q "$3"; echo "$2 at $(git -C "$2" log --oneline -1)"
}
# Title boilerplate (GPL-3, BlackBearReloaded): the folder make_jvm_title.sh turns into the title. Our title identity is title/sce_sys/param.json.
get https://github.com/blackbearreloaded/ps5-native-app-boilerplate "$HOME/ps5probe" dd44bbd
mkdir -p "$HOME/ps5probe/sce_sys" && cp -f "$REPO/title/sce_sys/param.json" "$HOME/ps5probe/sce_sys/param.json"
# Vulkan / RADV project and template (mihawk-99)
get https://github.com/mihawk-99/PS5_Vulkan.git "$HOME/PS5_Vulkan" f410e67
get https://github.com/mihawk-99/PS5_VulkanTemplate "$HOME/PS5_VulkanTemplate" 91179df7
echo "next: bash tools/sync.sh (copies this repo into ~/ps5-mc-java), then tools/build.sh"
