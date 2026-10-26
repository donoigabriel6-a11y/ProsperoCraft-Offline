#!/usr/bin/env bash
# build-zink-probe.sh (goes in ~/PS5_Vulkan/tools/): does Mesa's Zink + EGL (surfaceless) configure and compile for the console with the RADV recipe?
# Same source tree (the pinned revision tools/build-radv.sh exported), same cross files and zlib subproject; its own build directory, nothing of the RADV build is touched.
# Prints: where meson setup stops (if it does), every compile error, and the static archives that came out. Paste the output.
set -uo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
src=$root/.deps/work/radv-src
build=$root/.deps/work/zink-build-ps5
constants=$root/.deps/work/radv-cross-constants.ini
meson=${MESON:-$(command -v meson || echo "$HOME/.local/bin/meson")}
ninja=${NINJA:-$(command -v ninja || echo "$HOME/.local/bin/ninja")}
for f in "$src/.revision" "$constants" "$root/tooling/radv/ps5-cross.ini"; do
    [[ -f $f ]] || { echo "missing $f: run tools/build-radv.sh release once first"; exit 2; }
done
[[ -x $meson && -x $ninja ]] || { echo "meson / ninja not found"; exit 2; }
echo "mesa revision: $(cat "$src/.revision")  version: $(cat "$src/VERSION")"
export PATH="$root/.deps/work/radv-clc-bin:$PATH"
options=(-Dvulkan-drivers= -Dgallium-drivers=zink -Dplatforms= -Dopengl=true -Degl=enabled -Dgbm=disabled -Dglx=disabled -Dgles1=disabled -Dgles2=disabled
    -Dglvnd=disabled -Dllvm=disabled -Dvideo-codecs= -Dbuildtype=debugoptimized -Db_ndebug=true
    -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled -Dzlib=enabled --force-fallback-for=zlib,libdrm -Dallow-fallback-for=libdrm
    -Dexpat=disabled -Dxmlconfig=disabled -Dshader-cache=enabled -Dbuild-tests=false -Dvulkan-layers= -Dtools= -Dmesa-clc=system)
if [[ ! -f $build/build.ninja || $(cat "$build/.zink-options" 2>/dev/null) != "${options[*]}" ]]; then
    wipe=(); [[ -f $build/build.ninja ]] && wipe=(--wipe)
    "$meson" setup "${wipe[@]}" "$build" "$src" --cross-file "$constants" --cross-file "$root/tooling/radv/ps5-cross.ini" "${options[@]}" > "$build.setup.log" 2>&1 || {
        echo "== meson setup FAILED (last 40 lines of $build.setup.log)"; tail -40 "$build.setup.log"
        loc=$(grep -oE "meson.build:[0-9]+" "$build.setup.log" | tail -1); if [[ -n $loc ]]; then n=${loc#meson.build:}; echo "== $src/meson.build around line $n"; sed -n "$((n-14)),$((n+4))p" "$src/meson.build"; fi
        ls "$src/subprojects" | grep -i "drm" ; exit 1; }
    printf '%s\n' "${options[*]}" > "$build/.zink-options"
fi
echo "== meson setup ok"; grep -E "^ *(Gallium|EGL|OpenGL|Platforms|Zink|Shared glapi|GLES|Vulkan drivers|Gallium drivers|Window system)" "$build.setup.log" | head -20
"$ninja" -C "$build" -k 0 -j "${JOBS:-$(nproc)}" > "$build.log" 2>&1; rc=$?
echo "== ninja exit $rc (log $build.log)"
grep -E "FAILED|error:|undefined" "$build.log" | sort | uniq -c | sort -rn | head -40
echo "== static archives"
find "$build" -name '*.a' -size +0 -printf '%s %P\n' | sort -rn | head -50
echo "== EGL / driver objects"
find "$build" \( -name 'libEGL*' -o -name 'libgallium*' -o -name 'libglapi*' -o -name 'libGL*' -o -name '*dri*.so*' \) -printf '%s %P\n' | head -20
