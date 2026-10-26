#!/bin/bash
# make_vk_title.sh <PC_IP> <PS5_IP>: Java 17 (HotSpot + JDK libs from build_jdk.sh, no GL) + LWJGL 3.3.3 core natives + ps5lwjgl_vk.c (Vulkan-only GLFW shim) linked with RADV through the Vulkan template's link recipe.
#   Experiments A + B (earlier): HotSpot inside the RADV link; vkprobe.c in the same process (VKPROBE=1 runs it before the JVM and after Hello).
#   C (now): Hello runs mode "vk" -> VkTest.java: the real LWJGL Vulkan + GLFW classes (VK.create, glfwCreateWindowSurface through VK_KHR_display, swapchain, 8 s of colour-cycling clears, fps) on RADV.
#   It logs over UDP to <PC_IP>:9999. Nothing of ~/ps5probe is touched. First run clones LWJGL 3.3.3 (~/lwjgl3-3.3.3), libffi 3.4.6 (~/libffi-3.4.6) and fetches three LWJGL jars from Maven Central into ~/lwjgl_jars.
#   Needs: bootstrap + build.sh of ~/PS5_VulkanTemplate done once (RADV archive, SDK pin, native tool), ~/ps5jvm (build_jdk.sh), ~/jdk17u (headers), javac 17.
#   env: TPL=<PS5_VulkanTemplate dir> VK=<PS5_Vulkan dir> JDKDIR=<built JDK dir, default ~/ps5jvm> TITLE=<title id, default PPSA99130 = the template's own folder> XMX=<MB, default 1536> NODEPLOY=1
#   After it: python3 ~/PS5_Vulkan/tools/ps5_console.py launch PPSA99130   (ps5vkctl must be loaded), or start the title from the home screen.
set -euo pipefail
PC=${1:?usage: make_vk_title.sh <PC_IP> <PS5_IP>}
PS5=${2:?usage: make_vk_title.sh <PC_IP> <PS5_IP>}
HERE=$(cd "$(dirname "$0")" && pwd)
TPL=${TPL:-$HOME/PS5_VulkanTemplate}; VK=${VK:-$HOME/PS5_Vulkan}
JDKDIR=${JDKDIR:-$HOME/ps5jvm}; JDKSRC=${JDKSRC:-$HOME/jdk17u}
TITLE=${TITLE:-PPSA99130}; XMX=${XMX:-1536}; VKSECS=${VKSECS:-3}; VKPROBE=${VKPROBE:-0}; LV=${LWJGL_VER:-3.3.3}
VKINC=${VKINC:-$HOME/PS5_Mesa/include}; [ -f "$VKINC/vulkan/vulkan.h" ] || VKINC=$TPL/external
W=$HOME/ps5vk_title
SDK=$(bash "$TPL/ps5/tools/setup-sdk.sh")
ARCHIVE=${RADV_ARCHIVE:-$VK/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a}
TOOL=$VK/build/host/ps5-native-tool; NATIVE=$VK/tooling/native
LLD=$SDK/bin/prospero-lld; NM=$SDK/bin/llvm-nm
# the relocatable JDK object is made with the same tools as make_jvm_title.sh (ps5probe's SDK, else the system LLVM 18); the template SDK's prospero-lld -r produced an object its own linker refused
PSDK=${PROBE_SDK:-$HOME/ps5probe/.deps/native/ps5-payload-sdk}
RLLD=; for c in "$PSDK/bin/ld.lld" /usr/lib/llvm-18/bin/ld.lld "$SDK/bin/ld.lld"; do [ -x "$c" ] && { RLLD=$c; break; }; done
OBJC=; for c in "$PSDK/bin/llvm-objcopy" /usr/lib/llvm-18/bin/llvm-objcopy "$SDK/bin/llvm-objcopy"; do [ -x "$c" ] && { OBJC=$c; break; }; done
[ -n "$RLLD" ] && [ -n "$OBJC" ] || { echo "no ld.lld / llvm-objcopy found (install lld-18 llvm-18 or set PROBE_SDK)"; exit 1; }
for f in "$JDKDIR/whole-hotspot.a" "$ARCHIVE" "$TOOL" "$LLD" "$VK/tools/radv-link.sh" "$VK/runtime/libc.prx" "$TPL/ps5/sce_sys/param.json" \
         "$HERE/jvm_host.cpp" "$HERE/vkprobe.c" "$HERE/ps5lwjgl_vk.c" "$HERE/VkTest.java" "$VKINC/vulkan/vulkan.h" "$HERE/gen_symtab.sh" "$HERE/Hello.java" "$JDKSRC/src/java.base/share/native/include/jni.h"; do
  [ -e "$f" ] || { echo "missing $f (run ps5/tools/bootstrap.sh + build.sh in $TPL, and build_jdk.sh)"; exit 1; }
done
cc() { PS5_PAYLOAD_SDK="$SDK" sh "$VK/tooling/prospero-clang18" "$@"; }
rm -rf "$W"; mkdir -p "$W/obj" "$W/stubs" "$W/inc" "$W/hello"

LWB=${LWB:-$HOME/lwjgl_vk_build}
echo "== [${SECONDS}s] LWJGL $LV core natives + libffi (cached in $LWB) and the Vulkan GLFW shim"
 M=$HOME/lwjgl3-$LV/modules/lwjgl; F=$HOME/libffi-3.4.6
cp "$JDKSRC/src/java.base/share/native/include/jni.h" "$JDKSRC/src/java.base/unix/native/include/jni_md.h" "$W/inc/"
[ -d "$M" ] || git clone -q --depth 1 --branch "$LV" https://github.com/LWJGL/lwjgl3 "$HOME/lwjgl3-$LV"
[ -d "$F" ] || curl -sL https://github.com/libffi/libffi/releases/download/v3.4.6/libffi-3.4.6.tar.gz | tar -xz -C "$HOME"
AR=; for c in "$SDK/bin/llvm-ar" "$PSDK/bin/llvm-ar" /usr/lib/llvm-18/bin/llvm-ar; do [ -x "$c" ] && { AR=$c; break; }; done; [ -n "$AR" ] || { echo "no llvm-ar found"; exit 1; }
mkdir -p "$LWB/shim" "$LWB/ffi"
printf '#pragma once\n#include <stdlib.h>\n#ifndef alloca\n#define alloca(n) __builtin_alloca(n)\n#endif\n' > "$LWB/shim/alloca.h"
if [ -z "${FORCE:-}" ] && [ -f "$LWB/.done-$LV" ]; then echo "   natives: cached ($(ls "$LWB/obj" | wc -l) objects)"; else
  rm -rf "$LWB/obj" "$LWB/.done-$LV"; mkdir -p "$LWB/obj"
  # -DX86_64: LWJGL's bundled ffitarget.h only picks the 64-bit ABI with it; -Djvm=lwjgl_jvm: the JDK side owns the global "jvm" too
  FL="-std=gnu11 -O2 -fPIC -DX86_64 -Djvm=lwjgl_jvm -DLWJGL_LINUX -D_ALLBSD_SOURCE -include string.h -I$LWB/shim -I$W/inc -I$M/core/src/main/c -I$M/core/src/main/c/linux -I$M/core/src/main/c/libffi -I$M/core/src/main/c/libffi/x86"
  ls "$M"/core/src/main/c/*.c "$M"/core/src/generated/c/*.c "$M/core/src/generated/c/linux/org_lwjgl_system_linux_DynamicLinkLoader.c" \
    | xargs -P 8 -I{} sh -c 'f={}; b=$(basename $f .c); PS5_PAYLOAD_SDK="$0" sh "$1" $2 -c $f -o "$3/$b.o" 2>"$3/$b.err" || { echo "FAILED $b"; head -5 "$3/$b.err"; exit 255; }' "$SDK" "$VK/tooling/prospero-clang18" "$FL" "$LWB/obj"
  FD=$LWB/ffi
  sed -e 's/@VERSION@/3.4.6/' -e 's/@TARGET@/X86_64/' -e 's/@HAVE_LONG_DOUBLE@/1/' -e 's/@FFI_EXEC_TRAMPOLINE_TABLE@/0/' "$F/include/ffi.h.in" > "$FD/ffi.h"
  cp "$F/src/x86/ffitarget.h" "$FD/ffitarget.h"
  cat > "$FD/fficonfig.h" <<'CFG'
#define HAVE_ALLOCA 1
#define HAVE_ALLOCA_H 1
#define HAVE_MMAP 1
#define HAVE_MMAP_ANON 1
#define HAVE_MEMCPY 1
#define STDC_HEADERS 1
#define HAVE_AS_X86_PCREL 1
#define HAVE_AS_ASCII_PSEUDO_OP 1
#define HAVE_AS_STRING_PSEUDO_OP 1
#define EH_FRAME_FLAGS "a"
#define HAVE_HIDDEN_VISIBILITY_ATTRIBUTE 1
#ifdef LIBFFI_ASM
#define FFI_HIDDEN(name) .hidden name
#else
#define FFI_HIDDEN __attribute__((visibility("hidden")))
#endif
#define FFI_EXEC_TRAMPOLINE_TABLE 0
CFG
  FFL="-std=gnu11 -O2 -fPIC -DHAVE_CONFIG_H -I$LWB/shim -I$FD -I$F/src/x86 -I$F/include -I$F/src"
  for f in prep_cif types x86/ffi64; do cc $FFL -c "$F/src/$f.c" -o "$LWB/obj/ffi_$(basename $f).o"; done   # closures.c is left out: ps5lwjgl_vk.c serves closures from RWX memory
  cc $FFL -c "$F/src/x86/unix64.S" -o "$LWB/obj/ffi_unix64.o"
  touch "$LWB/.done-$LV"; echo "   natives: $(ls "$LWB/obj" | wc -l) objects built"
fi
cc -std=gnu11 -O2 -fPIC -w -I"$W/inc" -I"$VKINC" -c "$HERE/ps5lwjgl_vk.c" -o "$W/obj/ps5lwjgl_vk.o"
mkdir -p "$W/lw"; "$AR" rcs "$W/lw/liblwjgl_ps5.a" "$LWB"/obj/*.o "$W/obj/ps5lwjgl_vk.o"   # the symbol table (gen_symtab.sh) lists the JNI names from this archive

echo "== [${SECONDS}s] JDK object: HotSpot + JDK libraries + symbol table (with the LWJGL JNI names), globals made local except the entry points"
LIBS=""; for l in java zip net nio jimage verify management management_ext sunec fdlibm; do [ -f "$JDKDIR/lib$l.a" ] && LIBS="$LIBS $JDKDIR/lib$l.a"; done
bash "$HERE/gen_symtab.sh" "$SDK" "$JDKDIR" "$W/symtab.c" "$W/lw"
cc -c -O1 -w "$W/symtab.c" -o "$W/obj/symtab.o"
"$RLLD" -r -m elf_x86_64 -o "$W/pre.o" "$W/obj/symtab.o" --whole-archive "$JDKDIR/whole-hotspot.a" --no-whole-archive --start-group $LIBS --end-group
"$NM" -g --defined-only -j "$W/pre.o" | LC_ALL=C sort -u > "$W/defined.txt"
printf '%s\n' ps5_trace ps5_trace_bytes nl_langinfo | LC_ALL=C sort -u > "$W/want.txt"   # defined by the JDK, used by the host
LC_ALL=C comm -12 "$W/defined.txt" "$W/want.txt" > "$W/shared.txt"
LC_ALL=C comm -23 "$W/defined.txt" "$W/shared.txt" | grep -v -x -e JNI_CreateJavaVM -e ps5_set_java_home -e ps5_trace_hook -e ps5_dll_handle -e ps5_dlsym > "$W/local.txt" || true
"$OBJC" --redefine-sym JNI_CreateJavaVM=JNI_CreateJavaVM_17 --redefine-sym ps5_set_java_home=ps5_set_java_home_17 --redefine-sym ps5_trace_hook=ps5_trace_hook_17 \
        --redefine-sym ps5_dll_handle=ps5_dll_handle_17 --redefine-sym ps5_dlsym=ps5_dlsym_17 --localize-symbols="$W/local.txt" "$W/pre.o" "$W/obj/jdk17.o"
rm -f "$W/pre.o"
echo "   tools: $RLLD / $OBJC"; echo "   jdk17.o: $(du -m "$W/obj/jdk17.o" | cut -f1) MB, shared with the host: $(tr '\n' ' ' < "$W/shared.txt"), $(wc -l < "$W/local.txt") symbols made local"

echo "== [${SECONDS}s] host (jvm_host.cpp)"
{ printf '#define LOG_HOST "%s"\n' "$PC"; echo '#define JDK_MASK 1'; printf '#define TITLE_ID "%s"\n' "$TITLE"; echo '#define USE_JIT 1'; printf '#define XMX_MB %s\n' "$XMX"; [ "$VKPROBE" = 1 ] && { echo '#define VKPROBE 1'; printf '#define VKPROBE_SECS %s\n' "$VKSECS"; }
  echo '#define CLASS_PATH "/app0/assets/hello:/app0/assets/lib/lwjgl.jar:/app0/assets/lib/lwjgl-glfw.jar:/app0/assets/lib/lwjgl-vulkan.jar"'; } > "$W/inc/ps5_conf.h"
cc -std=gnu++17 -O1 -w -I"$W/inc" -c "$HERE/jvm_host.cpp" -o "$W/obj/jvm_host.o"
PROBEO=(); [ "$VKPROBE" = 1 ] && { cc -std=gnu11 -O2 -w -I"$VKINC" -c "$HERE/vkprobe.c" -o "$W/obj/vkprobe.o"; PROBEO=("$W/obj/vkprobe.o"); }

echo "== [${SECONDS}s] CRT + AGC import stubs (as the template's link-title.sh)"
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$NATIVE/app_crt.cpp" -o "$W/obj/app_crt.o"
stub() { cc -std=c11 -O2 -fPIC -c "$VK/$2" -o "$W/obj/$1_stub.o"; "$LLD" --shared -soname "$1.prx" -o "$W/stubs/$1.so" "$W/obj/$1_stub.o"; }
stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c

echo "== [${SECONDS}s] link with RADV (the template's recipe; --no-dynamic-linker keeps Mesa's weak dispatch references out of the dynamic table)"
source "$VK/tools/radv-link.sh"
radv_link_recipe "$VK" "$SDK" "$ARCHIVE" || exit 2
EXTRA_LOCAL=""
if grep -q " T ps5_localeconv$" <<<"$("$NM" --defined-only "$SDK/target/lib/libps5platform.a" 2>/dev/null)" && [[ " ${radv_link_flags[*]} " != *" --defsym=localeconv=ps5_localeconv "* ]]; then
  radv_link_flags+=(--defsym=localeconv=ps5_localeconv); EXTRA_LOCAL="localeconv"
fi
# The converter refuses any symbol the executable PUBLISHES ("does not yet publish application exports"). A symbol the executable defines that a system stub also exports (dlopen, dlsym ...)
# lands in the dynamic table; version-script it local (the executable still binds to its own copy). Link, list what got published, relink with those made local.
link_elf() {
  { printf '{\n    local:\n        _Zn*;\n        _Zd*;\n'; for n in $EXTRA_LOCAL; do printf '        %s;\n' "$n"; done; printf '};\n'; } > "$W/vk-symbols.map"
  "$LLD" "${radv_linker_script[@]}" --eh-frame-hdr --no-dynamic-linker "${radv_link_flags[@]}" \
    --version-script "$W/vk-symbols.map" --exclude-libs=ALL -e _start -o "$W/llvm-pie.elf" \
    "$W/obj/app_crt.o" "$W/obj/jvm_host.o" "$W/obj/ps5lwjgl_vk.o" "$LWB"/obj/*.o "${PROBEO[@]}" "$W/obj/jdk17.o" \
    "$W/stubs/libSceAgc.so" "$W/stubs/libSceAgcDriver.so" "${radv_link_inputs[@]}" --as-needed "$SDK"/target/lib/*.so
}
link_elf
published=$("$NM" -D --defined-only "$W/llvm-pie.elf" | awk '{ n = $NF; sub(/@.*/, "", n); print n }' | sort -u)
if [ -n "$published" ]; then
  echo "   published by the executable (made local, relinking): $(echo $published | tr '\n' ' ')"
  EXTRA_LOCAL="$EXTRA_LOCAL $published"; link_elf
  published=$("$NM" -D --defined-only "$W/llvm-pie.elf" | awk '{ n = $NF; sub(/@.*/, "", n); print n }' | sort -u)
  [ -z "$published" ] || { echo "still published after making them local: $(echo $published | tr '\n' ' ')"; exit 1; }
fi
null_imports=$(comm -23 \
    <("$NM" -D --undefined-only "$W/llvm-pie.elf" | awk '$1 == "U" { sub(/@.*/, "", $2); print $2 }' | sort -u) \
    <(for l in "$SDK"/target/lib/*.so "$W/stubs/libSceAgc.so" "$W/stubs/libSceAgcDriver.so"; do
        case ${l##*/} in libkernel_sys.so | libScePosixForWebKit.so) continue;; esac
        "$NM" -D --defined-only "$l" 2>/dev/null | awk '{ print $NF }'
      done | sort -u))
[ -z "$null_imports" ] || { echo "imports no loaded module exports (null at run time): ${null_imports//$'\n'/ }"; exit 1; }
"$TOOL" link --in "$W/llvm-pie.elf" --out "$W/eboot.elf" --stub-dir "$SDK/target/lib" --stub "$W/stubs/libSceAgc.so" --stub "$W/stubs/libSceAgcDriver.so" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf

echo "== [${SECONDS}s] title folder $TITLE"
APP=$W/dist/$TITLE
mkdir -p "$APP/sce_sys" "$APP/sce_module" "$APP/assets/hello"
"$TOOL" self --sign --in "$W/eboot.elf" --out "$APP/eboot.bin" --magic 0x1D3D154F
"$TOOL" self --inspect --file "$APP/eboot.bin" > /dev/null
python3 - "$TPL/ps5/sce_sys/param.json" "$APP/sce_sys/param.json" "$TITLE" <<'PY'
import json, re, sys
d = json.load(open(sys.argv[1])); t = sys.argv[3]
def walk(o):
    if isinstance(o, dict):
        for k, v in o.items(): o[k] = re.sub(r"PPSA\d{5}", t, v) if isinstance(v, str) else walk(v) or v
    elif isinstance(o, list):
        for i, v in enumerate(o): o[i] = re.sub(r"PPSA\d{5}", t, v) if isinstance(v, str) else walk(v) or v
walk(d); json.dump(d, open(sys.argv[2], "w"), indent=2)
PY
for a in icon0.png pic0.dds pic1.dds snd0.at9; do [ -f "$TPL/ps5/sce_sys/$a" ] && cp "$TPL/ps5/sce_sys/$a" "$APP/sce_sys/$a"; done
(cd "$VK/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)
cp "$VK/runtime/libc.prx" "$APP/sce_module/libc.prx"
JARS=${LWJGL_JARS:-$HOME/lwjgl_jars}; mkdir -p "$JARS" "$APP/assets/lib"
for m in lwjgl lwjgl-glfw lwjgl-vulkan; do
  [ -s "$JARS/$m-$LV.jar" ] || curl -fsSL -o "$JARS/$m-$LV.jar" "https://repo1.maven.org/maven2/org/lwjgl/$m/$LV/$m-$LV.jar" || { rm -f "$JARS/$m-$LV.jar"; echo "could not fetch $m-$LV.jar (put the LWJGL $LV jars into $JARS)"; exit 1; }
  cp "$JARS/$m-$LV.jar" "$APP/assets/lib/$m.jar"
done
/usr/lib/jvm/java-17-openjdk-amd64/bin/javac --release 8 -nowarn -cp "$JARS/lwjgl-$LV.jar:$JARS/lwjgl-glfw-$LV.jar:$JARS/lwjgl-vulkan-$LV.jar" -d "$APP/assets/hello" "$HERE/Hello.java" "$HERE/VkTest.java" 2>&1 | { grep -v -e warning -e '^Note' || true; }
echo vk > "$APP/assets/hello/mode.txt"
rsync -a --delete "$JDKDIR/jdk/" "$APP/assets/jdk17/"
echo "   eboot.bin $(stat -c %s "$APP/eboot.bin") bytes, assets/jdk17 $(du -sm "$APP/assets/jdk17" | cut -f1) MB"

if [ -z "${NODEPLOY:-}" ]; then
  echo "== [${SECONDS}s] deploy to $PS5 (ftpsrv from PS5_Vulkan's .env: PS5_HOST must be $PS5)"
  grep -q "^PS5_HOST=$PS5\$" "$VK/.env" 2>/dev/null || { echo "note: $VK/.env has PS5_HOST=$(sed -n 's/^PS5_HOST=//p' "$VK/.env" 2>/dev/null), not $PS5"; }
  python3 "$VK/tools/deploy-title-folder.py" --always eboot.bin "$APP"
fi
echo "== BUILD FINISHED $(date '+%H:%M:%S') in ${SECONDS}s. Start the PowerShell listener (port 9999), then: python3 $VK/tools/ps5_console.py launch $TITLE"
