#!/bin/bash
# vk_relink.sh (called by make_jvm_title.sh when the build word `vk` is given): the launcher title PPSA27272 with THREE renderers in one executable:
#   OpenGL SDK (real GL, default), native Vulkan (RADV, Minecraft's own Vulkan backend) and OpenGL over Zink (Mesa on RADV, for GL-only versions), chosen at start-up (launcher.cfg renderer=gl|vulkan|zink).
# The boilerplate in ~/ps5probe still COMPILES everything (jvm_host.cpp, the native launcher UI, tool objects: build/obj/**) and builds the per-JDK objects and the LWJGL archive; its own link cannot carry RADV
# (RADV needs the Vulkan project's link recipe: --no-dynamic-linker, its libc.prx and platform layer). So the boilerplate link is allowed to fail here, and the SAME objects are linked again with
# the recipe of make_mcvk_title.sh AIO=1 (GL SDK as a relocatable object next to RADV, Zink as zink.o), signed, assembled into dist/<title> and uploaded with the Vulkan project's folder uploader
# (it never deletes anything on the console: downloaded versions, worlds and mods under <app>/mc stay).
# Environment from make_jvm_title.sh: T HERE S JDKLIST TID PS5 GLSDK VKINC (and optionally ZINKOBJ, RADV_ARCHIVE, TPL, VK, NODEPLOY).
set -e
T=${T:?}; HERE=${HERE:?}; JDKLIST=${JDKLIST:?}; TID=${TID:?}; GLSDK=${GLSDK:?}; VKINC=${VKINC:?}
TPL=${TPL:-$HOME/PS5_VulkanTemplate}; VK=${VK:-$HOME/PS5_Vulkan}
SDK=$(bash "$TPL/ps5/tools/setup-sdk.sh")
ARCHIVE=${RADV_ARCHIVE:-$VK/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a}
TOOL=$VK/build/host/ps5-native-tool; NATIVE=$VK/tooling/native
LLD=$SDK/bin/prospero-lld; NM=$SDK/bin/llvm-nm
PSDK=$T/.deps/native/ps5-payload-sdk
RLLD=; for c in "$PSDK/bin/ld.lld" /usr/lib/llvm-18/bin/ld.lld "$SDK/bin/ld.lld"; do [ -x "$c" ] && { RLLD=$c; break; }; done
OBJC=; for c in "$PSDK/bin/llvm-objcopy" /usr/lib/llvm-18/bin/llvm-objcopy "$SDK/bin/llvm-objcopy"; do [ -x "$c" ] && { OBJC=$c; break; }; done
AR=; for c in "$SDK/bin/llvm-ar" "$PSDK/bin/llvm-ar" /usr/lib/llvm-18/bin/llvm-ar; do [ -x "$c" ] && { AR=$c; break; }; done
[ -n "$RLLD" ] && [ -n "$OBJC" ] && [ -n "$AR" ] || { echo "vk: no ld.lld / llvm-objcopy / llvm-ar found"; exit 1; }
ZSRC=${ZINKOBJ:-$HOME/zink_obj/zink.o}
for f in "$ARCHIVE" "$TOOL" "$LLD" "$VK/tools/radv-link.sh" "$VK/runtime/libc.prx" "$NATIVE/app_crt.cpp" "$T/vendor/whole-lwjgl.a" "$T/sce_sys/param.json" "$HERE/gen_gl_thunks.sh" "$HERE/ps5vk_stubs.c"; do
  [ -e "$f" ] || { echo "vk: missing $f (the RADV archive / Vulkan project: see make_mcvk_title.sh)"; exit 1; }
done
ZINK=; [ -f "$ZSRC" ] && ZINK=1
cc() { PS5_PAYLOAD_SDK="$SDK" sh "$VK/tooling/prospero-clang18" "$@"; }
V=$T/.vkrelink; rm -rf "$V"; mkdir -p "$V/obj" "$V/stubs"
# USB keyboard / mouse: the title imports sceKeyboard* / sceMouse* (ps5usbin.inc). The payload SDK has a stub for libSceKeyboard but none for libSceMouse, so a missing one is built here (the same asm-label trick as the
# SDK's sce_stubs) and put next to the others; the linker pulls it in (--as-needed *.so) and the title converter (--stub-dir) writes the import.
for spec in "libSceKeyboard:sceKeyboardInit sceKeyboardOpen sceKeyboardReadState sceKeyboardClose" "libSceMouse:sceMouseInit sceMouseOpen sceMouseRead sceMouseClose"; do
  lib=${spec%%:*}; fns=${spec#*:}
  [ -f "$SDK/target/lib/$lib.so" ] && continue
  MCLANG=; for c in "$SDK/bin/clang" "$PSDK/bin/clang" clang-18 clang; do command -v "$c" >/dev/null 2>&1 && { MCLANG=$c; break; }; done
  [ -n "$MCLANG" ] || { echo "vk: no clang found to build the $lib stub"; exit 1; }
  mkdir -p "$V/stubs_usb"; for f in $fns; do printf 'asm(".global %s\\n"\n    ".type %s @function\\n"\n    "%s:\\n");\n' $f $f $f; done > "$V/stubs_usb/$lib.c"
  "$MCLANG" -c -ffreestanding -fno-builtin -nostdlib -fPIC -target x86_64-sie-ps5 -fno-plt -fno-stack-protector -fvisibility-nodllstorageclass=default "$V/stubs_usb/$lib.c" -o "$V/stubs_usb/$lib.o" \
    && "$RLLD" -m elf_x86_64 -shared -soname $lib.sprx -o "$V/stubs_usb/$lib.so" "$V/stubs_usb/$lib.o" \
    && cp "$V/stubs_usb/$lib.so" "$SDK/target/lib/$lib.so" && echo "   USB input: built the $lib stub into $SDK/target/lib" \
    || { echo "vk: building the $lib stub failed (see above)"; exit 1; }
done
echo "== [${SECONDS}s] vk: Zink object: ${ZINK:+$ZSRC}${ZINK:-none (renderer=zink falls back to Vulkan)}"

echo "== [${SECONDS}s] vk: compile the boilerplate objects (its own link fails on purpose: RADV is not in it)"
# The app builder only accepts 3840x2160 BC7_UNORM DX10 DDS pictures; the console itself takes others (they work when put there by FTP). A picture it would refuse is held back for the build (a valid black
# placeholder stands in for pic0 / pic1) and goes into the title folder afterwards, so the build never judges it.
HOLD="$V/hold"; mkdir -p "$HOLD"
python3 - "$T/sce_sys" "$HOLD" <<'PYH'
import os, shutil, struct, sys
d, hold = sys.argv[1:3]
def ok(fn):
    b = open(fn, "rb").read(148)
    if len(b) < 148 or b[:4] != b"DDS " or b[84:88] != b"DX10": return False
    h, w, mips, fmt = struct.unpack_from("<I", b, 12)[0], struct.unpack_from("<I", b, 16)[0], struct.unpack_from("<I", b, 28)[0], struct.unpack_from("<I", b, 128)[0]
    return w == 3840 and h == 2160 and mips <= 1 and fmt == 98
for n in sorted(os.listdir(d)):
    fn = os.path.join(d, n)
    if not n.lower().endswith(".dds") or ok(fn): continue
    shutil.move(fn, os.path.join(hold, n)); print("   held back for the app builder (not 3840x2160 BC7): " + n)
    if n in ("pic0.dds", "pic1.dds"):
        hd = bytearray(148); hd[:4] = b"DDS "; struct.pack_into("<IIIII", hd, 4, 124, 0x1007, 2160, 3840, 3840 * 2160); struct.pack_into("<I", hd, 76, 32); struct.pack_into("<I", hd, 80, 4); hd[84:88] = b"DX10"; struct.pack_into("<I", hd, 108, 0x1000); struct.pack_into("<IIIII", hd, 128, 98, 3, 0, 1, 0)
        open(fn, "wb").write(bytes(hd) + bytes(3840 * 2160))
PYH
# the music: the builder wants ATRAC9 RIFF chunks the console itself does not insist on (a snd0.at9 put there by FTP plays): it is held back from the builder the same way and installed afterwards
if [ -f "$T/sce_sys/snd0.at9" ]; then
  python3 - "$T/sce_sys/snd0.at9" <<'PYS'
import struct, sys
b = open(sys.argv[1], "rb").read(4096); ch = []; o = 12
while o + 8 <= len(b) and len(ch) < 12:
    ch.append(b[o:o+4].decode("latin1")); o += 8 + struct.unpack_from("<I", b, o + 4)[0] + (struct.unpack_from("<I", b, o + 4)[0] & 1)
print("   snd0.at9: RIFF chunks seen: " + " ".join(ch) + " (held back from the app builder, installed afterwards)")
PYS
  mv "$T/sce_sys/snd0.at9" "$HOLD/snd0.at9"
fi
( cd "$T" && make app > "$V/boilerplate.log" 2>&1 ) || true
for h in "$HOLD"/*; do [ -f "$h" ] && mv -f "$h" "$T/sce_sys/"; done
nsrc=$(find "$T/src" -type f \( -name '*.c' -o -name '*.cc' -o -name '*.cpp' \) | wc -l)
# objects left over from earlier builds (e.g. the GL title's app_heap.o / runtime_shims.o) have no source any more and must not be linked
for o in $(find "$T/build/obj/src" -name '*.o'); do
  r=${o#$T/build/obj/src/}; r=${r%.o}; r=${r%.c}; r=${r%.cc}; r=${r%.cpp}
  [ -f "$T/src/$r.c" ] || [ -f "$T/src/$r.cc" ] || [ -f "$T/src/$r.cpp" ] || { echo "   stale object removed: $r.o"; rm -f "$o"; }
done
OBJS=$(find "$T/build/obj/src" -name '*.o' | sort)
nobj=$(echo "$OBJS" | grep -c . || true)
if [ "$nobj" -ne "$nsrc" ]; then
  echo "vk: $nobj objects for $nsrc sources: a compile failed. Errors from the boilerplate build:"; grep -E "error|FAILED" "$V/boilerplate.log" | head -30; exit 1
fi
[ -f "$T/build/obj/app_cpp_runtime.o" ] && CPPRT=$T/build/obj/app_cpp_runtime.o || CPPRT=
echo "   $nobj objects (host, native launcher UI, tools), app_cpp_runtime: ${CPPRT:+yes}${CPPRT:-no}"

echo "== [${SECONDS}s] vk: shim stubs, CRT"
cc -std=gnu11 -O2 -fPIC -w -DAIO ${ZINK:+-DPS5_ZINK} -c "$HERE/ps5vk_stubs.c" -o "$V/obj/ps5vk_stubs.o"
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$NATIVE/app_crt.cpp" -o "$V/obj/app_crt.o"

# every __wrap_X the shim (or the host) defines needs --wrap=X at the link; the per-JDK objects were built with the same list (make_jvm_title.sh, vk mode)
WRAPS=$({ "$NM" --defined-only "$T/vendor/whole-lwjgl.a"; for o in $OBJS; do "$NM" --defined-only "$o"; done; } 2>/dev/null | awk '$3 ~ /^__wrap_[A-Za-z0-9_]+$/ { sub(/^__wrap_/, "", $3); print "--wrap=" $3 }' | sort -u | tr '\n' ' ')
echo "   wraps: $WRAPS"

echo "== [${SECONDS}s] vk: GL thunks for the native launcher UI (it calls gl* directly; they must reach the ACTIVE renderer: SDK or Zink)"
KITOBJS=$(find "$T/build/obj/src" -name '*.o' -path '*kit*' | sort)
if [ -n "$KITOBJS" ]; then
  "$NM" -u -j $KITOBJS 2>/dev/null | grep -E '^gl[A-Z][A-Za-z0-9]*$' | LC_ALL=C sort -u > "$V/thunk_names.txt"
else : > "$V/thunk_names.txt"; fi
bash "$HERE/gen_gl_thunks.sh" "$V/thunk_names.txt" "$V/gl_thunks.S"
cc -c "$V/gl_thunks.S" -o "$V/obj/gl_thunks.o"
echo "   $(wc -l < "$V/thunk_names.txt") GL functions go through thunks: $(head -c 300 "$V/thunk_names.txt" | tr '\n' ' ')"

echo "== [${SECONDS}s] vk: OpenGL SDK as a relocatable object next to RADV"
GLC=$(sed -n 's/^ *\(lib[A-Za-z0-9_.+-]*\.a\) *$/\1/p' "$GLSDK/lib/libPS5OpenGL.a" | while read -r l; do printf '%s ' "$GLSDK/lib/$l"; done)
[ -n "$GLC" ] || { echo "vk: no component list in $GLSDK/lib/libPS5OpenGL.a"; exit 1; }
JDKA=""; for v in $JDKLIST; do JDKA="$JDKA $T/vendor/whole-jdk$v.a"; done
"$NM" -u -j $OBJS "$V/obj/ps5vk_stubs.o" "$T/vendor/whole-lwjgl.a" $JDKA 2>/dev/null | grep -v -e ':$' -e '^$' | LC_ALL=C sort -u > "$V/gl_want0.txt"
MALLOC_FAM='^__wrap_(malloc|calloc|realloc|free|posix_memalign|memalign|aligned_alloc|malloc_usable_size)$'   # ps5malloc.inc defines these; the GL SDK's app-heap copies must never be pulled in or exported
LC_ALL=C comm -23 "$V/gl_want0.txt" "$V/thunk_names.txt" | { grep -Ev "$MALLOC_FAM" || true; } > "$V/gl_want.txt"   # what the thunks define is not wanted from the SDK any more
"$NM" -g --defined-only -j $GLC 2>/dev/null | grep -v -e ':$' -e '^$' | LC_ALL=C sort -u > "$V/gl_arch_defined.txt"
{ LC_ALL=C comm -12 "$V/gl_arch_defined.txt" "$V/gl_want.txt"; grep -e '^egl' -e '^ps5_opengl_' -e '^glX' "$V/gl_arch_defined.txt"; } | LC_ALL=C sort -u > "$V/gl_roots.txt"
GLU=$(sed 's/^/-u /' "$V/gl_roots.txt" | tr '\n' ' ')
"$RLLD" -r -m elf_x86_64 -o "$V/gl_pre.o" $GLU --start-group $GLC --end-group 2>"$V/gl_pre.err" || { echo "vk: relocatable link of the GL SDK failed:"; sort "$V/gl_pre.err" | uniq -c | sort -rn | head -40; exit 1; }
"$NM" -g --defined-only -j "$V/gl_pre.o" | LC_ALL=C sort -u > "$V/gl_defined.txt"
LC_ALL=C comm -12 "$V/gl_defined.txt" "$V/gl_want.txt" > "$V/gl_keep.txt"; LC_ALL=C comm -23 "$V/gl_defined.txt" "$V/gl_keep.txt" > "$V/gl_local.txt"
{ grep -Ev "$MALLOC_FAM" "$V/gl_keep.txt" > "$V/gl_keep.tmp"; grep -E "$MALLOC_FAM" "$V/gl_keep.txt" >> "$V/gl_local.txt"; } || true; mv "$V/gl_keep.tmp" "$V/gl_keep.txt"; LC_ALL=C sort -u -o "$V/gl_local.txt" "$V/gl_local.txt"
"$OBJC" --localize-symbols="$V/gl_local.txt" "$V/gl_pre.o" "$V/obj/gl.o"
echo "   gl.o: $(du -m "$V/obj/gl.o" | cut -f1) MB, $(wc -l < "$V/gl_keep.txt") symbols kept global, $(wc -l < "$V/gl_local.txt") made local"
ZO=; if [ -n "$ZINK" ]; then "$OBJC" --strip-debug "$ZSRC" "$V/obj/zink.o"; ZO=$V/obj/zink.o; echo "   zink.o: $(du -m "$ZO" | cut -f1) MB"; fi

echo "== [${SECONDS}s] vk: AGC import stubs (union of RADV's and the GL SDK's)"
stub() { cc -std=c11 -O2 -fPIC -c "$VK/$2" -o "$V/obj/$1_stub.o"; "$LLD" --shared -soname "$1.prx" -o "$V/stubs/$1.so" "$V/obj/$1_stub.o"; }
stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c
for l in libSceAgc libSceAgcDriver; do
  { "$NM" -D --defined-only -j "$V/stubs/$l.so"; "$NM" -D --defined-only -j "$GLSDK/lib/$l.so"; } | grep -v -e ':$' -e '^$' | LC_ALL=C sort -u > "$V/stubs/$l.names"
  while read -r n; do echo "void $n(void){}"; done < "$V/stubs/$l.names" > "$V/stubs/$l.union.c"
  cc -std=c11 -O2 -fPIC -w -c "$V/stubs/$l.union.c" -o "$V/obj/$l.union.o"; "$LLD" --shared -soname "$l.prx" -o "$V/stubs/$l.u.so" "$V/obj/$l.union.o"
done
AGC1=$V/stubs/libSceAgc.u.so; AGC2=$V/stubs/libSceAgcDriver.u.so

echo "== [${SECONDS}s] vk: link with RADV (the Vulkan project's recipe)"
source "$VK/tools/radv-link.sh"
radv_link_recipe "$VK" "$SDK" "$ARCHIVE" || exit 2
SHIMWRAP=" $(printf '%s ' ${WRAPS//--wrap=/}) "; _k=(); _skip=0
for x in "${radv_link_flags[@]}"; do
  if [ "$_skip" = 1 ]; then
    _skip=0
    { echo '{'; echo '    local:'; sed -n 's/^ *\([A-Za-z_0-9]*\);$/\1/p' "$x" | while read -r n; do case "$SHIMWRAP" in *" $n "*) ;; *) printf '        %s;\n' "$n";; esac; done; echo '};'; } > "$V/radv-platform-local.map"
    _k+=("$V/radv-platform-local.map"); continue
  fi
  case $x in
    --defsym=*=ps5_*) n=${x#--defsym=}; n=${n%%=*}; case "$SHIMWRAP" in *" $n "*) echo "   RADV recipe: dropped $x (the shim wraps $n)"; continue;; esac;;
    --version-script) _skip=1;;
  esac
  _k+=("$x")
done
radv_link_flags=("${_k[@]}")
EXTRA_LOCAL=""
if grep -q " T ps5_localeconv$" <<<"$("$NM" --defined-only "$SDK/target/lib/libps5platform.a" 2>/dev/null)" && [[ " ${radv_link_flags[*]} " != *" --defsym=localeconv=ps5_localeconv "* ]]; then
  radv_link_flags+=(--defsym=localeconv=ps5_localeconv); EXTRA_LOCAL="localeconv"
fi
# some RADV-recipe / SDK archive member also defines the malloc-family __wrap_* (the GL title's app heap): the shim's allocator (ps5malloc.inc) must be the only global definition, so those get localized in private copies
mkdir -p "$V/loc"; printf '%s\n' __wrap_malloc __wrap_calloc __wrap_realloc __wrap_free __wrap_posix_memalign __wrap_memalign __wrap_aligned_alloc __wrap_malloc_usable_size > "$V/malloc_local.txt"
loc_fix() {   # $1 = path; prints the path to link instead
  case $1 in *.a|*.o) [ -f "$1" ] || { echo "$1"; return; };; *) echo "$1"; return;; esac
  if "$NM" -A --defined-only "$1" 2>/dev/null | grep -Eq ' [TtWwDdBb] __wrap_(malloc|calloc|realloc|free|posix_memalign|memalign|aligned_alloc|malloc_usable_size)$'; then
    local d="$V/loc/$(echo "$1" | tr '/' '_')"; "$OBJC" --localize-symbols="$V/malloc_local.txt" "$1" "$d" && { echo "   localized the malloc wrappers in $1" >&2; echo "$d"; return; }
  fi
  echo "$1"
}
_k=(); for x in "${radv_link_flags[@]}"; do _k+=("$(loc_fix "$x")"); done; radv_link_flags=("${_k[@]}")
_k=(); for x in "${radv_link_inputs[@]}"; do _k+=("$(loc_fix "$x")"); done; radv_link_inputs=("${_k[@]}")
CXXLIBS=""; for l in libunwind.a libc++abi.a libc++.a; do [ -f "$SDK/target/lib/$l" ] && CXXLIBS="$CXXLIBS $SDK/target/lib/$l"; done   # the native launcher UI is C++ (the boilerplate linked these too)
_c=""; for l in $CXXLIBS; do _c="$_c $(loc_fix "$l")"; done; CXXLIBS=$_c
RT=$(clang-18 --print-resource-dir 2>/dev/null)/lib/linux/libclang_rt.builtins-x86_64.a; [ -f "$RT" ] || RT=
WHOLE=(--whole-archive "$T/vendor/whole-lwjgl.a"); for v in $JDKLIST; do WHOLE+=("$T/vendor/whole-jdk$v.a"); done; WHOLE+=(--no-whole-archive)
USE_CPPRT=$CPPRT
link_elf() {
  { printf '{\n    local:\n        _Zn*;\n        _Zd*;\n'; for n in $EXTRA_LOCAL; do printf '        %s;\n' "$n"; done; printf '};\n'; } > "$V/vk-symbols.map"
  "$LLD" "${radv_linker_script[@]}" --eh-frame-hdr --no-dynamic-linker "${radv_link_flags[@]}" $WRAPS \
    --version-script "$V/vk-symbols.map" --exclude-libs=ALL -e _start -o "$V/llvm-pie.elf" \
    "$V/obj/app_crt.o" ${USE_CPPRT:+"$USE_CPPRT"} $OBJS "$V/obj/ps5vk_stubs.o" "$V/obj/gl_thunks.o" "$V/obj/gl.o" ${ZO:+"$ZO"} "${WHOLE[@]}" \
    "$AGC1" "$AGC2" "${radv_link_inputs[@]}" --start-group $CXXLIBS ${RT:+"$RT"} --end-group --as-needed "$SDK"/target/lib/*.so
}
run_link() { link_elf 2>"$V/link.err" || { grep -A3 -m2 'duplicate symbol' "$V/link.err" | sed 's/^/    lld: /'; for sym in $(grep -o 'duplicate symbol: [A-Za-z0-9_]*' "$V/link.err" | awk '{print $3}' | sort -u | head -3); do echo "vk: who defines $sym:"; for o in "$V/obj/app_crt.o" $CPPRT $OBJS "$V/obj/ps5vk_stubs.o" "$V/obj/gl_thunks.o" "$V/obj/gl.o" $ZO; do "$NM" --defined-only "$o" 2>/dev/null | grep -q " [TtWw] $sym$" && echo "    $o"; done; for a in "$T/vendor/whole-lwjgl.a" $JDKA "${radv_link_inputs[@]}" "${radv_link_flags[@]}" $CXXLIBS; do [ -f "$a" ] || continue;  "$NM" -A --defined-only "$a" 2>/dev/null | grep " [TW] $sym$" | head -3; done; done; echo "vk: link failed ($(wc -l < "$V/link.err") lines of lld output, all in $V/link.err); distinct errors:"; grep -E "error|undefined symbol|duplicate" "$V/link.err" | sort | uniq -c | sort -rn | head -60; return 1; }; cat "$V/link.err"; }
if ! run_link; then
  if [ -n "$USE_CPPRT" ] && grep -q "duplicate symbol" "$V/link.err"; then echo "vk: duplicate symbols with the boilerplate's app_cpp_runtime.o: relinking without it"; USE_CPPRT=; run_link || exit 1; else exit 1; fi
fi
published=$("$NM" -D --defined-only "$V/llvm-pie.elf" | awk '{ n = $NF; sub(/@.*/, "", n); print n }' | sort -u)
if [ -n "$published" ]; then
  echo "   published by the executable (made local, relinking): $(echo $published | tr '\n' ' ')"
  EXTRA_LOCAL="$EXTRA_LOCAL $published"; run_link || exit 1
  published=$("$NM" -D --defined-only "$V/llvm-pie.elf" | awk '{ n = $NF; sub(/@.*/, "", n); print n }' | sort -u)
  [ -z "$published" ] || { echo "vk: still published after making them local: $(echo $published | tr '\n' ' ')"; exit 1; }
fi
null_imports=$(comm -23 \
    <("$NM" -D --undefined-only "$V/llvm-pie.elf" | awk '$1 == "U" { sub(/@.*/, "", $2); print $2 }' | sort -u) \
    <(for l in "$SDK"/target/lib/*.so "$AGC1" "$AGC2"; do
        case ${l##*/} in libkernel_sys.so | libScePosixForWebKit.so) continue;; esac
        "$NM" -D --defined-only "$l" 2>/dev/null | awk '{ print $NF }'
      done | sort -u))
[ -z "$null_imports" ] || { echo "vk: imports no loaded module exports (null at run time): ${null_imports//$'\n'/ }"; exit 1; }
"$TOOL" link --in "$V/llvm-pie.elf" --out "$V/eboot.elf" --stub-dir "$SDK/target/lib" --stub "$AGC1" --stub "$AGC2" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf

echo "== [${SECONDS}s] vk: title folder $TID"
APP=$T/dist/$TID
rm -rf "$APP"; mkdir -p "$APP/sce_sys" "$APP/sce_module"
"$TOOL" self --sign --in "$V/eboot.elf" --out "$APP/eboot.bin" --magic 0x1D3D154F
"$TOOL" self --inspect --file "$APP/eboot.bin" > /dev/null
cp "$T/sce_sys/param.json" "$APP/sce_sys/param.json"
for a in icon0.png pic0.png pic1.png pic0.dds pic1.dds pic2.dds snd0.at9; do [ -f "$T/sce_sys/$a" ] && cp "$T/sce_sys/$a" "$APP/sce_sys/$a"; done
(cd "$VK/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)
cp "$VK/runtime/libc.prx" "$APP/sce_module/libc.prx"
cp -a "$T/assets" "$APP/assets"
echo "   eboot.bin $(stat -c %s "$APP/eboot.bin") bytes, assets $(du -sm "$APP/assets" | cut -f1) MB"
if [ -z "${NODEPLOY:-}" ]; then
  echo "== [${SECONDS}s] vk: deploy to ${PS5:-?} (the Vulkan project's uploader: only adds / replaces files, <app>/mc stays)"
  grep -q "^PS5_HOST=${PS5}\$" "$VK/.env" 2>/dev/null || echo "note: $VK/.env has PS5_HOST=$(sed -n 's/^PS5_HOST=//p' "$VK/.env" 2>/dev/null), not ${PS5:-?}"
  python3 "$VK/tools/deploy-title-folder.py" --always eboot.bin "$APP"
fi
echo "== vk: BUILD FINISHED $(date '+%H:%M:%S') in ${SECONDS}s"
