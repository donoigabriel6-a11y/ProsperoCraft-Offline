#!/bin/bash
# make_mcvk_title.sh <PC_IP> <PS5_IP>: the Vulkan title. Java 25 (HotSpot + JDK libs from build_jdk.sh) + LWJGL 3.4.3 natives (core, stb, VMA, FreeType, shaderc / SPIRV-Cross) + ps5lwjgl.c built with -DPS5_VK
#   (the same shim as the OpenGL title: pad -> keys / mouse, SDL3 subset, OpenAL on sceAudioOut, sockets; but no EGL, the SDL_Vulkan_* functions and a VK_KHR_display surface on RADV, see ps5vk.inc)
#   linked with RADV through the Vulkan template's link recipe. It runs Minecraft 26.x's OWN Vulkan renderer (com.mojang.renderpearl.backend.vulkan, "Graphics API" video setting).
#   McBoot / Launcher / McDownload are the OpenGL title's Java classes; this title has no native launcher UI (-Dps5.noui=1): it starts the instance named in <app>/mc/launcher.cfg directly, and the first start installs
#   what <app>/mc/download.txt asks for (default: 26.3). Leaving the game (or the pad quit combo) launches the OpenGL title (RETURN_TITLE_ID, default PPSA27272).
#   Logs go over UDP to <PC_IP>:9999. Nothing of ~/ps5probe is touched. The OpenGL title is not touched either.
#   Needs, once: bootstrap + build.sh of ~/PS5_VulkanTemplate (RADV archive, SDK pin, native tool); build_jdk.sh for Java 25 (~/ps5jvm25 or ~/ps5jvm25c2, ~/jdk25u); the OpenGL title built once with LWJGL_VERS "3.3.3 3.4.3"
#   (it leaves FreeType in ~/lwjgl_build, shaderc / SPIRV-Cross in ~/spirv_build, desktop-lite.jar in ~/desktop_lite_c, the LWJGL 3.4.3 jars in ~/lwjgl_jars_3.4.3 and the LWJGL 3.4.3 sources in ~/lwjgl3-3.4.3).
#   env: TPL VK (the template / PS5_Vulkan dirs) JDKDIR (default ~/ps5jvm25c2 if built, else ~/ps5jvm25) JDKSRC (~/jdk25u) TITLE (PPSA99130) RETURN_TITLE_ID (PPSA27272) XMX (2560) MCVER (26.3, what download.txt installs)
#        NODEPLOY=1 FORCE=1 (rebuild the cached LWJGL natives)
#   AIO=1: link-feasibility experiment for one title that carries BOTH renderers: the OpenGL SDK (~/glsdk) is linked next to RADV, each as its own relocatable object with everything local except the
#   symbols the rest of the program uses (the JDK trick). Title PPSA99131, work dir ~/ps5aio_title; it still RUNS the Vulkan path only. PPSA27272 and PPSA99130 are not touched.
#   AIO=1 now also picks the renderer at start-up: <app>/mc/launcher.cfg renderer=gl|vulkan (the Launcher's 'config renderer gl' download.txt line writes it), else RENDERER=gl|vulkan given at build time (default vulkan). A change needs a restart of the title.
#   GLPROBE=1 (implies AIO=1): the title does not start the game; it runs ps5lwjgl.c's native OpenGL clear loop (180 frames) on the linked SDK and logs 'selftest:' / 'GLPROBE' lines: does the GL stack work inside this ELF?
#   After it: python3 ~/PS5_Vulkan/tools/ps5_console.py launch PPSA99130   (ps5vkctl must be loaded: nc -w 2 <PS5_IP> 9021 < ~/PS5_Vulkan/build/ps5vkctl/ps5vkctl.elf)
set -euo pipefail
trap 'echo "FAILED (exit $?) at line $LINENO: $BASH_COMMAND" >&2' ERR
[ -z "${ZPROBE:-}" ] || ZINK=1   # ZPROBE=1: AIO title whose start-up runs the native Zink probe (EGL surfaceless + Zink on RADV, no JVM)
[ -z "${ZINK:-}" ] || AIO=1   # ZINK=1: also link ~/zink_obj/zink.o (make_zink_obj.sh): Mesa Zink + EGL, OpenGL on top of RADV
[ -z "${GLPROBE:-}" ] || AIO=1   # GLPROBE=1: AIO title whose start-up runs the native OpenGL clear loop on the SDK linked next to RADV (no JVM)
PC=${1:?usage: make_mcvk_title.sh <PC_IP> <PS5_IP>}
PS5=${2:?usage: make_mcvk_title.sh <PC_IP> <PS5_IP>}
HERE=$(cd "$(dirname "$0")" && pwd)
TPL=${TPL:-$HOME/PS5_VulkanTemplate}; VK=${VK:-$HOME/PS5_Vulkan}
JDKDIR=${JDKDIR:-}; [ -n "$JDKDIR" ] || { for d in $HOME/ps5jvm25c2 $HOME/ps5jvm25; do [ -f "$d/whole-hotspot.a" ] && { JDKDIR=$d; break; }; done; }
JDKSRC=${JDKSRC:-$HOME/jdk25u}
TITLE=${TITLE:-$([ -n "${AIO:-}" ] && echo PPSA99131 || echo PPSA99130)}; RETURN_TITLE_ID=${RETURN_TITLE_ID:-PPSA27272}; XMX=${XMX:-2560}; MCVER=${MCVER:-26.3}; LV=3.4.3
VKINC=${VKINC:-$HOME/PS5_Mesa/include}; [ -f "$VKINC/vulkan/vulkan.h" ] || VKINC=$TPL/external
W=$HOME/ps5$([ -n "${AIO:-}" ] && echo aio || echo mcvk)_title
SDK=$(bash "$TPL/ps5/tools/setup-sdk.sh")
ARCHIVE=${RADV_ARCHIVE:-$VK/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a}
TOOL=$VK/build/host/ps5-native-tool; NATIVE=$VK/tooling/native
LLD=$SDK/bin/prospero-lld; NM=$SDK/bin/llvm-nm
PSDK=${PROBE_SDK:-$HOME/ps5probe/.deps/native/ps5-payload-sdk}
RLLD=; for c in "$PSDK/bin/ld.lld" /usr/lib/llvm-18/bin/ld.lld "$SDK/bin/ld.lld"; do [ -x "$c" ] && { RLLD=$c; break; }; done
OBJC=; for c in "$PSDK/bin/llvm-objcopy" /usr/lib/llvm-18/bin/llvm-objcopy "$SDK/bin/llvm-objcopy"; do [ -x "$c" ] && { OBJC=$c; break; }; done
[ -n "$RLLD" ] && [ -n "$OBJC" ] || { echo "no ld.lld / llvm-objcopy found (install lld-18 llvm-18 or set PROBE_SDK)"; exit 1; }
AR=; for c in "$SDK/bin/llvm-ar" "$PSDK/bin/llvm-ar" /usr/lib/llvm-18/bin/llvm-ar; do [ -x "$c" ] && { AR=$c; break; }; done; [ -n "$AR" ] || { echo "no llvm-ar found"; exit 1; }
M=$HOME/lwjgl3-$LV/modules/lwjgl; F=$HOME/libffi-3.4.6
FTC=$(ls -dt $HOME/lwjgl_build/cache/freetype-*/ 2>/dev/null | while read d; do [ -f "$d/.done" ] && { echo "$d"; break; }; done)
FTSYMS=$HOME/lwjgl_build/ps5ft_syms.inc; SPB=$HOME/spirv_build
GLINC=; for v in 1.0.2 1.0.1 1.0.0 0.3.0; do d=$HOME/glsdk/ps5-opengl-sdk-$v/sdk/include; [ -f "$d/EGL/egl.h" ] && { GLINC=$d; break; }; done
[ -n "$GLINC" ] || GLINC=$HERE/tests/inc   # only the EGL headers are needed to compile the (never used) GL code
for f in "$JDKDIR/whole-hotspot.a" "$ARCHIVE" "$TOOL" "$LLD" "$VK/tools/radv-link.sh" "$VK/runtime/libc.prx" "$TPL/ps5/sce_sys/param.json" "$HERE/jvm_host.cpp" "$HERE/ps5lwjgl.c" "$HERE/ps5vk.inc" "$HERE/ps5vk_stubs.c" \
         "$HERE/gen_symtab.sh" "$HERE/Hello.java" "$HERE/McBoot.java" "$HERE/log4j2-ps5.xml" "$VKINC/vulkan/vulkan.h" "$JDKSRC/src/java.base/share/native/include/jni.h" "$M/core/src/main/c/common_tools.h" "$FTSYMS" "$SPB/ps5spirv_syms.inc" "$GLINC/EGL/egl.h"; do
  [ -e "$f" ] || { echo "missing $f (see the header of this script: template bootstrap, build_jdk.sh for Java 25, the OpenGL title built once with LWJGL 3.4.3)"; exit 1; }
done
[ -n "$FTC" ] && [ -f "$SPB/done" ] || { echo "FreeType / SPIRV objects are not cached (~/lwjgl_build/cache/freetype-*, ~/spirv_build/done): build the OpenGL launcher title once first"; exit 1; }
HASC2=; case "$JDKDIR" in *c2) HASC2=1;; esac
cc() { PS5_PAYLOAD_SDK="$SDK" sh "$VK/tooling/prospero-clang18" "$@"; }
rm -rf "$W"; mkdir -p "$W/obj" "$W/stubs" "$W/inc" "$W/lw"
echo "== [${SECONDS}s] Java 25 from $JDKDIR (${HASC2:+C2}${HASC2:-C1 only}), LWJGL $LV, Vulkan headers $VKINC, EGL headers $GLINC"

# ---------------- LWJGL 3.4.3 natives: core + libffi + stb + VMA (cached; FORCE=1 rebuilds) ----------------
LWB=${LWB:-$HOME/lwjgl_mcvk_build}
cp "$JDKSRC/src/java.base/share/native/include/jni.h" "$JDKSRC/src/java.base/unix/native/include/jni_md.h" "$W/inc/"
[ -d "$M" ] || git clone -q --depth 1 --branch "$LV" https://github.com/LWJGL/lwjgl3 "$HOME/lwjgl3-$LV"
[ -d "$F" ] || curl -sL https://github.com/libffi/libffi/releases/download/v3.4.6/libffi-3.4.6.tar.gz | tar -xz -C "$HOME"
mkdir -p "$LWB/shim" "$LWB/ffi"
printf '#pragma once\n#include <stdlib.h>\n#ifndef alloca\n#define alloca(n) __builtin_alloca(n)\n#endif\n' > "$LWB/shim/alloca.h"
LWREV=rev4   # rev4: + the lwjgl-opengl natives (the AIO title runs the OpenGL backend; GL.create finds the entry points through the SDK). rev3: VMA built with -DNDEBUG (its debug assert on vkGetPhysicalDeviceProperties2KHR killed the first Vulkan start; LWJGL fills only the core pointers, desktop builds have asserts off)
if [ -z "${FORCE:-}" ] && [ -f "$LWB/.done-$LV-$LWREV" ]; then echo "   natives: cached ($(ls "$LWB/obj" | wc -l) objects)"; else
  rm -rf "$LWB/obj" "$LWB"/.done-*; mkdir -p "$LWB/obj"
  FL="-O2 -fPIC -DX86_64 -Djvm=lwjgl_jvm -DLWJGL_LINUX -D_ALLBSD_SOURCE -include string.h -I$LWB/shim -I$W/inc -I$M/core/src/main/c -I$M/core/src/main/c/linux -I$M/core/src/main/c/libffi -I$M/core/src/main/c/libffi/x86 -I$M/stb/src/main/c -I$M/stb/src/generated/c -I$M/vma/src/main/c -I$M/vma/src/generated/c -I$M/opengl/src/main/c -I$M/opengl/src/generated/c -I$M/vulkan/src/main/c"
  { ls "$M"/core/src/main/c/*.c "$M"/core/src/generated/c/*.c "$M/core/src/generated/c/linux/org_lwjgl_system_linux_DynamicLinkLoader.c"
    ls "$M/core/src/generated/c/freebsd/org_lwjgl_system_freebsd_DynamicLinkLoader.c" 2>/dev/null || true   # 3.4 has its own FreeBSD platform: the console reports os.name FreeBSD
    find "$M/stb" -name '*.c' ! -name stb_vorbis.c
    find "$M/opengl" -name '*.c' ! -name '*WGL*' ! -name 'WindowsLWJGL*' ! -path '*windows*' ! -path '*macosx*' 2>/dev/null
    ls "$M"/vma/src/generated/c/*.cpp; } \
    | xargs -P 8 -I{} sh -c 'f={}; b=$(basename $f); b=${b%.*}; case $f in *.cpp) std="-std=gnu++17 -fno-exceptions -fno-rtti";; *) std="-std=gnu11";; esac; case $f in */stb/*) m=stb;; */vma/*) m=vma;; *) m=;; esac; if [ -n "$m" ]; then for n in malloc calloc realloc free aligned_alloc aligned_free; do std="$std -Dorg_lwjgl_$n=org_lwjgl_${m}_$n"; done; fi; [ "$m" = vma ] && std="$std -DNDEBUG"; PS5_PAYLOAD_SDK="$0" sh "$1" $std $2 -c $f -o "$3/$b.o" 2>"$3/$b.err" || { echo "FAILED $b"; head -8 "$3/$b.err"; exit 255; }' "$SDK" "$VK/tooling/prospero-clang18" "$FL" "$LWB/obj"
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
  for f in prep_cif types x86/ffi64; do cc $FFL -c "$F/src/$f.c" -o "$LWB/obj/ffi_$(basename $f).o"; done   # closures.c is left out: ps5lwjgl.c serves closures from RWX memory
  cc $FFL -c "$F/src/x86/unix64.S" -o "$LWB/obj/ffi_unix64.o"
  touch "$LWB/.done-$LV-$LWREV"; echo "   natives: $(ls "$LWB/obj" | wc -l) objects built"
fi

# ---------------- the shim: ps5lwjgl.c with -DPS5_VK ----------------
echo "== [${SECONDS}s] shim (ps5lwjgl.c -DPS5_VK) + stubs"
cc -std=gnu11 -O2 -fPIC -DPS5_VK ${ZINK:+-DPS5_ZINK} -DNO_VIDEO_WRAPS -DNO_PREPARE_FIX -DPS5_FREETYPE -DPS5_SPIRV -I"$HOME/lwjgl_build" -I"$SPB" -I"$HERE" -I"$VKINC" -I"$GLINC" -I"$W/inc" -I"$LWB/ffi" -c "$HERE/ps5lwjgl.c" -o "$W/obj/ps5lwjgl.o"
cc -std=gnu11 -O2 -fPIC -w ${AIO:+-DAIO} ${ZINK:+-DPS5_ZINK} -c "$HERE/ps5vk_stubs.c" -o "$W/obj/ps5vk_stubs.o"
# every __wrap_X the shim defines needs --wrap=X at the link (and in the JDK's relocatable link, so the JDK's own calls are wrapped too)
WRAPS=$("$NM" --defined-only "$W/obj/ps5lwjgl.o" | awk '$3 ~ /^__wrap_[A-Za-z0-9_]+$/ { sub(/^__wrap_/, "", $3); print "--wrap=" $3 }' | tr '\n' ' ')
echo "   wraps: $WRAPS"
FTO=$(ls "$FTC"/ft_*.o); SPO=$(ls "$SPB"/obj/*.o)
cp "$FTSYMS" "$W/inc/" 2>/dev/null || true
"$AR" rcs "$W/lw/liblwjgl_ps5.a" "$LWB"/obj/*.o "$W/obj/ps5lwjgl.o"   # the symbol table (gen_symtab.sh) lists the JNI names from this archive

echo "== [${SECONDS}s] JDK object: HotSpot 25 + JDK libraries + symbol table (with the LWJGL JNI names)"
LIBS=""; for l in java zip net nio jimage verify management management_ext sunec; do [ -f "$JDKDIR/lib$l.a" ] && LIBS="$LIBS $JDKDIR/lib$l.a"; done
bash "$HERE/gen_symtab.sh" "$SDK" "$JDKDIR" "$W/symtab.c" "$W/lw"
cc -c -O1 -w "$W/symtab.c" -o "$W/obj/symtab.o"
"$RLLD" -r -m elf_x86_64 $WRAPS -o "$W/pre.o" "$W/obj/symtab.o" --whole-archive "$JDKDIR/whole-hotspot.a" --no-whole-archive --start-group $LIBS --end-group
echo "   pre.o getaddrinfo refs: $("$NM" "$W/pre.o" | grep -i getaddrinfo | tr '\n' ';')"
"$NM" -g --defined-only -j "$W/pre.o" | LC_ALL=C sort -u > "$W/defined.txt"
{ "$NM" -u -j "$W/lw/liblwjgl_ps5.a" $FTO $SPO 2>/dev/null; printf '%s\n' ps5_trace ps5_trace_bytes nl_langinfo; } | grep -v -e ':$' -e '^$' | LC_ALL=C sort -u > "$W/want.txt"   # defined by the JDK, used by the shim / host
LC_ALL=C comm -12 "$W/defined.txt" "$W/want.txt" > "$W/shared.txt"
LC_ALL=C comm -23 "$W/defined.txt" "$W/shared.txt" | grep -v -x -e JNI_CreateJavaVM -e ps5_set_java_home -e ps5_trace_hook -e ps5_dll_handle -e ps5_dlsym > "$W/local.txt" || true
"$OBJC" --redefine-sym JNI_CreateJavaVM=JNI_CreateJavaVM_25 --redefine-sym ps5_set_java_home=ps5_set_java_home_25 --redefine-sym ps5_trace_hook=ps5_trace_hook_25 \
        --redefine-sym ps5_dll_handle=ps5_dll_handle_25 --redefine-sym ps5_dlsym=ps5_dlsym_25 --localize-symbols="$W/local.txt" "$W/pre.o" "$W/obj/jdk25.o"
rm -f "$W/pre.o"
echo "   jdk25.o: $(du -m "$W/obj/jdk25.o" | cut -f1) MB, shared with the shim: $(tr '\n' ' ' < "$W/shared.txt"), $(wc -l < "$W/local.txt") symbols made local"

echo "== [${SECONDS}s] host (jvm_host.cpp)"
{ printf '#define LOG_HOST "%s"\n' "$PC"; echo '#define JDK_MASK 4'; printf '#define TITLE_ID "%s"\n#define RETURN_TITLE_ID "%s"\n' "$TITLE" "$RETURN_TITLE_ID"; echo '#define USE_JIT 1'; echo '#define GC_PARALLEL 1'; echo '#define MC_VK 1'
  printf '#define XMX_MB %s\n' "$XMX"; [ -n "$HASC2" ] && echo '#define HAS_C2 1'
  echo '#define MC_CP "/app0/assets/hello"'
  echo '#define CLASS_PATH "/app0/assets/hello:/app0/assets/lib/lwjgl.jar:/app0/assets/lib/lwjgl-stb.jar"';
  [ "${NETPROBE:-}" = 1 ] && echo '#define NETPROBE 1'; [ -n "${GLPROBE:-}" ] && echo '#define GLPROBE 1'; [ -n "${AIO:-}" ] && echo '#define AIO_RENDERER 1'; [ -n "${ZINK:-}" ] && echo '#define PS5_ZINK 1'; [ -n "${ZPROBE:-}" ] && echo '#define ZPROBE 1'; [ -z "${RENDERER:-}" ] || printf '#define DEFAULT_RENDERER "%s"\n' "$RENDERER"; true; } > "$W/inc/ps5_conf.h"
cc -std=gnu++17 -O1 -w -I"$W/inc" -c "$HERE/jvm_host.cpp" -o "$W/obj/jvm_host.o"

GLO=; AGC1=$W/stubs/libSceAgc.so; AGC2=$W/stubs/libSceAgcDriver.so
if [ -n "${AIO:-}" ]; then
  echo "== [${SECONDS}s] AIO: OpenGL SDK as a relocatable object next to RADV"
  GLSDK=; for V in ${GLSDK_VER:-1.0.2 1.0.1 1.0.0}; do d=$HOME/glsdk/ps5-opengl-sdk-$V/sdk; [ -f "$d/lib/libps5_opengl_core33.a" ] && { GLSDK=$d; break; }; done
  [ -n "$GLSDK" ] || { echo "AIO=1 needs the OpenGL SDK under ~/glsdk (install steps: make_jvm_title.sh)"; exit 1; }
  GLC=$(sed -n 's/^ *\(lib[A-Za-z0-9_.+-]*\.a\) *$/\1/p' "$GLSDK/lib/libPS5OpenGL.a" | while read -r l; do printf '%s ' "$GLSDK/lib/$l"; done)
  [ -n "$GLC" ] || { echo "no component list in $GLSDK/lib/libPS5OpenGL.a"; exit 1; }
  echo "   SDK $GLSDK: $(echo $GLC | wc -w) component archives"
  "$NM" -u -j "$W/obj/ps5lwjgl.o" "$W/obj/ps5vk_stubs.o" "$W/obj/jvm_host.o" "$W/obj/jdk25.o" "$LWB"/obj/*.o $FTO $SPO 2>/dev/null | grep -v -e ':$' -e '^$' | LC_ALL=C sort -u > "$W/gl_want.txt"
  "$NM" -g --defined-only -j $GLC 2>/dev/null | grep -v -e ':$' -e '^$' | LC_ALL=C sort -u > "$W/gl_arch_defined.txt"
  { LC_ALL=C comm -12 "$W/gl_arch_defined.txt" "$W/gl_want.txt"; grep -e '^egl' -e '^ps5_opengl_' -e '^glX' "$W/gl_arch_defined.txt"; } | LC_ALL=C sort -u > "$W/gl_roots.txt"
  echo "   GL roots (undefined in the program, defined by the SDK): $(wc -l < "$W/gl_roots.txt"): $(head -c 400 "$W/gl_roots.txt" | tr '\n' ' ')"
  GLU=$(sed 's/^/-u /' "$W/gl_roots.txt" | tr '\n' ' ')
  "$RLLD" -r -m elf_x86_64 -o "$W/gl_pre.o" $GLU --start-group $GLC --end-group 2>"$W/gl_pre.err" || { echo "relocatable link of the GL SDK failed:"; sort "$W/gl_pre.err" | uniq -c | sort -rn | head -40; exit 1; }
  "$NM" -g --defined-only -j "$W/gl_pre.o" | LC_ALL=C sort -u > "$W/gl_defined.txt"
  LC_ALL=C comm -12 "$W/gl_defined.txt" "$W/gl_want.txt" > "$W/gl_keep.txt"; LC_ALL=C comm -23 "$W/gl_defined.txt" "$W/gl_keep.txt" > "$W/gl_local.txt"
  "$OBJC" --localize-symbols="$W/gl_local.txt" "$W/gl_pre.o" "$W/obj/gl.o"; GLO=$W/obj/gl.o
  echo "   gl.o: $(du -m "$GLO" | cut -f1) MB, $(wc -l < "$W/gl_defined.txt") global symbols, $(wc -l < "$W/gl_keep.txt") kept global (used by the rest): $(head -c 600 "$W/gl_keep.txt" | tr '\n' ' ')..., $(wc -l < "$W/gl_local.txt") made local"
  AGC1=$GLSDK/lib/libSceAgc.so; AGC2=$GLSDK/lib/libSceAgcDriver.so
fi

ZO=
if [ -n "${ZINK:-}" ]; then
  ZSRC=${ZINKOBJ:-$HOME/zink_obj/zink.o}; [ -f "$ZSRC" ] || { echo "ZINK=1 needs $ZSRC (build-zink-probe.sh, then make_zink_obj.sh)"; exit 1; }
  "$OBJC" --strip-debug "$ZSRC" "$W/obj/zink.o"; ZO=$W/obj/zink.o
  echo "   zink.o: $(du -m "$ZO" | cut -f1) MB (debug info stripped)"
fi

echo "== [${SECONDS}s] CRT + AGC import stubs (as the template's link-title.sh)"
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$NATIVE/app_crt.cpp" -o "$W/obj/app_crt.o"
stub() { cc -std=c11 -O2 -fPIC -c "$VK/$2" -o "$W/obj/$1_stub.o"; "$LLD" --shared -soname "$1.prx" -o "$W/stubs/$1.so" "$W/obj/$1_stub.o"; }
stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c
if [ -n "${AIO:-}" ]; then   # AIO: union of both stacks' AGC imports (RADV's canary stubs + the GL SDK's), one stub per library
  for l in libSceAgc libSceAgcDriver; do
    { "$NM" -D --defined-only -j "$W/stubs/$l.so"; "$NM" -D --defined-only -j "$GLSDK/lib/$l.so"; } | grep -v -e ':$' -e '^$' | LC_ALL=C sort -u > "$W/stubs/$l.names"
    while read -r n; do echo "void $n(void){}"; done < "$W/stubs/$l.names" > "$W/stubs/$l.union.c"
    cc -std=c11 -O2 -fPIC -w -c "$W/stubs/$l.union.c" -o "$W/obj/$l.union.o"; "$LLD" --shared -soname "$l.prx" -o "$W/stubs/$l.u.so" "$W/obj/$l.union.o"
    echo "   $l: $(wc -l < "$W/stubs/$l.names") imports (RADV $("$NM" -D --defined-only "$W/stubs/$l.so" | wc -l) + GL $("$NM" -D --defined-only "$GLSDK/lib/$l.so" | wc -l))"
  done
  AGC1=$W/stubs/libSceAgc.u.so; AGC2=$W/stubs/libSceAgcDriver.u.so
fi

echo "== [${SECONDS}s] link with RADV (the template's recipe; --no-dynamic-linker keeps Mesa's weak dispatch references out of the dynamic table)"
source "$VK/tools/radv-link.sh"
radv_link_recipe "$VK" "$SDK" "$ARCHIVE" || exit 2
# The recipe binds libc names to the platform layer with --defsym=NAME=ps5_NAME, including getaddrinfo / freeaddrinfo. With --wrap=NAME on top, lld sends the wrapped references to the defsym target, so the JDK's lookups
# went to the platform's ps5_getaddrinfo (empty answer -> bare UnknownHostException) and ps5lwjgl.c's __wrap_getaddrinfo was never called (reproduced with ld.lld 18). Drop the defsym of every name the shim wraps,
# and its entry in the recipe's version-script map (lld rejects a map entry for an undefined symbol).
SHIMWRAP=" $(printf '%s ' ${WRAPS//--wrap=/}) "; _k=(); _skip=0
for x in "${radv_link_flags[@]}"; do
  if [ "$_skip" = 1 ]; then
    _skip=0
    { echo '{'; echo '    local:'; sed -n 's/^ *\([A-Za-z_0-9]*\);$/\1/p' "$x" | while read -r n; do case "$SHIMWRAP" in *" $n "*) ;; *) printf '        %s;\n' "$n";; esac; done; echo '};'; } > "$W/radv-platform-local.map"
    _k+=("$W/radv-platform-local.map"); continue
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
# The converter refuses any symbol the executable PUBLISHES. A symbol the executable defines that a system stub also exports (dlopen, dlsym, opendir ...) lands in the dynamic table; version-script it local
# (the executable still binds to its own copy). Link, list what got published, relink with those made local.
link_elf() {
  { printf '{\n    local:\n        _Zn*;\n        _Zd*;\n'; for n in $EXTRA_LOCAL; do printf '        %s;\n' "$n"; done; printf '};\n'; } > "$W/vk-symbols.map"
  "$LLD" "${radv_linker_script[@]}" --eh-frame-hdr --no-dynamic-linker "${radv_link_flags[@]}" $WRAPS \
    --version-script "$W/vk-symbols.map" --exclude-libs=ALL -e _start -o "$W/llvm-pie.elf" \
    "$W/obj/app_crt.o" "$W/obj/jvm_host.o" "$W/obj/ps5lwjgl.o" "$W/obj/ps5vk_stubs.o" ${GLO:+"$GLO"} ${ZO:+"$ZO"} "$LWB"/obj/*.o $FTO $SPO "$W/obj/jdk25.o" \
    "$AGC1" "$AGC2" "${radv_link_inputs[@]}" --as-needed "$SDK"/target/lib/*.so
}
run_link() { link_elf 2>"$W/link.err" || { echo "link failed ($(wc -l < "$W/link.err") lines of lld output, all in $W/link.err); distinct errors:"; grep -E "error|undefined symbol|duplicate" "$W/link.err" | sort | uniq -c | sort -rn | head -100; exit 1; }; cat "$W/link.err"; }
run_link
published=$("$NM" -D --defined-only "$W/llvm-pie.elf" | awk '{ n = $NF; sub(/@.*/, "", n); print n }' | sort -u)
if [ -n "$published" ]; then
  echo "   published by the executable (made local, relinking): $(echo $published | tr '\n' ' ')"
  EXTRA_LOCAL="$EXTRA_LOCAL $published"; run_link
  published=$("$NM" -D --defined-only "$W/llvm-pie.elf" | awk '{ n = $NF; sub(/@.*/, "", n); print n }' | sort -u)
  [ -z "$published" ] || { echo "still published after making them local: $(echo $published | tr '\n' ' ')"; exit 1; }
fi
null_imports=$(comm -23 \
    <("$NM" -D --undefined-only "$W/llvm-pie.elf" | awk '$1 == "U" { sub(/@.*/, "", $2); print $2 }' | sort -u) \
    <(for l in "$SDK"/target/lib/*.so "$AGC1" "$AGC2"; do
        case ${l##*/} in libkernel_sys.so | libScePosixForWebKit.so) continue;; esac
        "$NM" -D --defined-only "$l" 2>/dev/null | awk '{ print $NF }'
      done | sort -u))
[ -z "$null_imports" ] || { echo "imports no loaded module exports (null at run time): ${null_imports//$'\n'/ }"; exit 1; }
"$TOOL" link --in "$W/llvm-pie.elf" --out "$W/eboot.elf" --stub-dir "$SDK/target/lib" --stub "$AGC1" --stub "$AGC2" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf

echo "== [${SECONDS}s] title folder $TITLE"
APP=$W/dist/$TITLE
mkdir -p "$APP/sce_sys" "$APP/sce_module" "$APP/assets/hello" "$APP/assets/lib" "$APP/assets/lib34" "$APP/assets/mc" "$APP/mc"
"$TOOL" self --sign --in "$W/eboot.elf" --out "$APP/eboot.bin" --magic 0x1D3D154F
"$TOOL" self --inspect --file "$APP/eboot.bin" > /dev/null
python3 - "$TPL/ps5/sce_sys/param.json" "$APP/sce_sys/param.json" "$TITLE" <<'PY'
import json, os, re, sys
d = json.load(open(sys.argv[1])); t = sys.argv[3]
def walk(o):
    if isinstance(o, dict):
        for k, v in o.items(): o[k] = re.sub(r"PPSA\d{5}", t, v) if isinstance(v, str) else walk(v) or v
    elif isinstance(o, list):
        for i, v in enumerate(o): o[i] = re.sub(r"PPSA\d{5}", t, v) if isinstance(v, str) else walk(v) or v
walk(d); d["localizedParameters"]["en-US"]["titleName"] = "Minecraft AIO (debug)" if os.environ.get("AIO") else "Minecraft Vulkan"; json.dump(d, open(sys.argv[2], "w"), indent=2)
PY
if [ -n "${AIO:-}" ]; then   # the OpenGL SDK needs the large GPU page tables and the display declaration of its own title
  GLSRC=$HOME/ps5gl; [ -f "$GLSRC/native-app/param.json" ] || { echo "AIO needs $GLSRC (make_gl_cube.sh)"; exit 1; }
  python3 - "$APP/sce_sys/param.json" "$GLSRC/native-app/param.json" <<'PYG'
import json, sys
d = json.load(open(sys.argv[1])); s = json.load(open(sys.argv[2]))
for k in ("amm", "kernel"): d[k] = s[k]
json.dump(d, open(sys.argv[1], "w"), indent=2); print("   param.json: GPU page-table settings from the SDK title")
PYG
  # display declaration of the GL title (60 Hz): it capped the Vulkan side at 60 fps too. Default: none (the Vulkan title's own param.json, 120 fps); DISPHZ=60|120 adds it.
  [ -z "${DISPHZ:-}" ] || python3 "$GLSRC/tools/native-display-metadata.py" "$APP/sce_sys/param.json" --fps "$DISPHZ"
fi
for a in icon0.png pic0.dds pic1.dds snd0.at9; do if [ -f "$TPL/ps5/sce_sys/$a" ]; then cp "$TPL/ps5/sce_sys/$a" "$APP/sce_sys/$a"; fi; done
(cd "$VK/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)
cp "$VK/runtime/libc.prx" "$APP/sce_module/libc.prx"
# LWJGL 3.4.3 Java jars: lwjgl + stb on the class path, the full set under lib34 (McBoot loads them child-first for 26.x), from the OpenGL title's cache or Maven
LJ=${LWJGL_JARS:-$HOME/lwjgl_jars_$LV}; mkdir -p "$LJ"
for j in lwjgl lwjgl-opengl lwjgl-stb lwjgl-openal lwjgl-sdl lwjgl-freetype lwjgl-jemalloc lwjgl-vulkan lwjgl-vma lwjgl-shaderc lwjgl-spvc; do
  [ -s "$LJ/$j.jar" ] || curl -fsSL -o "$LJ/$j.jar" "https://repo1.maven.org/maven2/org/lwjgl/$j/$LV/$j-$LV.jar" || { rm -f "$LJ/$j.jar"; echo "cannot download $j $LV into $LJ"; exit 1; }
  cp "$LJ/$j.jar" "$APP/assets/lib34/$j-$LV.jar"
done
cp "$LJ/lwjgl.jar" "$APP/assets/lib/lwjgl.jar"; cp "$LJ/lwjgl-stb.jar" "$APP/assets/lib/lwjgl-stb.jar"
DLJ=$(ls -t $HOME/desktop_lite_cache/*.jar $HOME/ps5*_title/dist/*/assets/lib/desktop-lite.jar 2>/dev/null | head -1 || true); [ -s "$DLJ" ] || { echo "no desktop-lite.jar (run the OpenGL title build once: ~/desktop_lite_cache)"; exit 1; }
cp "$DLJ" "$APP/assets/lib/desktop-lite.jar"
BP=$HOME/base_patch25/out; [ -d "$BP" ] || BP=$HOME/base_patch/out; [ -d "$BP" ] || { echo "missing ~/base_patch25/out (java.base patch classes of the OpenGL launcher build)"; exit 1; }
mkdir -p "$APP/assets/lib/base-patch25" && cp -r "$BP"/* "$APP/assets/lib/base-patch25/"
cp "$HERE/log4j2-ps5.xml" "$APP/assets/mc/log4j2.xml"
JAVAC=/usr/lib/jvm/java-17-openjdk-amd64/bin/javac
CPJ="$LJ/lwjgl.jar:$LJ/lwjgl-stb.jar"
$JAVAC --release 8 -nowarn -cp "$CPJ" -d "$APP/assets/hello" "$HERE/Hello.java" "$HERE/McBoot.java" "$HERE/McDownload.java" "$HERE/Launcher.java" "$HERE/McAuth.java" "$HERE/McMods.java" "$HERE/Kit.java" 2>&1 | { grep -v -e warning -e '^Note' || true; }
printf 'mc            \n' > "$APP/assets/hello/mode.txt"   # padded: the deploy tool skips files whose size is unchanged, and the old Vulkan test build left a 3-byte "vk"
# first start: install and select the Minecraft version (McBoot's Launcher.runQueue reads <gameDir>/download.txt, the game folder is /app0/mc)
printf '# commands for the Vulkan title, one per line (see Launcher.java); done lines are removed\n%s select\n' "$MCVER" > "$APP/mc/download.txt"
rsync -a --delete "$JDKDIR/jdk/" "$APP/assets/jdk25/"
echo "   eboot.bin $(stat -c %s "$APP/eboot.bin") bytes, assets/jdk25 $(du -sm "$APP/assets/jdk25" | cut -f1) MB"
# the option that makes 26.x use its Vulkan backend: show what the client calls it (needs the client jar ~/mc_$MCVER from mc_vk_probe.sh)
if [ -f "$HOME/mc_$MCVER/client.jar" ] && command -v javap >/dev/null; then
  echo "== graphics API option strings in the client:"
  (cd "$HOME/mc_$MCVER" && for c in $(grep -rl -a "graphicsApi" x --include='*.class' 2>/dev/null | head -4 | sed 's#^x/##; s#\.class$##'); do echo "-- $c"; javap -c -p -constants -cp client.jar "$(echo $c | tr / .)" 2>/dev/null | grep -i -E "graphicsApi|vulkan|opengl|\"default\"" | sort -u | head -12; done)
fi

if [ -z "${NODEPLOY:-}" ]; then
  echo "== [${SECONDS}s] deploy to $PS5 (ftpsrv from PS5_Vulkan's .env: PS5_HOST must be $PS5)"
  grep -q "^PS5_HOST=$PS5\$" "$VK/.env" 2>/dev/null || { echo "note: $VK/.env has PS5_HOST=$(sed -n 's/^PS5_HOST=//p' "$VK/.env" 2>/dev/null), not $PS5"; }
  python3 "$VK/tools/deploy-title-folder.py" --always eboot.bin "$APP"
fi
echo "== BUILD FINISHED $(date '+%H:%M:%S') in ${SECONDS}s. Start the PowerShell listener (port 9999), then: python3 $VK/tools/ps5_console.py launch $TITLE"
