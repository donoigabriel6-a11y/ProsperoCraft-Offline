#!/usr/bin/env bash
# build_zink.sh: Mesa PS5 patch (Zink presents on the console display) -> incremental Zink + EGL build -> ~/zink_obj/zink.o. Run from ~/ps5-mc-java with the tools in place:
#   cp build-zink-probe.sh ~/PS5_Vulkan/tools/  (done once)   Needs ~/PS5_Vulkan/.deps/work/radv-src from tools/build-radv.sh.
set -eo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
python3 "$HERE/patch_mesa_zink_ps5.py" "${MESA_SRC:-$HOME/PS5_Vulkan/.deps/work/radv-src}"
out=$(bash "$HOME/PS5_Vulkan/tools/build-zink-probe.sh" 2>&1) || true
echo "$out" | grep -E "^== (meson|ninja)|FAILED|error:|undefined" | sort | uniq -c | sort -rn | head -30
LOG=$HOME/PS5_Vulkan/.deps/work/zink-build-ps5.log
# the shared libgallium / libEGL and the libdrm test programs never link on this target (known, unused: make_zink_obj.sh links the objects itself); only a COMPILE failure matters
if grep -E "^FAILED:" "$LOG" | grep -E "\.(o|c|cpp|h|py)( |$)|\.p/" | grep -v "tests/" | head -1 | grep -q .; then echo "BUILD FAILED: a compile step failed:"; grep -E "^FAILED:" "$LOG" | grep -E "\.p/" | head; grep -E "error:" "$LOG" | grep -v -E "ld.lld|linker command" | head -20; exit 1; fi
echo "compile steps ok (only the known link-only failures remain)"
bash "$HERE/make_zink_obj.sh" 2>&1 | grep -E "^== (ninja|link inputs|/home|still undefined)|FAILED|failed|error" | cut -c1-300
ls -la "$HOME/zink_obj/zink.o"
