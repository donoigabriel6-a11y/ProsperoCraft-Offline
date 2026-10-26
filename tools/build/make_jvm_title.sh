#!/bin/bash
# make_jvm_title.sh <PC_IP> <PS5_IP> [options...]: turn ~/ps5probe into the "boot the JVM" title and deploy it over FTP.
#   options (any order, up to 5):
#     jit        C1 compiler on (always use for real runs)           gl        OpenGL SDK + LWJGL (needed for everything below)
#     mc         Minecraft 1.20.1 boot via McBoot (needs gl, ~/mc_1.20.1 from mc_deps.sh, ~/ps5jvm/libmanagement.a from build_mgmt.sh, ~/jdk17u sources)
#     sdk100     link the newest installed OpenGL SDK from ~/glsdk (1.0.2, else 1.0.1, else 1.0.0; GLSDK_VER=<v> forces one) instead of 0.3.0 (much faster, colours correct; the script prints install steps if missing)
#     sdklog     forward ALL SDK printf output to UDP (slow; default drops the per-draw chatter)
#     novw       drop the SDK video/agc logging wraps          selftest / mainthread   no-JVM native GL clear loop (bisect builds)
#     bare | bare46 | baremin | baretrace | barefix   SDK-only EGL loops in the host (bisect/trace builds; see README)
#     vk         (with gl launcher nativeui sdk100) the SAME title also carries RADV (native Vulkan) and Zink (OpenGL over Vulkan): launcher.cfg renderer=gl|vulkan|zink. Needs the Vulkan project (~/PS5_Vulkan, ~/PS5_VulkanTemplate), ~/zink_obj/zink.o (build_zink.sh); the boilerplate only compiles, vk_relink.sh links.
#   AUTOMATIC JDK: every Java built by build_jdk.sh (17, JDKV=21, JDKV=25, JDKV=8; same C2=1 dirs) is linked into the one title and the host picks it from launcher.cfg java=<major>; JDKS="17 21" restricts; JDKV=<v> sets the primary (Java-side build; 8 cannot be the primary, it is linked next to a newer Java).
#   env: FORCE=1 ignores the build caches (LWJGL natives, FreeType, LWJGL 2, desktop-lite.jar) and rebuilds them; NOCCACHE=1 compiles without ccache.  XMX=<MB> Java heap max (default 1536).  GC=parallel uses the parallel collector (default: serial).  GLTHREAD=1 enables the SDK's GL worker thread.  FINISH=1 logs how long the GPU is still busy when the CPU has finished a frame (a glFinish before the swap; adds a stall).  UNCAP=1 makes Minecraft's VSync OFF a real uncap (tearing flips, no vblank wait; live toggle, falls back if the console refuses).  GPUTIME=1 logs real GPU time (timestamp queries).  VERIFY=1 read-back checks of the merged draws' data.  RING=<n> slot ring depth (default 6).  NETPROBE=1 probes DNS/TCP/HTTP/HTTPS from the title at boot (launcher groundwork).  IDXCACHE=0 goes back to rebuilding the chunk index list every frame (default: persistent per-chunk index blocks).  HZ120=1 asks the console for 120 Hz output (needs a display that accepts 1080p120; otherwise it stays at 60).  ARENA_MB=<128|256|512> sizes the chunk arena (default 256), MEGA_FENCE=1 adds GL fences (slower with GLTHREAD).  MEGA=1 merges Minecraft's chunk draws into one draw per layer (implies VFIX; see ps5mega.inc).  MDBENCH=1 runs a one-off offscreen multi-draw benchmark at startup (logs 'mdbench:' lines).  GLPROF=1 turns the per-call GL/kernel timing wrappers back on (they cost ~8 ms/frame at distance 4: leave off for real play; the lean default prints only fps).  CPUS=<n> / JIT=<n> set the reported processor count / C1 compiler threads.  VFIX=1 rewrites MC's 16-bit integer / 3-byte vertex attributes into natively fetched formats (see ps5lwjgl.c).  NPROF=1 adds the native sampling profiler + pre-swap glFinish probe (use without GLTHREAD).   Without mc the title runs GLTest (PerfTest first, then the triangle); every build writes assets/hello/mode.txt.
# Needs: build_jdk.sh finished (~/ps5jvm), ~/ps5probe built once, make_gl_cube.sh run once (~/glsdk 0.3.0 + ~/ps5gl sources).
# Files expected next to this script: jvm_host.cpp gen_symtab.sh Hello.java
#   (gl: GLTest.java ColorTest.java PerfTest.java build_lwjgl.sh ps5lwjgl.c realnames.inc stubs.inc)   (mc: McBoot.java log4j2-ps5.xml)
set -e
PC=${1:?usage: make_jvm_title.sh <PC_IP> <PS5_IP>}
PS5=${2:?usage: make_jvm_title.sh <PC_IP> <PS5_IP>}
HERE=$(cd "$(dirname "$0")" && pwd)
T=$HOME/ps5probe
VKMODE=; GL=; SELFTEST=; NOVW=; BARE=; TRACE=; MC=; SDKLOG=; SDK100=; LAUNCHER=; NATIVEUI=
for opt in "${@:3}"; do case "$opt" in mc) MC=1;; launcher) MC=1; LAUNCHER=1; NATIVEUI=1;; nativeui) NATIVEUI=1;; sdklog) SDKLOG=1;; sdk100) SDK100=1;; gl) GL=1;; vk) VKMODE=1;; novw|bare|bare46|baremin) NOVW=1;; baretrace|barefix) NOVW=1; TRACE=1;; esac; done
# launcher builds: runtime-switchable features are compiled in by default; the launcher settings (launcher.cfg / instance.cfg) pick what is active. X=0 opts out of compiling one in.
if [ -n "$LAUNCHER" ]; then SDK100=1; : ${XMX:=2560}; : ${GC:=parallel}; : ${GLTHREAD:=1}; : ${UNCAP:=1}; : ${MEGA:=1}; [ "$XMX" = 0 ] && XMX=; [ "$GC" = 0 ] && GC=; [ "$GLTHREAD" = 0 ] && GLTHREAD=; [ "$UNCAP" = 0 ] && UNCAP=; [ "$MEGA" = 0 ] && MEGA=; fi
LV=${LWJGL_VER:-3.3.1}; [ -n "$LAUNCHER" ] && LV=${LWJGL_VER:-3.3.3}; export LWJGL_VER=$LV   # launcher builds carry LWJGL 3.3.3 (Sodium: "Required version: 3.3.3"); the baked-in 1.20.1 build keeps 3.3.1
LJ=$HOME/lwjgl_jars_$LV
# Automatic JDK: every Java that build_jdk.sh has built (~/ps5jvm = 17, ~/ps5jvm21, ~/ps5jvm25, ~/ps5jvm8 (Java 8); with C2=1 the *c2 dirs, Java 8 has none) is linked into the ONE title; the host picks the Java at start-up from launcher.cfg (java=<major the chosen version needs>).
# JDKS="17 21" restricts the list. JDKV is the PRIMARY Java: its sources/headers/javac build the Java side of the title (default: the lowest linked one).
jdkdir() { case $1 in 8) echo $HOME/ps5jvm8;; 21|25) echo $HOME/ps5jvm$1$([ "$C2" = 1 ] && echo c2);; *) echo $HOME/ps5jvm$([ "$C2" = 1 ] && echo c2);; esac; }
JDKLIST=""; for v in ${JDKS:-17 21 25 8}; do [ -f $(jdkdir $v)/whole-hotspot.a ] && JDKLIST="$JDKLIST $v"; done
JDKLIST=${JDKLIST# }; [ -n "$JDKLIST" ] || { echo "run build_jdk.sh first (no whole-hotspot.a in ~/ps5jvm, ~/ps5jvm21, ~/ps5jvm25 or ~/ps5jvm8)"; exit 1; }
JDKV=${JDKV:-${JDKLIST%% *}}; [ "$JDKV" = 8 ] && { echo "Java 8 cannot be the primary Java (the Java side is built with the sources of a newer one): build 17 or newer too, or set JDKV=17"; exit 1; }; case " $JDKLIST " in *" $JDKV "*) ;; *) echo "JDKV=$JDKV is not among the built JDKs ($JDKLIST)"; exit 1;; esac
JREL=17; case " $JDKLIST " in *" 8 "*) JREL=8;; esac   # --release of the Java side (Hello, McBoot, Launcher ...): 8 when a Java 8 is linked, because those classes must load on it
JDKMASK=0; for v in $JDKLIST; do case $v in 17) JDKMASK=$((JDKMASK|1));; 21) JDKMASK=$((JDKMASK|2));; 25) JDKMASK=$((JDKMASK|4));; 8) JDKMASK=$((JDKMASK|8));; esac; done
case $JDKV in 21|25) JDKSRC=$HOME/jdk${JDKV}u; FDL="";; *) JDKSRC=$HOME/jdk17u; FDL=" vendor/libfdlibm.a";; esac   # JDK 21+ has no native fdlibm
OUT=$(jdkdir $JDKV)
echo "== JDKs linked into the title: $JDKLIST (primary $JDKV)"
if [ -n "$VKMODE" ]; then   # three renderers in one title (see vk_relink.sh)
  [ -n "$GL" ] && [ -n "$LAUNCHER" ] && [ -n "$NATIVEUI" ] && [ -n "$SDK100" ] || { echo "vk needs the words: jit gl mc launcher sdk100 nativeui vk"; exit 1; }
  VKTPL=${TPL:-$HOME/PS5_VulkanTemplate}; VKDIR=${VK:-$HOME/PS5_Vulkan}
  VKINC=${VKINC:-$HOME/PS5_Mesa/include}; [ -f "$VKINC/vulkan/vulkan.h" ] || VKINC=$VKTPL/external
  [ -d "$VKDIR" ] && [ -d "$VKTPL" ] && [ -f "$VKINC/vulkan/vulkan.h" ] || { echo "vk: needs $VKDIR, $VKTPL and vulkan.h (see make_mcvk_title.sh for the one-time setup)"; exit 1; }
  VKSDK=$(bash $VKTPL/ps5/tools/setup-sdk.sh) || { echo "vk: $VKTPL/ps5/tools/setup-sdk.sh failed"; exit 1; }; export VKSDK
  for f in vk_relink.sh gen_gl_thunks.sh ps5vk.inc ps5vk_stubs.c; do [ -f "$HERE/$f" ] || { echo "vk: missing $HERE/$f"; exit 1; }; done
  echo "== vk: Vulkan project $VKDIR, template $VKTPL, vulkan.h from $VKINC, Zink object: $([ -f "${ZINKOBJ:-$HOME/zink_obj/zink.o}" ] && echo yes || echo 'NO (renderer=zink will fall back to Vulkan)')"
fi
S=$T/.deps/native/ps5-payload-sdk
[ -d $T/src ] || { echo "$T not found"; exit 1; }
for f in jvm_host.cpp gen_symtab.sh Hello.java; do [ -f $HERE/$f ] || { echo "missing $HERE/$f"; exit 1; }; done

echo "== [${SECONDS}s] allow whole-archive for vendor/whole-*.a (one change to the boilerplate's tools/build.sh)"
python3 - $T/tools/build.sh <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = '    link_inputs+=("$root/$archive")\n'
new = ('    if [[ $(basename "$archive") == whole-* ]]; then\n'
       '        link_inputs+=(--whole-archive "$root/$archive" --no-whole-archive)\n'
       '    else\n        link_inputs+=("$root/$archive")\n    fi\n')
if "whole-*" in s: print("already patched")
elif old in s: open(p, "w").write(s.replace(old, new, 1)); print("patched")
else: raise SystemExit("could not find the archive loop in tools/build.sh")
PY

echo "== [${SECONDS}s] USB keyboard: link the SDK's libSceKeyboard stub like the pad's (only when tools/build.sh names a -lScePad; the lines below show what it links)"
grep -n "lSce\|target/lib\|\.so" $T/tools/build.sh | head -12 | cut -c1-200
python3 - $T/tools/build.sh <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
if "SceKeyboard" in s: print("keyboard stub: already linked")
elif "-lScePad" in s: open(p, "w").write(s.replace("-lScePad", "-lScePad -lSceKeyboard", 1)); print("keyboard stub: -lSceKeyboard added")
else: print("keyboard stub: no -lScePad in tools/build.sh, not linked (send tools/build.sh)")
PY
[ -f $S/target/lib/libSceKeyboard.so ] && echo "keyboard stub: $S/target/lib/libSceKeyboard.so present" || echo "keyboard stub: $S/target/lib/libSceKeyboard.so MISSING"
echo "== [${SECONDS}s] make every symbol local (the title converter refuses any exported symbol; replacements for libc functions would be exported otherwise)"
python3 - $T/tooling/native/app-symbols.map <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
if "        *;" in s: print("already patched")
elif "        _Zd*;" in s:
    s = s.replace("        _Zd*;", "        _Zd*;\n        scalbn;\n        *;", 1) if "scalbn" not in s else s.replace("        scalbn;", "        scalbn;\n        *;", 1)
    open(p, "w").write(s); print("patched")
else: raise SystemExit("could not find the local: list in app-symbols.map")
PY
sed -i '/^        scalbn;$/d' $T/tooling/native/app-symbols.map   # '*;' already localizes it; an explicit name fails when the library is not linked (selftest build)

echo "== [${SECONDS}s] sources"
mkdir -p ~/ps5probe_orig2 && mv $T/src/jvm_probe.cpp ~/ps5probe_orig2/ 2>/dev/null || true
cp $HERE/jvm_host.cpp $T/src/
if [ -n "$NATIVEUI" ]; then   # native launcher (PS5 UI kit): sources + fonts from kit_launcher/ next to this script
  [ -d $HERE/kit_launcher/src ] || { echo "nativeui: $HERE/kit_launcher/src is missing"; exit 1; }
  rm -rf $T/src/kit && mkdir -p $T/src/kit && cp -r $HERE/kit_launcher/src/. $T/src/kit/
  rm -rf $T/assets/fonts && mkdir -p $T/assets/fonts && cp $HERE/kit_launcher/assets/fonts/*.huifont $T/assets/fonts/
  echo "== [${SECONDS}s] nativeui: $(find $T/src/kit -name '*.cpp' | wc -l) kit sources, fonts: $(ls $T/assets/fonts | tr '\n' ' ')"
  # Launcher pictures: put backgrounds in kit_launcher/assets/backgrounds/ (any size, png / jpg / webp / bmp; they are upscaled and sharpened here)
  # and the picture for worlds without an icon at kit_launcher/assets/unknown_pack.png.
  mkdir -p $HERE/kit_launcher/assets/backgrounds
  rm -rf $T/assets/backgrounds && mkdir -p $T/assets/backgrounds
  python3 $HERE/prepare_backgrounds.py $HERE/kit_launcher/assets/backgrounds $T/assets/backgrounds || echo "backgrounds: the picture step failed, the launcher keeps its animated backdrop"
  rm -f $T/assets/unknown_pack.png
  if [ -f $HERE/kit_launcher/assets/unknown_pack.png ]; then cp $HERE/kit_launcher/assets/unknown_pack.png $T/assets/unknown_pack.png; echo "== unknown_pack.png staged"; else echo "== no kit_launcher/assets/unknown_pack.png: worlds without an icon keep the green letter tile"; fi
  echo "== [${SECONDS}s] backgrounds staged: $(ls $T/assets/backgrounds | wc -l)"
else
  rm -rf $T/src/kit $T/assets/fonts $T/assets/backgrounds $T/assets/unknown_pack.png
fi
mkdir -p $T/include
cp $JDKSRC/src/java.base/share/native/include/jni.h $JDKSRC/src/java.base/unix/native/include/jni_md.h $T/include/
printf '#define LOG_HOST "%s"\n' "$PC" > $T/include/ps5_conf.h
echo "#define JDK_MASK $JDKMASK" >> $T/include/ps5_conf.h
TID=${TITLE_ID:-PPSA27272}   # one title for every Java
echo "#define TITLE_ID \"$TID\"" >> $T/include/ps5_conf.h
if [ -f $T/sce_sys/param.json ]; then
  python3 - $T/sce_sys/param.json "$TID" <<'PYT'
import json, re, sys
fn, tid = sys.argv[1:3]
d = json.load(open(fn))
def walk(o):
    if isinstance(o, dict):
        for k, v in list(o.items()):
            if isinstance(v, str): o[k] = re.sub(r"PPSA\d{5}", tid, v)
            else: walk(v)
    elif isinstance(o, list):
        for i, v in enumerate(o):
            if isinstance(v, str): o[i] = re.sub(r"PPSA\d{5}", tid, v)
            else: walk(v)
walk(d)
open(fn, "w").write(json.dumps(d, indent=2) + "\n")
print("param.json: title id", tid)
PYT
  echo "== title id $TID: other places in $T that still name a title id (if the deploy folder is not $TID, one of these is the source):"
  grep -rIn "PPSA[0-9]\{5\}" $T --exclude-dir=build --exclude-dir=dist --exclude-dir=.deps --exclude-dir=.git --exclude-dir=vendor --exclude-dir=assets --exclude=ps5_symtab.c 2>/dev/null | grep -v "$TID" | cut -c1-200 | head -8
fi
# Branding: the name on the home screen and the title's own art / music. Put the files in a folder "brand" next to this script (icon0.png, pic0.png, pic0.dds, pic1.dds, pic2.dds, snd0.at9);
# whichever exist are copied into sce_sys, the rest keep what the title has. APP_NAME=... changes the name (default ProsperoCraft).
APPN=${APP_NAME:-ProsperoCraft}
if [ -f $T/sce_sys/param.json ]; then
  python3 - $T/sce_sys/param.json "$APPN" <<'PYN'
import json, sys
fn, name = sys.argv[1:3]
d = json.load(open(fn)); n = [0]
def walk(o):
    if isinstance(o, dict):
        for k, v in list(o.items()):
            if k == "titleName" and isinstance(v, str): o[k] = name; n[0] += 1
            else: walk(v)
    elif isinstance(o, list):
        for v in o: walk(v)
walk(d)
open(fn, "w").write(json.dumps(d, indent=2) + "\n")
print("param.json: titleName set to", name, "in", n[0], "place(s)")
PYN
fi
mkdir -p $T/sce_sys
for f in icon0.png pic0.png pic1.png pic0.dds pic1.dds pic2.dds snd0.at9; do
  [ -f $HERE/brand/$f ] || { echo "== brand: no $f (title keeps its own)"; continue; }
  case $f in *.dds)
    if ! python3 $HERE/brand_check.py $HERE/brand/$f; then
      echo "   not 3840x2160 BC7_UNORM DX10 (the app builder's rule): converting it with brand_dds.py (crop to 16:9, BC7)"
      python3 -c "import etcpak, PIL" 2>/dev/null || pip install --break-system-packages -q etcpak pillow 2>&1 | tail -1
      if python3 $HERE/brand_dds.py $HERE/brand/$f $T/sce_sys/$f 2>/dev/null; then continue; fi
      if [ -f $HERE/brand/pic0.png ] && python3 $HERE/brand_dds.py $HERE/brand/pic0.png $T/sce_sys/$f; then echo "   ($f was unreadable: made from pic0.png)"; continue; fi
      echo "   conversion not possible: the vk build keeps $f out of the app builder and installs it as it is"
    fi;; esac
  cp $HERE/brand/$f $T/sce_sys/$f && echo "== brand: $f -> sce_sys ($(stat -c %s $HERE/brand/$f) bytes)"
done
[ "$C2" = 1 ] && echo "#define HAS_C2 1" >> $T/include/ps5_conf.h
[ -n "$NATIVEUI" ] && echo "#define NATIVE_UI 1" >> $T/include/ps5_conf.h
if [ -n "$VKMODE" ]; then { echo "#define AIO_RENDERER 1"; echo "#define VK_LAUNCHER 1"; echo "#define DEFAULT_RENDERER \"${RENDERER:-gl}\""; [ -f "${ZINKOBJ:-$HOME/zink_obj/zink.o}" ] && echo "#define PS5_ZINK 1"; true; } >> $T/include/ps5_conf.h; fi
[ -n "$XMX" ] && echo "#define XMX_MB $XMX" >> $T/include/ps5_conf.h   # optional: XMX=2048 bash make_jvm_title.sh ...
[ "$GLTHREAD" = 1 ] && echo "#define GLTHREAD 1" >> $T/include/ps5_conf.h   # optional: GLTHREAD=1 bash make_jvm_title.sh ... (SDK glthread: GL calls run on a worker thread)
[ -n "$CPUS" ] && echo "#define CPUS_N $CPUS" >> $T/include/ps5_conf.h   # optional: CPUS=6 (processor count the JVM reports: sizes Minecraft's worker pools; default 4)
[ -n "$JIT" ] && echo "#define JIT_THREADS $JIT" >> $T/include/ps5_conf.h   # optional: JIT=2 (C1 compiler threads; default 1)
[ "$NETPROBE" = 1 ] && echo "#define NETPROBE 1" >> $T/include/ps5_conf.h   # NETPROBE=1: probe DNS/TCP/HTTP/HTTPS from inside the JVM at boot (logs "net:" lines)
[ "$GC" = parallel ] && echo "#define GC_PARALLEL 1" >> $T/include/ps5_conf.h   # optional: GC=parallel bash make_jvm_title.sh ... (4 collector threads instead of serial; shorter full-GC pauses if it boots)
for opt in "${@:3}"; do case "$opt" in novw) ;; bare|bare46|baremin|baretrace|barefix) echo '#define GL_BARE 1' >> $T/include/ps5_conf.h; [ "$opt" = baretrace ] && echo '#define GL_BARE_TRACE 1' >> $T/include/ps5_conf.h; [ "$opt" = barefix ] && { echo '#define GL_BARE_TRACE 1' >> $T/include/ps5_conf.h; echo '#define GL_BARE_FIX 1' >> $T/include/ps5_conf.h; }; [ "$opt" = baremin ] && echo '#define GL_BARE_MIN 1' >> $T/include/ps5_conf.h; [ "$opt" = bare46 ] && echo '#define GL_BARE_46 1' >> $T/include/ps5_conf.h; echo '#define GL_SELFTEST 1' >> $T/include/ps5_conf.h; echo '#define GL_SELFTEST_ONLY 1' >> $T/include/ps5_conf.h; echo '#define GL_SELFTEST_MAIN 1' >> $T/include/ps5_conf.h; SELFTEST=1; BARE=1;; jit) echo '#define USE_JIT 1' >> $T/include/ps5_conf.h;; gl) echo '#define USE_GL 1' >> $T/include/ps5_conf.h;; mainthread) echo '#define GL_SELFTEST_MAIN 1' >> $T/include/ps5_conf.h;; selftest) echo '#define GL_SELFTEST 1' >> $T/include/ps5_conf.h; echo '#define GL_SELFTEST_ONLY 1' >> $T/include/ps5_conf.h; SELFTEST=1;; esac; done

echo "== [${SECONDS}s] OpenGL mode: ${GL:-off}"
GLSDK=$HOME/glsdk/ps5-opengl-sdk-0.3.0/sdk; GLSRC=$HOME/ps5gl
if [ -n "$SDK100" ]; then
  # newest installed package wins: 1.0.1 fixes a use-after-free when Mesa rebinds vertex buffers (it held the last reference), which is exactly the path Minecraft's chunk draws take
  GLSDK=""
  for V in ${GLSDK_VER:-1.0.2 1.0.1 1.0.0}; do
    [ -f $HOME/glsdk/ps5-opengl-sdk-$V/sdk/lib/libps5_opengl_core33.a ] && { GLSDK=$HOME/glsdk/ps5-opengl-sdk-$V/sdk; GLVER=$V; break; }
  done
  if [ -z "$GLSDK" ]; then
    echo "OpenGL SDK 1.0.1 not found under ~/glsdk. Install it once:"
    echo "  mkdir -p ~/glsdk && cd ~/glsdk"
    echo "  curl -L -O https://github.com/blackbearreloaded/ps5-opengl/releases/download/v1.0.2/ps5-opengl-sdk-1.0.2.tar.gz"
    echo "  curl -L -O https://github.com/blackbearreloaded/ps5-opengl/releases/download/v1.0.2/ps5-opengl-sdk-1.0.2.tar.gz.sha256"
    echo "  sha256sum -c ps5-opengl-sdk-1.0.2.tar.gz.sha256 && tar xzf ps5-opengl-sdk-1.0.2.tar.gz"
    exit 1
  fi
  [ -n "$GLSDK_DIR" ] && { [ -f $GLSDK_DIR/lib/libps5_opengl_core33.a ] || { echo "GLSDK_DIR=$GLSDK_DIR has no lib/libps5_opengl_core33.a"; exit 1; }; GLSDK=$GLSDK_DIR; GLVER="custom build"; }   # a self-built SDK (e.g. ASYNCPREP: native preparation on worker threads), same layout as the release package
  echo "== [${SECONDS}s] OpenGL SDK $GLVER ($GLSDK)"
fi
export SDKGL=$GLSDK   # build_lwjgl.sh compiles the LWJGL natives against this package's headers
python3 - $T "$GL" "$NOVW" "$TRACE" "$BARE" "$SDK100" <<'PY'
import sys
t, gl, novw, trace, bare, sdk100 = sys.argv[1], sys.argv[2] == "1", sys.argv[3] == "1", sys.argv[4] == "1", sys.argv[5] == "1", sys.argv[6] == "1"
w = t + "/tooling/native/sce_module_writer.cpp"; b = t + "/tools/build.sh"
WRAPS = " --wrap=malloc --wrap=calloc --wrap=realloc --wrap=free --wrap=posix_memalign --wrap=malloc_usable_size"
# libc heap limit: log ac55cf32 measured only ~12 MB free in the libc heap at start-up and 0 after ~90 s (the "pthread pool cliff": every libc/system-library allocation then fails). The SDK recipe bounds it at 256 MiB;
# HEAPLIM (hex, default 0x40000000 = 1 GiB) is tried instead. The 25th-thread "libcheap" line in the log shows whether the limit is what bounds it (MB available jumps) or not (stays ~12).
HEAPLIM = __import__("os").environ.get("HEAPLIM", "0x40000000")
s = open(w).read(); old = "write_u64(result.data, result.heap_size, std::numeric_limits<std::uint64_t>::max());"; new = "write_u64(result.data, result.heap_size, %sULL);" % HEAPLIM
import re as _re
cur = _re.search(r"write_u64\(result\.data, result\.heap_size, [^;]*\);", s).group(0)
if gl and cur != new: open(w, "w").write(s.replace(cur, new, 1)); print("heap patched:", HEAPLIM)
if not gl and cur != old: open(w, "w").write(s.replace(cur, old, 1)); print("heap restored")
s = open(b).read()
VW = "".join(" --wrap=" + n for n in ("sceVideoOutOpen sceVideoOutSetFlipRate sceVideoOutRegisterBuffers2 sceVideoOutSubmitFlip sceVideoOutIsFlipPending "
                                       "sceVideoOutWaitVblank sceVideoOutGetFlipStatus sceAgcDriverSubmitDcb sceAgcDcbSetFlip sceAgcDriverWaitUntilSafeForRendering").split())
s = open(b).read()
if gl and "--wrap=malloc" not in s:
    assert s.count("--eh-frame-hdr \\") == 1; s = s.replace("--eh-frame-hdr \\", "--eh-frame-hdr" + WRAPS + " \\", 1); open(b, "w").write(s); print("wraps added")
TW = "".join(" --wrap=" + n for n in ("sceVideoOutOpen sceVideoOutSetFlipRate sceVideoOutRegisterBuffers2 sceVideoOutSubmitFlip sceVideoOutIsFlipPending "
                                       "sceVideoOutWaitVblank sceVideoOutGetFlipStatus sceAgcDriverSubmitDcb sceAgcDcbSetFlip "
                                       "ps5_agc_gate2_prepare_present ps5_agc_gate2_present ps5_agc_gate2_batch_begin ps5_agc_gate2_batch_end ps5_agc_gate2_batch_present").split())
s = open(b).read()
FW = ("" if sdk100 else " --wrap=ps5_agc_gate2_prepare_present") + " --wrap=clock_gettime --wrap=sceKernelUsleep --wrap=sceKernelAllocateDirectMemory --wrap=sceKernelMapDirectMemory --wrap=munmap --wrap=sceKernelReleaseDirectMemory --wrap=getaddrinfo --wrap=freeaddrinfo --wrap=fcntl --wrap=ioctl --wrap=read --wrap=write --wrap=readv --wrap=writev --wrap=recv --wrap=recvfrom --wrap=send --wrap=sendto --wrap=accept --wrap=close --wrap=bind --wrap=setsockopt --wrap=listen --wrap=connect --wrap=pwrite --wrap=ftruncate --wrap=fsync --wrap=fdatasync --wrap=dup"   # presentation fix + GPU-wait polling + name resolution (the JDK getaddrinfo is a stub) + emulated non-blocking sockets (fcntl is refused on sockets), wrappers live in ps5lwjgl.c
import re
s = re.sub(r" --wrap=(?:sceVideoOut|sceAgc|ps5_agc_gate2_|sceKernelUsleep|sceKernelAllocateDirectMemory|sceKernelMapDirectMemory|munmap|sceKernelReleaseDirectMemory|getaddrinfo|freeaddrinfo|fcntl|ioctl|read|write|recv|send|accept|close|bind|pwrite|ftruncate|fsync|fdatasync|dup)\w*", "", s)   # strip EVERY video/agc/runtime wrap an earlier build left behind (any set, any order, duplicates)
want = TW if trace else (("" if novw else VW) + ("" if bare else FW))
if gl and want: s = s.replace(WRAPS, WRAPS + want, 1)
open(b, "w").write(s); print("video/agc wraps:", "trace set (host wrappers)" if trace and gl else "none (novw)" if novw else "lwjgl set" if gl else "none")
if not gl and "--wrap=malloc" in s: open(b, "w").write(s.replace(VW, "").replace(WRAPS, "")); print("wraps removed")
PY
rm -f $T/src/runtime_shims.c $T/src/app_heap.c
if [ -n "$GL" ]; then
  [ -f $GLSDK/lib/libPS5OpenGLCore33.a ] && [ -f $GLSRC/native-app/app_heap.c ] || { echo "run make_gl_cube.sh once first (it fetches the OpenGL SDK and sources)"; exit 1; }
  for f in GLTest.java ColorTest.java PerfTest.java build_awtlite.sh awtlite.src ps5awt.c build_lwjgl.sh build_lwjgl2.sh ps5lwjgl2.c l2src/PS5Display.java build_gl4es.sh ps5gl4es.c ps5lwjgl.c ps5usbin.inc ps5sdl.inc ps5sdl_keys.inc build_spirv.sh ps5locale.c realnames.inc stubs.inc; do [ -f $HERE/$f ] || { echo "missing $HERE/$f"; exit 1; }; done
  grep -q "ps5sdl.inc" $HERE/ps5lwjgl.c || { echo "$HERE/ps5lwjgl.c is an old copy (it does not include ps5sdl.inc: copy the new ps5lwjgl.c)"; exit 1; }
  grep -q "PS5_SPIRV" $HERE/ps5lwjgl.c || { echo "$HERE/ps5lwjgl.c is an old copy (no shaderc/SPIRV-Cross support: copy the new ps5lwjgl.c)"; exit 1; }
  grep -q "ps5_pal_lookup" $HERE/ps5lwjgl.c || { echo "$HERE/ps5lwjgl.c is an old copy (no LWJGL 2 hooks: copy the new ps5lwjgl.c)"; exit 1; }
  grep -q "JNI_OnLoad_jimage" $HERE/ps5lwjgl.c || { echo "$HERE/ps5lwjgl.c is an old copy (a browser may have saved the new one as 'ps5lwjgl (1).c')"; exit 1; }
  NPT=""   # NPROF: end of .text from the previous link output (the sampler only treats words inside .text as return addresses)
  if [ "$NPROF" = 1 ] && [ -f $HOME/ps5probe/build/llvm-pie.elf ]; then
    read TA TS <<<"$(readelf -SW $HOME/ps5probe/build/llvm-pie.elf | awk '{for(i=1;i<=NF;i++) if($i==".text"){print $(i+2), $(i+4); exit}}')"
    [ -n "$TA" ] && NPT=$(printf '0x%xULL' $((0x$TA + 0x$TS + 0x40000)))
  fi
  [ -n "$VKMODE" ] && : ${HZ120:=1}; LWJGL_EXTRA=""; [ -n "$NPT" ] && LWJGL_EXTRA="$LWJGL_EXTRA -DNP_TEXT_END=$NPT"; [ -n "$NOVW" ] && LWJGL_EXTRA="$LWJGL_EXTRA -DNO_VIDEO_WRAPS"; [ "${OWNMALLOC:-1}" = 0 ] && LWJGL_EXTRA="$LWJGL_EXTRA -DPS5_NO_OWNMALLOC"; [ "${RTLDHEAP:-1}" = 0 ] && LWJGL_EXTRA="$LWJGL_EXTRA -DPS5_NO_RTLDHEAP"; [ -n "$SDKLOG" ] && LWJGL_EXTRA="$LWJGL_EXTRA -DPS5_SDK_LOG_ALL"; [ -n "$SDK100" ] && LWJGL_EXTRA="$LWJGL_EXTRA -DNO_PREPARE_FIX"; [ "$NPROF" = 1 ] && LWJGL_EXTRA="$LWJGL_EXTRA -DPS5_NPROF"; [ "$HZ120" = 1 ] && LWJGL_EXTRA="$LWJGL_EXTRA -DPS5_HZ120"; [ "$UNCAP" = 1 ] && LWJGL_EXTRA="$LWJGL_EXTRA -DPS5_UNCAP"; [ "$GPUTIME" = 1 ] && LWJGL_EXTRA="$LWJGL_EXTRA -DPS5_GPUTIME"; [ "$VERIFY" = 1 ] && LWJGL_EXTRA="$LWJGL_EXTRA -DMEGA_VERIFY"; [ -n "$RING" ] && LWJGL_EXTRA="$LWJGL_EXTRA -DMEGA_RING=$RING"; [ "$IDXCACHE" = 0 ] && LWJGL_EXTRA="$LWJGL_EXTRA -DMEGA_IDXCACHE=0"; [ "$MEGA" = 1 ] && VFIX=1 && LWJGL_EXTRA="$LWJGL_EXTRA -DPS5_MEGA"; [ -n "$ARENA_MB" ] && LWJGL_EXTRA="$LWJGL_EXTRA -DMEGA_ARENA_MB=$ARENA_MB"; [ "$MEGA_FENCE" = 1 ] && LWJGL_EXTRA="$LWJGL_EXTRA -DMEGA_FENCE"; [ "$VFIX" = 1 ] && LWJGL_EXTRA="$LWJGL_EXTRA -DPS5_VFIX"; [ "$GLPROF" = 1 ] && LWJGL_EXTRA="$LWJGL_EXTRA -DPS5_GLPROF"; [ "$MDBENCH" = 1 ] && LWJGL_EXTRA="$LWJGL_EXTRA -DPS5_MDBENCH"; [ "$FINISH" = 1 ] && LWJGL_EXTRA="$LWJGL_EXTRA -DPS5_FINISH"; export LWJGL_EXTRA
  if [ -n "$VKMODE" ]; then LWJGL_EXTRA="$(echo " $LWJGL_EXTRA " | sed -e 's/ -DPS5_UNCAP / /' -e 's/ -DPS5_GPUTIME / /') -DPS5_VK -DAIO -DNO_VIDEO_WRAPS -DNO_PREPARE_FIX -I$VKINC"; [ -f "${ZINKOBJ:-$HOME/zink_obj/zink.o}" ] && LWJGL_EXTRA="$LWJGL_EXTRA -DPS5_ZINK"; export LWJGL_EXTRA LWJGL_VMA=1; fi   # vk: the shim is built like make_mcvk_title.sh AIO=1 does (Vulkan hooks, video wraps of ps5vk.inc instead of the SDK ones)
  [ -n "$LAUNCHER" ] && export LWJGL_VERS="$LV 3.4.3"   # launcher builds link the natives of LWJGL 3.3.x (1.16-1.21.x) AND 3.4.3 (26.x) + FreeType; McBoot picks the Java jars per Minecraft version
  OUT=$OUT JNIINC=$JDKSRC/src/java.base bash $HERE/build_lwjgl.sh   # the LWJGL natives archive goes next to the JDK libraries it is linked with
  [ -f $T/tooling/native/ps5-pie-base.ld ] || cp $T/tooling/native/ps5-pie.ld $T/tooling/native/ps5-pie-base.ld
  cp $GLSRC/native-app/ps5-pie.ld $T/tooling/native/ps5-pie.ld
  [ -n "$VKMODE" ] || cp $GLSRC/native-app/runtime_shims.c $GLSRC/native-app/app_heap.c $T/src/   # vk: RADV/Mesa use the platform's malloc, the SDK's heap wraps are left out (as in make_mcvk_title.sh AIO=1)
  cp -n $GLSDK/lib/libSceAgc.so $GLSDK/lib/libSceAgcDriver.so $S/target/lib/   # the title converter needs these import stubs next to the others
  for h in EGL GL KHR; do cp -a $GLSDK/include/$h $T/include/; done
  # the SDK's own title sets large kernel/GPU page tables and a 512 GiB multimap range; without them the GPU runs out of mappings
  python3 - $T/sce_sys/param.json $GLSRC/native-app/param.json <<'PY'
import json, sys
mine, sdk = sys.argv[1], sys.argv[2]
d = json.load(open(mine)); s = json.load(open(sdk))
for k in ("amm", "kernel"): d[k] = s[k]
open(mine, "w").write(json.dumps(d, indent=2) + "\n"); print("param.json: page-table settings copied from the SDK title")
PY
  python3 $GLSRC/tools/native-display-metadata.py $T/sce_sys/param.json --fps $([ "$HZ120" = 1 ] && echo 120 || echo 60)   # HZ120=1: the title declares 120 Hz output (attribute3 bits 0x80040)
fi

echo "== [${SECONDS}s] symbol table (after the LWJGL archive, so its JNI names are included)"
echo "/* each JDK carries its own symbol table inside its whole-jdk<version>.a (make_jvm_title.sh) */" > $T/src/ps5_symtab.c

echo "== [${SECONDS}s] libraries + assets"
mkdir -p $T/vendor && rm -f $T/vendor/*.a $T/vendor/*.o
rm -rf $T/assets/jdk $T/assets/jdk8 $T/assets/jdk17 $T/assets/jdk21 $T/assets/jdk25 $T/assets/hello
for v in $JDKLIST; do cp -r $(jdkdir $v)/jdk $T/assets/jdk$v; done
JDKARCH=""
if [ -z "$SELFTEST" ] && [ -z "$BARE" ]; then
  LLD=$S/bin/ld.lld; [ -x $LLD ] || LLD=/usr/lib/llvm-18/bin/ld.lld
  OBJC=$S/bin/llvm-objcopy; [ -x $OBJC ] || OBJC=/usr/lib/llvm-18/bin/llvm-objcopy
  # symbols the non-JDK code (LWJGL shim, gl4es, SPIRV-Cross ...) takes from the JDK objects (ps5_trace, nl_langinfo ...): the FIRST linked JDK keeps those global, the others make them local
  { $S/bin/llvm-nm -u -j $OUT/liblwjgl_ps5.a 2>/dev/null; printf '%s\n' ps5_trace ps5_trace_bytes nl_langinfo; } | LC_ALL=C sort -u > $T/.jdk_shared_want.txt
  FIRSTJDK=1
  for v in $JDKLIST; do
    O=$(jdkdir $v); W=$T/.jdkobj$v; rm -rf $W; mkdir -p $W
    bash $HERE/gen_symtab.sh $S $O $W/symtab.c $OUT || exit 1   # this JDK's native libraries + the shared LWJGL archive of the primary JDK
    $S/bin/prospero-clang -c -O1 -w -o $W/symtab.o $W/symtab.c || exit 1
    LIBS=""; for l in java zip net nio jimage verify management management_ext sunec fdlibm; do [ -f $O/lib$l.a ] && LIBS="$LIBS $O/lib$l.a"; done
    # one relocatable object per JDK: HotSpot + its libraries + symbol table. Every global is then made local except the three entry points the host uses (renamed with a _<version> suffix),
    # so three JDKs can live in one executable without a single symbol clash.
    # --wrap must be applied here too: the JDK defines stand-ins (getaddrinfo, freeaddrinfo ...) for libc functions the title wraps; without --wrap, ld -r would bind the JDK's own calls to those stand-ins and the wrappers in ps5lwjgl.c (DNS, sockets) would never see them
    WRAPS=""; for w in getaddrinfo freeaddrinfo fcntl read write readv writev recv recvfrom send sendto accept close bind pwrite ftruncate fsync fdatasync dup clock_gettime sceKernelUsleep sceKernelAllocateDirectMemory sceKernelMapDirectMemory munmap sceKernelReleaseDirectMemory malloc calloc realloc free posix_memalign malloc_usable_size; do WRAPS="$WRAPS --wrap=$w"; done
    if [ -n "$VKMODE" ]; then WRAPS=$($S/bin/llvm-nm --defined-only $OUT/liblwjgl_ps5.a 2>/dev/null | awk '$3 ~ /^__wrap_[A-Za-z0-9_]+$/ { sub(/^__wrap_/, "", $3); print "--wrap=" $3 }' | sort -u | tr '\n' ' '); fi   # vk: the wraps of the Vulkan-aware shim (ps5vk.inc), not the boilerplate list (malloc heap wraps etc.)
    $LLD -r -m elf_x86_64 $WRAPS -o $W/pre.o $W/symtab.o --whole-archive $O/whole-hotspot.a --no-whole-archive --start-group $LIBS --end-group || exit 1
    $S/bin/llvm-nm -g --defined-only -j $W/pre.o | LC_ALL=C sort -u > $W/defined.txt
    LC_ALL=C comm -12 $W/defined.txt $T/.jdk_shared_want.txt > $W/shared.txt
    if [ -n "$FIRSTJDK" ]; then LC_ALL=C comm -23 $W/defined.txt $W/shared.txt > $W/local0.txt; echo "jdk $v exports for the shared code: $(tr '\n' ' ' < $W/shared.txt)"; else cp $W/defined.txt $W/local0.txt; fi
    FIRSTJDK=
    grep -v -x -e JNI_CreateJavaVM -e ps5_set_java_home -e ps5_trace_hook -e ps5_dll_handle -e ps5_dlsym $W/local0.txt > $W/local.txt
    $OBJC --redefine-sym JNI_CreateJavaVM=JNI_CreateJavaVM_$v --redefine-sym ps5_set_java_home=ps5_set_java_home_$v --redefine-sym ps5_trace_hook=ps5_trace_hook_$v --redefine-sym ps5_dll_handle=ps5_dll_handle_$v --redefine-sym ps5_dlsym=ps5_dlsym_$v --localize-symbols=$W/local.txt $W/pre.o $W/jdk$v.o || exit 1
    for e in JNI_CreateJavaVM_$v ps5_set_java_home_$v ps5_trace_hook_$v ps5_dll_handle_$v ps5_dlsym_$v; do $S/bin/llvm-nm -g --defined-only -j $W/jdk$v.o | grep -qx $e || { echo "jdk $v: entry point $e is missing from the linked JDK object"; exit 1; }; done
    rm -f $T/vendor/whole-jdk$v.a; $S/bin/llvm-ar rcs $T/vendor/whole-jdk$v.a $W/jdk$v.o
    echo "jdk $v: $(du -m $W/jdk$v.o | cut -f1) MB object, $(wc -l < $W/local.txt) symbols made local"
    JDKARCH="$JDKARCH vendor/whole-jdk$v.a"
  done
fi
rm -rf $T/.jdkobj*/pre.o
mkdir -p $T/assets/hello
rm -rf $T/assets/lib $T/assets/lib2
rm -rf $T/assets/mc
/usr/lib/jvm/java-17-openjdk-amd64/bin/javac --release $JREL -d $T/assets/hello $HERE/Hello.java
# Extra JDK modules the runtime images lack but Forge / NeoForge put on the module path: Forge's Nashorn (JS coremods) needs jdk.dynalink ("FindException: Module jdk.dynalink not found, required by org.openjdk.nashorn").
# Built from the JDK's own sources (pure Java, needs only java.logging) as a modular jar: McBoot adds every assets/extra/*.jar whose module the running Java does not have to the module path. Cached in ~/jdk_extra.
rm -rf $T/assets/extra; mkdir -p $T/assets/extra $HOME/jdk_extra
# a jar cached by an older build lacks the resource bundle (jdk/dynalink/support/messages.properties): Guards.<clinit> then dies with MissingResourceException and Forge's Nashorn never starts
if [ -s $HOME/jdk_extra/jdk.dynalink.jar ] && ! /usr/lib/jvm/java-17-openjdk-amd64/bin/jar tf $HOME/jdk_extra/jdk.dynalink.jar | grep -q 'dynalink/support/messages.properties'; then rm -f $HOME/jdk_extra/jdk.dynalink.jar; fi
if [ ! -s $HOME/jdk_extra/jdk.dynalink.jar ]; then
  DYN=$HOME/jdk17u/src/jdk.dynalink/share/classes
  if [ ! -d $DYN ]; then rm -rf $HOME/jdk_extra/src && git clone -q --depth 1 --filter=blob:none --sparse https://github.com/openjdk/jdk17u.git $HOME/jdk_extra/src && git -C $HOME/jdk_extra/src sparse-checkout set src/jdk.dynalink && DYN=$HOME/jdk_extra/src/src/jdk.dynalink/share/classes; fi
  if [ -d $DYN ]; then
    rm -rf $HOME/jdk_extra/dyn $HOME/jdk_extra/dyn_out && cp -r $DYN $HOME/jdk_extra/dyn && mkdir -p $HOME/jdk_extra/dyn_out
    python3 - $HOME/jdk_extra/dyn/jdk/dynalink/beans/AbstractJavaLinker.java <<'PYD'
import sys,re
p=sys.argv[1]; s=open(p).read()
s=s.replace("import jdk.internal.reflect.CallerSensitive;\n","")   # not exported to a module on the module path: look the annotation up by name
s=s.replace("m.isAnnotationPresent(CallerSensitive.class)","isCallerSensitive(m)")
s=s.replace("    private static SingleDynamicMethod createDynamicMethod(","    private static boolean isCallerSensitive(final Executable m) { for (final java.lang.annotation.Annotation a : m.getAnnotations()) if (a.annotationType().getName().equals(\"jdk.internal.reflect.CallerSensitive\")) return true; return false; }\n    private static SingleDynamicMethod createDynamicMethod(",1)
open(p,"w").write(s)
PYD
    /usr/lib/jvm/java-17-openjdk-amd64/bin/javac --release 17 -nowarn -d $HOME/jdk_extra/dyn_out $(find $HOME/jdk_extra/dyn -name '*.java') 2>&1 | grep -v '^Note' | head -5
    (cd $HOME/jdk_extra/dyn && find . -type f ! -name '*.java' -exec cp --parents {} $HOME/jdk_extra/dyn_out/ \;)
    /usr/lib/jvm/java-17-openjdk-amd64/bin/jar --create --file $HOME/jdk_extra/jdk.dynalink.jar -C $HOME/jdk_extra/dyn_out . || rm -f $HOME/jdk_extra/jdk.dynalink.jar
  fi
fi
if [ -s $HOME/jdk_extra/jdk.dynalink.jar ]; then cp $HOME/jdk_extra/jdk.dynalink.jar $T/assets/extra/; echo "== jdk.dynalink module staged (Forge's Nashorn needs it)"; else echo "== WARNING: jdk.dynalink.jar not built (no ~/jdk17u sources, no GitHub): Forge's JS coremods will fail"; fi
if [ -n "$GL" ]; then
  mkdir -p $T/assets/lib $LJ
  echo "== [${SECONDS}s] LWJGL $LV (natives built from source, Java jars from Maven)"
  for j in lwjgl lwjgl-glfw lwjgl-opengl lwjgl-stb; do
    [ -s $LJ/$j.jar ] || curl -fL -o $LJ/$j.jar https://repo1.maven.org/maven2/org/lwjgl/$j/$LV/$j-$LV.jar
    cp $LJ/$j.jar $T/assets/lib/$j.jar
  done
  # org.lwjgl.Version: getVersion() follows -Dps5.lwjgl.version (set by McBoot from the Minecraft version's JSON), so mods that insist on the LWJGL of THEIR Minecraft version (1.20.1: 3.3.1) accept the title's one
  patch_lwjgl_version() {   # $1 = lwjgl core jar, $2 = minor, $3 = revision of the generation in that jar
    VS=$HOME/lwjgl_version_shim; rm -rf $VS; mkdir -p $VS/src/org/lwjgl $VS/out
    sed "s/VERSION_MINOR = 3, VERSION_REVISION = 3/VERSION_MINOR = $2, VERSION_REVISION = $3/" $HERE/lwjgl_Version.java > $VS/src/org/lwjgl/Version.java
    /usr/lib/jvm/java-17-openjdk-amd64/bin/javac --release 8 -nowarn -d $VS/out $VS/src/org/lwjgl/Version.java 2>&1 | { grep -v warning || true; }   # grep exits 1 on empty input: under set -e that silently ended the build
    [ -s $VS/out/org/lwjgl/Version.class ] || { echo 'org.lwjgl.Version shim did not compile'; exit 1; }
    python3 - $1 $VS/out <<'PYV' || exit 1
import os, sys, zipfile, shutil
jar, root = sys.argv[1:3]
new = {"org/lwjgl/Version.class": root + "/org/lwjgl/Version.class", "org/lwjgl/Version$BuildType.class": root + "/org/lwjgl/Version$BuildType.class"}
tmp = jar + ".tmp"
with zipfile.ZipFile(jar) as zi, zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as zo:
    for it in zi.infolist():
        if it.filename in new: continue
        zo.writestr(it, zi.read(it.filename))
    for n, f in new.items(): zo.write(f, n)
shutil.move(tmp, jar); print(os.path.basename(jar) + ": org.lwjgl.Version replaced (ps5.lwjgl.version aware)")
PYV
  }
  patch_lwjgl_version $T/assets/lib/lwjgl.jar 3 3
  if [ "${LWJGL2:-1}" != 0 ]; then   # LWJGL 2 Java classes (Minecraft 1.12.2 and older), built by build_lwjgl2.sh (called from build_lwjgl.sh above); McBoot puts this jar first on the classpath of such versions
    L2J=${LWJGL2_BUILD:-$HOME/lwjgl2_build}/lwjgl2_ps5.jar; [ -s $L2J ] || { echo "missing $L2J (build_lwjgl2.sh did not run?)"; exit 1; }
    mkdir -p $T/assets/lib2 && cp $L2J $T/assets/lib2/lwjgl2_ps5.jar; echo "== [${SECONDS}s] LWJGL 2 jar staged under assets/lib2"
  fi
  if [ -n "$LAUNCHER" ]; then   # LWJGL 3.4.3 Java jars for Minecraft 26.x (SDL3 windowing, FreeType): loaded child-first by McBoot; the natives are linked into the title (build_lwjgl.sh LWJGL_VERS)
    L34=3.4.3; LJ34=$HOME/lwjgl_jars_$L34; mkdir -p $LJ34 $T/assets/lib34
    # EVERY org.lwjgl module that is on the app class path (lib/lwjgl*.jar + mc/lib/lwjgl-*.jar, all 3.3) needs a 3.4 copy here. A module that is missing is served to the game by the
    # app class path instead (McBoot$ChildFirst falls back to the parent), and the two generations in one game end in "loader constraint violation: loader 'app' wants to load
    # org.lwjgl.PointerBuffer" (26.1: lwjgl-glfw was missing here). The last four are only here so the Vulkan backend's classes resolve; Vulkan itself is not available.
    for j in lwjgl lwjgl-glfw lwjgl-opengl lwjgl-stb lwjgl-openal lwjgl-tinyfd lwjgl-sdl lwjgl-freetype lwjgl-jemalloc lwjgl-vulkan lwjgl-vma lwjgl-shaderc lwjgl-spvc; do
      [ -s $LJ34/$j.jar ] || curl -fL -o $LJ34/$j.jar https://repo1.maven.org/maven2/org/lwjgl/$j/$L34/$j-$L34.jar || { rm -f $LJ34/$j.jar; echo "cannot download $j $L34"; exit 1; }
      cp $LJ34/$j.jar $T/assets/lib34/$j-$L34.jar
    done
    patch_lwjgl_version $T/assets/lib34/lwjgl-$L34.jar 4 3
    echo "== [${SECONDS}s] LWJGL $L34 jars staged under assets/lib34: $(ls $T/assets/lib34 | wc -l)"
  fi
  CPJ=$T/assets/hello:$T/assets/lib/lwjgl.jar:$T/assets/lib/lwjgl-glfw.jar:$T/assets/lib/lwjgl-opengl.jar:$T/assets/lib/lwjgl-stb.jar
  /usr/lib/jvm/java-17-openjdk-amd64/bin/javac --release $JREL -cp $CPJ -d $T/assets/hello $HERE/GLTest.java $HERE/ColorTest.java $HERE/PerfTest.java
  if [ -n "$MC" ]; then
    mkdir -p $T/assets/mc/lib
    if [ -z "$LAUNCHER" ]; then
    MCD=$HOME/mc_1.20.1; [ -s $MCD/client.jar ] && ls $MCD/lib/*.jar >/dev/null 2>&1 || { echo "run: bash mc_deps.sh 1.20.1 first ($MCD missing)"; exit 1; }
    [ -f $HERE/McBoot.java ] || { echo "missing $HERE/McBoot.java"; exit 1; }
    mkdir -p $T/assets/mc/lib
    # Mojang signs client.jar and its obfuscated classes live in the DEFAULT package, next to our unsigned Hello/McBoot: the JVM refuses mixed signers
    # in one package ("signer information does not match"). Drop the signature files (standard practice for modded clients).
    python3 - $MCD/client.jar $T/assets/mc/client.jar <<'PYJ'
import sys, zipfile
src, dst = sys.argv[1:3]; n = 0
with zipfile.ZipFile(src) as zi, zipfile.ZipFile(dst, "w") as zo:
    for it in zi.infolist():
        nm = it.filename.upper()
        if nm.startswith("META-INF/") and nm.rsplit(".", 1)[-1] in ("SF", "DSA", "RSA", "EC"): n += 1; continue
        zo.writestr(it, zi.read(it.filename))
print("client.jar: removed", n, "signature files")
PYJ
    MC_CP=/app0/assets/mc/client.jar
    for j in $MCD/lib/*.jar; do b=$(basename $j)
      case "$b" in lwjgl-3.3.1.jar|lwjgl-glfw-*|lwjgl-opengl-*|lwjgl-stb-*) continue;; esac   # our own four LWJGL jars come first on the classpath
      cp $j $T/assets/mc/lib/; MC_CP=$MC_CP:/app0/assets/mc/lib/$b
    done
    else   # launcher build: nothing is baked into the title, versions are installed on the console (download.txt, McDownload)
      MC_CP=/app0/assets/hello; rm -rf $T/assets/mc/client.jar $T/assets/mc/lib/*.jar
      # the LWJGL jars beyond our own four (openal, jemalloc, tinyfd, freetype (1.21.x TTF fonts, e.g. Legacy4J's; the FreeType natives are the ones built for 3.4): their natives are linked into the title, the Java classes are not) come from Maven in the SAME version as the natives, because
      # the downloader skips org.lwjgl libraries (a version's own LWJGL generation would not match the static natives). Without them: NoClassDefFoundError org/lwjgl/openal/ALC10.
      LX=
      for j in lwjgl-openal lwjgl-jemalloc lwjgl-tinyfd lwjgl-freetype; do
        [ -s $LJ/$j.jar ] || curl -fL -o $LJ/$j.jar https://repo1.maven.org/maven2/org/lwjgl/$j/$LV/$j-$LV.jar || { echo "launcher: cannot download $j $LV"; exit 1; }
        b=$j-$LV.jar; cp $LJ/$j.jar $T/assets/mc/lib/$b; MC_CP=$MC_CP:/app0/assets/mc/lib/$b; LX="$LX $b"
      done
      echo "== [${SECONDS}s] launcher: no Minecraft version staged in the title; LWJGL extras:$LX"
    fi
    # java.desktop is not in the runtime image (AWT natives cannot be cross-built). Pure-Java pieces MC's libraries need are compiled from the
    # jdk17u sources and appended to the boot class path (java.* names are only allowed there). Add classes to DL_JAVA_BEANS when MC hits more.
    SRCD=$JDKSRC/src/java.desktop/share/classes; JAVAC17=/usr/lib/jvm/java-$JDKV-openjdk-amd64/bin/javac   # (variable name kept) compiler of the JDK being linked: its java.beans/PollingWatchService sources may use newer APIs
    [ -d $SRCD/java/beans ] || { echo "missing $SRCD (build_jdk.sh clones jdk17u)"; exit 1; }
    DL_JAVA_BEANS="PropertyChangeEvent PropertyChangeListener PropertyChangeListenerProxy PropertyChangeSupport IndexedPropertyChangeEvent ChangeListenerMap VetoableChangeListener VetoableChangeListenerProxy VetoableChangeSupport PropertyVetoException ConstructorProperties Transient JavaBean BeanProperty"
    DLS=""; for f in $DL_JAVA_BEANS; do DLS="$DLS $SRCD/java/beans/$f.java"; done
    # desktop-lite.jar is cached by a hash of everything it is built from (FORCE=1 rebuilds): the javac runs below take a while and the result only changes with these inputs
    DLK=$( { cat $HERE/build_awtlite.sh; cat $HERE/awtlite.src 2>/dev/null; find $HERE/awtlite -type f 2>/dev/null | sort | xargs -r cat 2>/dev/null; echo "$DL_JAVA_BEANS|$JDKV|$($JAVAC17 -version 2>&1)|$(git -C $JDKSRC rev-parse HEAD 2>/dev/null)"; } | md5sum | cut -c1-32); DLJ=$HOME/desktop_lite_cache/$DLK.jar
    mkdir -p $T/assets/lib
    if [ -z "$FORCE" ] && [ -s $DLJ ]; then cp $DLJ $T/assets/lib/desktop-lite.jar; echo "desktop-lite.jar: unchanged, reused from the cache"
    else
    rm -rf $HOME/desktop_lite && mkdir -p $HOME/desktop_lite
    $JAVAC17 --patch-module java.desktop=$SRCD -d $HOME/desktop_lite $DLS
    # AWT-lite + javax.script (build_awtlite.sh): the real ImageIO needs sun.awt.AppContext and the real BufferedImage the ICC / LittleCMS natives, neither exists here; Minecraft <= 1.12.2 reads every texture with
    # ImageIO.read + BufferedImage. This replaces BufferedImage / Image / ImageIO (PNG) / PlatformGraphicsInfo, draws in software (PS5Graphics2D) and adds the JDK's own javax.script (log4j's ScriptManager).
    # The natives these classes ask for (initIDs, JNI_OnLoad_awt) are no-ops in ps5awt.c, linked in by build_lwjgl.sh.
    bash $HERE/build_awtlite.sh $JDKSRC $JAVAC17 $HOME/desktop_lite || exit 1
    python3 - $HOME/desktop_lite $T/assets/lib/desktop-lite.jar <<'PYD'
import os, sys, zipfile
root, out = sys.argv[1:3]; n = 0
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for r, _, fs in os.walk(root):
        for f in fs: z.write(os.path.join(r, f), os.path.relpath(os.path.join(r, f), root)); n += 1
print("desktop-lite.jar:", n, "classes")
PYD
    mkdir -p $HOME/desktop_lite_cache && rm -f $HOME/desktop_lite_cache/*.jar && cp $T/assets/lib/desktop-lite.jar $DLJ
    fi
    # java.base patch: the JDK makefiles compile sun.nio.fs.PollingWatchService only for macOS/AIX, but our BSD overlay's BsdFileSystem.newWatchService() instantiates it
    # (the resource-pack screen watches the resourcepacks folder): NoClassDefFoundError. Compile it from the JDK sources and load it with --patch-module java.base.
    PWS=$JDKSRC/src/java.base/share/classes/sun/nio/fs/PollingWatchService.java
    [ -f $PWS ] || { echo "missing $PWS"; exit 1; }
    # java.lang.ProcessImpl: the JDK throws "java.lang.Error: FreeBSD is not a supported OS platform" when anything starts a process (Sodium's graphics-card probe through OSHI, "open folder" buttons...); the console cannot fork/exec anyway.
    # The stub fails like a missing program (IOException), which every caller already handles. One source for all JDKs (ProcessBuilder.start calls the same static method in 8, 17, 21 and 25).
    PIJ='package java.lang;
import java.io.IOException;
import java.util.Map;
final class ProcessImpl {
    private ProcessImpl() {}
    // JDK 21+/25: ProcessEnvironment (System.getenv, hit by every logger) and ProcessHandleImpl read these straight from ProcessImpl: NoSuchFieldError JNU_CHARSET (log bfd392bf, Minecraft 26.x on Java 25)
    static final java.nio.charset.Charset JNU_CHARSET = java.nio.charset.Charset.defaultCharset();
    static final boolean SUPPORTS_NORMAL_TERMINATION = false;
    static java.io.FileDescriptor newFileDescriptor(int fd) { throw new UnsupportedOperationException(); }
    static Process start(String[] cmdarray, Map<String, String> environment, String dir, ProcessBuilder.Redirect[] redirects, boolean redirectErrorStream) throws IOException {
        throw new IOException("error=2, creating processes is not supported on this console (" + (cmdarray != null && cmdarray.length > 0 ? cmdarray[0] : "") + ")");
    }
}'
    rm -rf $HOME/base_patch && mkdir -p $HOME/base_patch/src/sun/nio/fs $HOME/base_patch/src/java/lang $HOME/base_patch/out && cp $PWS $HOME/base_patch/src/sun/nio/fs/ && printf '%s\n' "$PIJ" > $HOME/base_patch/src/java/lang/ProcessImpl.java
    $JAVAC17 --patch-module java.base=$HOME/base_patch/src -d $HOME/base_patch/out $HOME/base_patch/src/sun/nio/fs/PollingWatchService.java $HOME/base_patch/src/java/lang/ProcessImpl.java || exit 1
    rm -rf $T/assets/lib/base-patch* && mkdir -p $T/assets/lib/base-patch$JDKV && cp -r $HOME/base_patch/out/* $T/assets/lib/base-patch$JDKV/
    echo "base-patch$JDKV: $(find $T/assets/lib/base-patch$JDKV -name '*.class' | wc -l) classes (PollingWatchService, ProcessImpl stub)"
    for v in $JDKLIST; do   # the other linked JDKs get their own build of it (their sources, their javac); without that JDK's javac/sources the primary's classes are used
      [ "$v" = "$JDKV" ] && continue
      [ "$v" = 8 ] && continue   # Java 8 has no modules: its rt.jar carries sun.nio.fs.PollingWatchService (ps5_jdk_patch8.py)
      JC=/usr/lib/jvm/java-$v-openjdk-amd64/bin/javac; PW=$HOME/jdk${v}u/src/java.base/share/classes/sun/nio/fs/PollingWatchService.java; D=$HOME/base_patch$v
      mkdir -p $T/assets/lib/base-patch$v
      if [ -x $JC ] && [ -f $PW ]; then rm -rf $D && mkdir -p $D/src/sun/nio/fs $D/src/java/lang $D/out && cp $PW $D/src/sun/nio/fs/ && printf '%s\n' "$PIJ" > $D/src/java/lang/ProcessImpl.java && $JC --patch-module java.base=$D/src -d $D/out $D/src/sun/nio/fs/PollingWatchService.java $D/src/java/lang/ProcessImpl.java && cp -r $D/out/* $T/assets/lib/base-patch$v/ && echo "base-patch$v: built from the Java $v sources"
      else cp -r $HOME/base_patch/out/* $T/assets/lib/base-patch$v/; echo "base-patch$v: no Java $v javac/sources, using the Java $JDKV classes"; fi
    done
    # java/util/currency.data: the runtime images are jlinked from the classes the build leaves in jdkbuild*/jdk/modules/java.base, and "make java.base-libs" never runs the data generator ("Gendata.gmk"), so
    # java.util.Currency died with "InternalError: Currency data not found" (Forge's title screen: MessageFormat -> NumberFormat.getCurrencyInstance, log abaa1787). The title already patches java.base
    # (--patch-module, base-patch<V>): the file goes in there, copied from the build tree or generated from the JDK's own sources (same tool and input as the JDK build, byte-identical).
    for v in $JDKLIST; do
      [ "$v" = 8 ] && continue   # Java 8 reads lib/currency.data of its jre directory, which its build ships
      CD=$T/assets/lib/base-patch$v/java/util/currency.data; mkdir -p $(dirname $CD)
      SUFB=""; [ "$v" != 17 ] && SUFB=$v
      SRCF=$(ls $HOME/jdkbuild$SUFB/jdk/modules/java.base/java/util/currency.data $HOME/jdkbuild${SUFB}c2/jdk/modules/java.base/java/util/currency.data 2>/dev/null | head -1)
      if [ -n "$SRCF" ]; then cp "$SRCF" $CD
      else
        GS=$HOME/jdk${v}u/make/jdk/src/classes/build/tools/generatecurrencydata/GenerateCurrencyData.java; GP=$(ls $HOME/jdk${v}u/src/java.base/share/data/currency/CurrencyData.properties $HOME/jdk${v}u/make/data/currency/CurrencyData.properties $HOME/jdk${v}u/make/data/currency/currency.properties 2>/dev/null | head -1)   # 17 keeps it under make/data, 21+ under src/java.base/share/data
        GJ=/usr/lib/jvm/java-$v-openjdk-amd64/bin; [ -x $GJ/javac ] || GJ=/usr/lib/jvm/java-17-openjdk-amd64/bin
        if [ ! -f $GS ] || [ -z "$GP" ] || [ ! -f "$GP" ]; then   # no JDK source tree here: the two files come from GitHub (openjdk/jdk<v>u)
          RAW=https://raw.githubusercontent.com/openjdk/jdk${v}u/master; DL=$HOME/currency_src$v; mkdir -p $DL
          [ -f $GS ] || { curl -fsSL $RAW/make/jdk/src/classes/build/tools/generatecurrencydata/GenerateCurrencyData.java -o $DL/GenerateCurrencyData.java && GS=$DL/GenerateCurrencyData.java; }
          if [ -z "$GP" ] || [ ! -f "$GP" ]; then GP=""; for u in make/data/currency/CurrencyData.properties src/java.base/share/data/currency/CurrencyData.properties; do curl -fsSL $RAW/$u -o $DL/CurrencyData.properties && [ -s $DL/CurrencyData.properties ] && GP=$DL/CurrencyData.properties && break; done; fi
        fi
        if [ -f $GS ] && [ -n "$GP" ] && [ -f "$GP" ]; then rm -rf $HOME/currency_gen$v && mkdir -p $HOME/currency_gen$v && $GJ/javac -d $HOME/currency_gen$v $GS && $GJ/java -cp $HOME/currency_gen$v build.tools.generatecurrencydata.GenerateCurrencyData -o $CD -i $GP; fi
      fi
      if [ -s $CD ]; then echo "base-patch$v: java/util/currency.data ($(wc -c < $CD) bytes)"; else rm -f $CD; echo "== WARNING: no currency.data for Java $v (no build tree, no ~/jdk${v}u sources): java.util.Currency / NumberFormat.getCurrencyInstance will fail (Forge's title screen)"; fi
    done
    case " $JDKLIST " in *" 8 "*)   # Java 8: AWT-lite as a jar that goes in FRONT of its rt.jar (-Xbootclasspath/p); the rest of java.desktop (java.beans, javax.script, datatransfer) is in its rt.jar
      bash $HERE/build_awtlite.sh 8 $(jdkdir 8)/jdk/lib/rt.jar $T/assets/lib/desktop-lite8.jar /usr/lib/jvm/java-8-openjdk-amd64/bin/javac || exit 1
      P8=$HOME/proc_stub8; rm -rf $P8 && mkdir -p $P8/src/java/lang $P8/out && printf '%s\n' "$PIJ" > $P8/src/java/lang/ProcessImpl.java   # Java 8 has no modules: the stub goes into the jar in front of rt.jar
      /usr/lib/jvm/java-8-openjdk-amd64/bin/javac -d $P8/out $P8/src/java/lang/ProcessImpl.java && (cd $P8/out && /usr/lib/jvm/java-8-openjdk-amd64/bin/jar uf $T/assets/lib/desktop-lite8.jar java/lang/ProcessImpl.class) && echo "java 8: ProcessImpl stub added to desktop-lite8.jar" || exit 1;;
    esac
    [ -f $HERE/log4j2-ps5.xml ] || { echo "missing $HERE/log4j2-ps5.xml"; exit 1; }
    cp $HERE/log4j2-ps5.xml $T/assets/mc/log4j2.xml
    # Microsoft sign-in: the Azure app (client) id comes from the build command (MSA_CLIENT_ID=<guid>), never from the sources; McAuth reads /app0/assets/mc/msa_client_id (launcher.cfg msaClientId overrides it). Not set = no file.
    rm -f $T/assets/mc/msa_client_id
    if [ -n "${MSA_CLIENT_ID:-}" ]; then
      echo "$MSA_CLIENT_ID" | grep -Eq '^([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}|[0-9a-fA-F]{16})$' || { echo "MSA_CLIENT_ID is neither a GUID (Azure Application (client) ID) nor a 16-digit hex legacy id"; exit 1; }
      printf '%s\n' "$MSA_CLIENT_ID" > $T/assets/mc/msa_client_id; echo "== Microsoft sign-in: client id staged from MSA_CLIENT_ID"
    else echo "== Microsoft sign-in: no MSA_CLIENT_ID given (launcher.cfg msaClientId is the only source)"; fi
    echo "#define MC_CP \"$MC_CP\"" >> $T/include/ps5_conf.h
    /usr/lib/jvm/java-17-openjdk-amd64/bin/javac --release $JREL -cp $CPJ -d $T/assets/hello $HERE/McBoot.java $HERE/McDownload.java $HERE/Launcher.java $HERE/McAuth.java $HERE/McMods.java $HERE/McForge.java $HERE/Kit.java $HERE/DnsContextFactory.java
    echo "== [${SECONDS}s] mc: client + $(ls $T/assets/mc/lib | wc -l) libraries staged under assets/mc"
  fi
  cp $OUT/liblwjgl_ps5.a $T/vendor/whole-lwjgl.a
  RT=$(clang-18 --print-resource-dir)/lib/linux/libclang_rt.builtins-x86_64.a
  if [ -n "$SDK100" ]; then   # 1.0.0: libPS5OpenGLCore33.a is a GROUP() text script with bare names; list the component archives by absolute path
    GLIBS=$(sed -n 's/^ *\(lib[A-Za-z0-9_.+-]*\.a\) *$/\1/p' $GLSDK/lib/libPS5OpenGL.a | while read l; do printf '"%s/lib/%s" ' "$GLSDK" "$l"; done)
    [ -n "$GLIBS" ] || { echo "could not read the component list from $GLSDK/lib/libPS5OpenGL.a"; exit 1; }
  else
    GLIBS="\"$GLSDK/lib/libPS5OpenGLCore33.a\""
  fi
  { echo "SEARCH_DIR(\"$S/target/lib\")"; echo "EXTERN($([ "$ASYNCPREP" = 1 ] && echo ps5_agc_gate2_run_sync || echo ps5_agc_gate2_run))"
    echo "GROUP ( $GLIBS \"$GLSDK/lib/libSceAgc.so\" \"$GLSDK/lib/libSceAgcDriver.so\" \"$S/target/lib/libunwind.a\" \"$S/target/lib/libc++abi.a\" \"$S/target/lib/libc++.a\" \"$RT\" )"; } > $T/vendor/libps5_opengl_group.a
fi

# Hello reads this to choose McBoot or GLTest; written fresh by every build so stale files on the console cannot change the choice
if [ -n "$MC" ]; then echo mc > $T/assets/hello/mode.txt; else echo plain > $T/assets/hello/mode.txt; fi
echo "== [${SECONDS}s] BUILD MODE: $(cat $T/assets/hello/mode.txt)   (mc = Minecraft boot, plain = GL/color/triangle tester)"
echo "== [${SECONDS}s] build + deploy"
cd $T
export APP_INCLUDE_PATHS=include
[ -n "$NATIVEUI" ] && export APP_INCLUDE_PATHS="include src/kit" APP_DEFINITIONS="GL_GLEXT_PROTOTYPES=1"   # kit_launcher/src/GL/glcorearb.h is the SDK 1.0.1 header: direct gl* calls, the symbols come from libps5_opengl_group.a
export APP_STATIC_ARCHIVES="$JDKARCH"
[ -n "$GL" ] && APP_STATIC_ARCHIVES="$APP_STATIC_ARCHIVES vendor/whole-lwjgl.a vendor/libps5_opengl_group.a"
[ -n "$SELFTEST" ] && APP_STATIC_ARCHIVES="vendor/whole-lwjgl.a vendor/libps5_opengl_group.a"   # no HotSpot/JDK objects in the bisect build
[ -n "$BARE" ] && APP_STATIC_ARCHIVES="vendor/libps5_opengl_group.a"   # bare: SDK only, not even the LWJGL/shim archive
if [ -n "$VKMODE" ]; then
  export T HERE S JDKLIST TID PS5 GLSDK VKINC TPL=$VKTPL VK=$VKDIR
  bash $HERE/vk_relink.sh
else
  make deploy PS5_HOST=$PS5
fi
echo "== total build+deploy time: ${SECONDS}s"
echo "== BUILD FINISHED $(date '+%Y-%m-%d %H:%M:%S') (this clock is the WSL machine's; the title's build marker is back2l-14)"
echo "== [${SECONDS}s] deployed. Start the PowerShell listener on port 9999, fully close the title, restart ShadowMountPlus, launch '${APPN}'."
