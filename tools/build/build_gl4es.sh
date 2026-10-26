#!/bin/bash
# build_gl4es.sh: fixed-function OpenGL for old Minecraft versions. Builds gl4es (github.com/ptitSeb/gl4es, MIT) + ps5gl4es.c into ~/gl4es_build/libgl4es_ps5.a (objects alongside).
#   * gl4es is cloned at a pinned commit into ~/gl4es and patched in place by this script (one line; the build stops if the line is not found)
#   * built static, without its EGL/GLX/dlopen loader (NOEGL NOX11 NO_LOADER STATICLIB NO_INIT_CONSTRUCTOR): it runs on the context the title already has
#   * every global symbol of gl4es except the few ps5gl4es.c needs is renamed g4_<name> (objcopy), so nothing can collide with HotSpot, LWJGL, FreeType, shaderc ...
# build_lwjgl.sh calls this (GL4ES=0 skips it) and puts the objects (~/gl4es_build/*.o) into liblwjgl_ps5.a; it can also be run alone:  bash build_gl4es.sh
# Sandbox test (no console): HOME=/some/dir G4CC=clang G4OBJCOPY=objcopy G4NM=nm G4AR=ar bash build_gl4es.sh   (the archive for the tests is $HOME/gl4es_build/libgl4es_ps5.a)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
T=${T:-$HOME/ps5probe}; OUT=${OUT:-$HOME/ps5jvm}; G=${GL4ES_DIR:-$HOME/gl4es}; COMMIT=${GL4ES_COMMIT:-ec16bedd8819c475326f4f1a3063772c6d986e06}
PSDK=$T/.deps/native/ps5-payload-sdk
if [ -n "$G4CC" ]; then CC="$G4CC"; OBJCOPY=${G4OBJCOPY:-objcopy}; NM=${G4NM:-nm}; AR=${G4AR:-ar}
else
  CCX=; [ "$NOCCACHE" != 1 ] && command -v ccache >/dev/null && CCX="USE_CCACHE=1"   # unchanged sources compile in milliseconds
  CC="env PS5_PAYLOAD_SDK=$PSDK PS5_CLANG=clang-18 $CCX sh $T/tooling/prospero-clang18"
  OBJCOPY=$(ls $PSDK/bin/llvm-objcopy 2>/dev/null || command -v llvm-objcopy-18 || command -v llvm-objcopy || command -v objcopy)
  NM=$(ls $PSDK/bin/llvm-nm 2>/dev/null || command -v llvm-nm-18 || command -v llvm-nm || command -v nm)
  AR=$(ls $PSDK/bin/llvm-ar 2>/dev/null || command -v llvm-ar-18 || command -v llvm-ar || command -v ar)
fi
mkdir -p $OUT
if [ ! -d $G/src/gl ]; then git clone -q https://github.com/ptitSeb/gl4es $G; fi
( cd $G && git checkout -q $COMMIT 2>/dev/null || { git fetch -q --depth 1 origin $COMMIT && git checkout -q $COMMIT; } ) || { echo "gl4es: cannot check out $COMMIT"; exit 1; }
# the one source patch: gl4es must not probe the hardware itself (desktop core would answer its GLES questions with nothing); ps5gl4es.c sets the capabilities
python3 - "$G/src/gl/init.c" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
a = "int gl4es_notest = !gles_getProcAddress;"
if a in s: s = s.replace(a, "int gl4es_notest = 1;  /* ps5: capabilities are set by ps5gl4es.c */")
elif "int gl4es_notest = 1;  /* ps5" not in s: sys.exit("gl4es: init.c does not contain the expected line (version changed?)")
open(p, "w").write(s)
PY
B=$HOME/gl4es_build; rm -rf $B; mkdir -p $B
FL="-std=gnu11 -O2 -w -fPIC -fvisibility=default -DNOEGL -DNOX11 -DNO_GBM -DSTATICLIB -DNO_LOADER -DNO_INIT_CONSTRUCTOR -DDEFAULT_ES=2 -I$G/include -I$G/src -I$G/src/gl -I$G/src/glx"
SRCS="$(sed -n '/^SET(GL_SRC/,/^)/p' $G/src/CMakeLists.txt | grep -o '/gl/[A-Za-z0-9_/]*\.c' | sed "s|^/|$G/src/|") $G/src/glx/hardext.c"
n=0
for f in $SRCS; do o=$B/$(echo ${f#$G/src/} | tr / _).o; $CC $FL -c $f -o $o 2>$o.err || { echo "gl4es: FAILED $f"; head -5 $o.err; exit 1; }; n=$((n+1)); done
echo "gl4es: $n files compiled"
# rename: every global that is not gl4es_* and not needed by ps5gl4es.c
KEEP="^(hardext|globals4es|glstate|set_getprocaddress|set_getmainfbsize|initialize_gl4es|close_gl4es)$"
$NM --defined-only -g $B/*.o 2>/dev/null | awk '$2 ~ /^[TDBRCVWS]$/ {print $3}' | sort -u | grep -v '^gl4es_' | grep -vE "$KEEP" | awk '{print $1" g4_"$1}' > $B/rename.txt
for o in $B/*.o; do $OBJCOPY --redefine-syms=$B/rename.txt $o; done
echo "gl4es: $(wc -l < $B/rename.txt) global symbols renamed g4_*"
$CC $FL -c $HERE/ps5gl4es.c -o $B/ps5gl4es.o 2>$B/ps5gl4es.err || { echo "gl4es: FAILED ps5gl4es.c"; head -20 $B/ps5gl4es.err; exit 1; }
$AR rcs $B/libgl4es_ps5.a $B/*.o
echo "gl4es: $n files + ps5gl4es.c built ($(ls -l $B/libgl4es_ps5.a | awk '{print $5}') bytes as an archive, objects in $B)"
