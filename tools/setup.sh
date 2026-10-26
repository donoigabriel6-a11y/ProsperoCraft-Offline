#!/usr/bin/env bash
# One-time setup of the external parts the build needs, at the versions this project was built with.
# (The title boilerplate is in title/. The payload SDK, the OpenGL SDK 1.0.2 and the PS5_Mesa headers are installed separately: see docs/BUILDING.md.)
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
get() {  # get <url> <dir> <commit>
  if [ ! -d "$2/.git" ]; then git clone "$1" "$2"; fi
  git -C "$2" fetch -q origin && git -C "$2" checkout -q "$3"; echo "$2 at $(git -C "$2" log --oneline -1)"
}
# Title boilerplate: vendored in this repo (title/), the upstream repository no longer exists. tools/sync.sh copies it to ~/ps5probe.
# Vulkan / RADV project and template (mihawk-99)
get https://github.com/mihawk-99/PS5_Vulkan.git "$HOME/PS5_Vulkan" f410e67
get https://github.com/mihawk-99/PS5_VulkanTemplate "$HOME/PS5_VulkanTemplate" 91179df7
# Our edited file of the template (stored whole in third_party/)
cp -f "$REPO/third_party/PS5_VulkanTemplate/ps5/tools/link-title.sh" "$HOME/PS5_VulkanTemplate/ps5/tools/link-title.sh"
echo "next: bash tools/sync.sh (copies this repo into ~/ps5-mc-java), then tools/build.sh"
