#!/usr/bin/env bash
# make_zink_obj.sh: turns the Zink + EGL build of build-zink-probe.sh into ONE relocatable object, ~/zink_obj/zink.o, the way the GL SDK is isolated in make_mcvk_title.sh (gl.o):
#   - the inputs are exactly what Mesa's own link steps use (libgallium-*.so for the Zink driver, libEGL.so for the EGL front), taken from ninja, so nothing is guessed;
#   - one relocatable link pulls in what the entry points need;
#   - every symbol becomes local except the EGL API (renamed zink_eglXxx: the GL SDK has its own eglXxx) and the symbols the DRI loader looks up (__driDriverGetExtensions_zink ...),
#     so Zink's copies of NIR / Mesa util / glapi can sit next to RADV's and the GL SDK's without a duplicate-symbol error.
# Prints the symbols the object still needs from the rest of the program (libc, libm, pthread, dl*): that list decides what the title's link has to provide.
# Run from anywhere:  bash make_zink_obj.sh        (BUILD=... PSDK=... OUT=... override the defaults)
set -uo pipefail
BUILD=${BUILD:-$HOME/PS5_Vulkan/.deps/work/zink-build-ps5}
PSDK=${PSDK:-$HOME/PS5_Vulkan/.deps/native/ps5-payload-sdk}
OUT=${OUT:-$HOME/zink_obj}
NINJA=${NINJA:-$(command -v ninja || echo "$HOME/.local/bin/ninja")}
[ -f "$BUILD/build.ninja" ] || { echo "no $BUILD/build.ninja: run tools/build-zink-probe.sh first"; exit 2; }
pick() { for c in "$@"; do [ -x "$c" ] && { echo "$c"; return; }; done; }
RLLD=$(pick "$PSDK/bin/ld.lld" /usr/lib/llvm-18/bin/ld.lld); OBJC=$(pick "$PSDK/bin/llvm-objcopy" /usr/lib/llvm-18/bin/llvm-objcopy); NM=$(pick "$PSDK/bin/llvm-nm" /usr/lib/llvm-18/bin/llvm-nm)
[ -n "$RLLD" ] && [ -n "$OBJC" ] && [ -n "$NM" ] || { echo "ld.lld / llvm-objcopy / llvm-nm not found (set PSDK)"; exit 2; }
mkdir -p "$OUT"; cd "$BUILD" || exit 2
# 1. the link inputs of Mesa's own targets (objects and archives, build-dir relative)
inputs() {   # $1 = ninja target; last command printed is its link
    "$NINJA" -t commands "$1" 2>/dev/null | tail -1 | python3 -c "
import shlex,sys
for t in shlex.split(sys.stdin.read()):
    if (t.endswith('.o') or t.endswith('.a')) and 'tests/' not in t and not t.startswith('-'): print(t)"
}
TARGETS=$("$NINJA" -t targets all 2>/dev/null | sed 's/:.*//')
DRI=$(echo "$TARGETS" | grep -E '^src/gallium/targets/dri/libgallium-[0-9.]+\.so$' | head -1)
EGLT=$(echo "$TARGETS" | grep -E '^src/egl/libEGL\.so\.[0-9]+\.[0-9]+\.[0-9]+$' | head -1); EGLT=${EGLT#src/egl/}
echo "== ninja targets: DRI='$DRI' EGL='$EGLT'"
{ inputs "$DRI"; inputs "src/egl/$EGLT"; } | awk '!s[$0]++' > "$OUT/inputs.txt"
echo "== link inputs: $(wc -l < "$OUT/inputs.txt") ($(grep -c '\.o$' "$OUT/inputs.txt") objects, $(grep -c '\.a$' "$OUT/inputs.txt") archives)"
[ -s "$OUT/inputs.txt" ] || { echo "ninja gave no link inputs for $DRI / src/egl/$EGLT"; echo "$TARGETS" | grep -E "libEGL|libgallium" | grep -E "\.so" | head; exit 1; }
for f in $(cat "$OUT/inputs.txt"); do [ -e "$f" ] || echo "  (missing: $f - build it with ninja -C $BUILD $f)"; done
OBJS=$(grep '\.o$' "$OUT/inputs.txt" | tr '\n' ' '); ARCS=$(grep '\.a$' "$OUT/inputs.txt" | tr '\n' ' ')
# 2. roots: the EGL API in the EGL objects and what the DRI loader looks up (dri.sym / dri.dyn)
: > "$OUT/roots.txt"
for o in $(grep 'libEGL' "$OUT/inputs.txt" | grep '\.o$'); do "$NM" -g --defined-only -j "$o" 2>/dev/null; done | grep -E '^egl[A-Z]' | sort -u > "$OUT/egl_api.txt"
DYN=$HOME/PS5_Vulkan/.deps/work/radv-src/src/gallium/targets/dri.dyn
{ grep -hoE '^[[:space:]]*[A-Za-z_][A-Za-z0-9_]*;' "$BUILD/src/gallium/targets/dri/dri.sym" 2>/dev/null; [ -f "$DYN" ] && grep -hoE '[A-Za-z_][A-Za-z0-9_]*;' "$DYN"; } | tr -d '; \t' | grep -v -E '^(global|local)$' | sort -u > "$OUT/dri_exports.txt"
cat "$OUT/egl_api.txt" "$OUT/dri_exports.txt" | sort -u > "$OUT/roots.txt"
echo "== roots: $(wc -l < "$OUT/egl_api.txt") EGL entry points, $(wc -l < "$OUT/dri_exports.txt") DRI loader symbols: $(tr '\n' ' ' < "$OUT/dri_exports.txt" | cut -c1-200)"
U=; for s in $(cat "$OUT/roots.txt"); do U="$U -u $s"; done
# 3. one relocatable link
"$RLLD" -r -m elf_x86_64 -o "$OUT/zink_pre.o" $U $OBJS --start-group $ARCS --end-group 2> "$OUT/pre.err" || { echo "== relocatable link FAILED"; sort "$OUT/pre.err" | uniq -c | sort -rn | head -30; exit 1; }
sort "$OUT/pre.err" | uniq -c | sort -rn | head -5
"$NM" -g --defined-only -j "$OUT/zink_pre.o" | LC_ALL=C sort -u > "$OUT/defined.txt"
# 4. only the roots stay global; the EGL API gets a zink_ prefix
grep -Fxf "$OUT/defined.txt" "$OUT/roots.txt" > "$OUT/keep_roots.txt"
grep -Fxf "$OUT/defined.txt" "$OUT/egl_api.txt" | awk '{print $1" zink_"$1}' > "$OUT/rename.txt"
{ cat "$OUT/keep_roots.txt" | grep -v '^egl[A-Z]'; awk '{print $2}' "$OUT/rename.txt"; } > "$OUT/keep_final.txt"
"$OBJC" --redefine-syms="$OUT/rename.txt" "$OUT/zink_pre.o" "$OUT/zink_ren.o" && "$OBJC" --keep-global-symbols="$OUT/keep_final.txt" "$OUT/zink_ren.o" "$OUT/zink.o" || { echo "objcopy failed"; exit 1; }
"$NM" -u -j "$OUT/zink.o" | LC_ALL=C sort -u > "$OUT/undefined.txt"
"$NM" -g --defined-only -j "$OUT/zink.o" | LC_ALL=C sort -u > "$OUT/exported.txt"
echo "== $OUT/zink.o: $(du -h "$OUT/zink.o" | cut -f1), exports $(wc -l < "$OUT/exported.txt"): $(tr '\n' ' ' < "$OUT/exported.txt" | cut -c1-400)"
echo "== still undefined ($(wc -l < "$OUT/undefined.txt")): the rest of the program has to provide these"
cat "$OUT/undefined.txt" | tr '\n' ' ' | fold -w 200
echo
echo "== of these, Vulkan / GL-SDK looking:"; grep -E '^(vk|ps5_|sce|agc|Sce)' "$OUT/undefined.txt" | tr '\n' ' '; echo
