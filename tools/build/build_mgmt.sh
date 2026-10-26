#!/bin/bash
# build_mgmt.sh: build the java.management native library (libmanagement) as a static archive into ~/ps5jvm/libmanagement.a.
# Why: MC's startup code calls ManagementFactory, whose static init does System.loadLibrary("management").
# Re-uses the existing configured build in ~/jdkbuild (run build_jdk.sh once before). Safe to re-run; incremental.
set -e
S=${PS5_SDK:-$HOME/ps5probe/.deps/native/ps5-payload-sdk}
JDKV=${JDKV:-17}; SUF=""; [ "$JDKV" != 17 ] && SUF=$JDKV   # JDKV=21: use the build from JDKV=21 build_jdk.sh (~/jdkbuild21 -> ~/ps5jvm21)
[ "$C2" = 1 ] && SUF="${SUF}c2"   # C2=1: use the server-VM build (build_jdk.sh with C2=1)
OUT=$HOME/ps5jvm$SUF; B=$HOME/jdkbuild$SUF
[ -f $B/spec.gmk ] || { echo "$B is not configured: run build_jdk.sh first"; exit 1; }
CPPX="/usr/lib/llvm-18/bin/clang -E -target x86_64-sie-ps5 -isysroot $S -isystem $S/target/include -isystem $S/target/include_common"
cd $B
make java.management-libs java.management-static-libs JOBS=$(nproc) -k CPP="$CPPX" 2>&1 | tee $HOME/mgmt$SUF.log | grep --line-buffered -E "^(Building target|Compiling|Creating|Linking|Finished)" || true
A=$(find $B/support/native/java.management -name 'libmanagement.a' 2>/dev/null | head -1)
[ -n "$A" ] || { echo "libmanagement.a not produced (see ~/mgmt.log)"; grep -E "error:|No rule|Unknown target" $HOME/mgmt$SUF.log | head -10; exit 1; }
cp "$A" $OUT/libmanagement.a
echo "== $OUT/libmanagement.a ($(du -h $OUT/libmanagement.a | cut -f1))"
"$S/bin/llvm-nm" -g --defined-only $OUT/libmanagement.a 2>/dev/null | grep -c " T Java_" | sed 's/^/Java_ entry points: /'
echo "== undefined symbols (JVM_GetManagement etc. must come from whole-hotspot.a):"
"$S/bin/llvm-nm" -g --undefined-only $OUT/libmanagement.a 2>/dev/null | awk '{print $2}' | grep -E "^(JVM_|jio_)" | sort -u | head -20
echo "== HotSpot exports JVM_GetManagement?"
"$S/bin/llvm-nm" -g --defined-only $OUT/whole-hotspot.a 2>/dev/null | grep -c " T JVM_GetManagement" | sed 's/^/count: /'
if [ "$JDKV" != 17 ]; then   # jdk.management natives (JVM flag queries for HotSpotDiagnosticMXBean, GC extensions): libmanagement_ext
  make jdk.management-libs jdk.management-static-libs JOBS=$(nproc) -k CPP="$CPPX" 2>&1 | tee $HOME/mgmtext$SUF.log | grep --line-buffered -E "^(Building target|Compiling|Creating|Linking|Finished)" || true
  A=$(find $B/support/native/jdk.management -name 'libmanagement_ext.a' 2>/dev/null | head -1)
  [ -n "$A" ] || { echo "libmanagement_ext.a not produced (see ~/mgmtext$SUF.log)"; grep -E "error:|No rule|Unknown target" $HOME/mgmtext$SUF.log | head -10; exit 1; }
  cp "$A" $OUT/libmanagement_ext.a
  echo "== $OUT/libmanagement_ext.a ($(du -h $OUT/libmanagement_ext.a | cut -f1))"
  "$S/bin/llvm-nm" -g --defined-only $OUT/libmanagement_ext.a 2>/dev/null | grep -cE " T (Java_|JNI_OnLoad)" | sed 's/^/entry points: /'
fi
echo "Next: bash make_jvm_title.sh <PC> <PS5> jit gl mc"
