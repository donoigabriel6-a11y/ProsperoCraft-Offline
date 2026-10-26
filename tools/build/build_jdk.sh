#!/bin/bash
# build_jdk.sh: build OpenJDK 17 (default), 21 (JDKV=21) or 25 (JDKV=25) for the PS5 SDK in WSL/Ubuntu:
#   HotSpot (C1 client VM) objects + static java.base native libs + java.base runtime image.
# Needs ~/ps5probe built once (provides the SDK). Outputs in ~/ps5jvm:
#   libjvm.a  libjava.a libzip.a libnet.a libnio.a libjimage.a libverify.a libfdlibm.a  jdk/lib/modules
# Run:  bash build_jdk.sh      (first run: ~30-60 min on 16 cores; safe to re-run)
#       JDKV=21 bash build_jdk.sh   -> OpenJDK 21 (jdk-21.0.12-ga, ps5_jdk_patch21.py): sources ~/jdk21u, build dir ~/jdkbuild21, outputs ~/ps5jvm21, logs ~/*21.log; the 17 outputs stay untouched.
#       C2=1 bash build_jdk.sh (any JDKV) -> HotSpot SERVER VM (C1 + C2, tiered) instead of the client VM; separate dirs: ~/jdkbuild<V>c2 -> ~/ps5jvm<V>c2, logs ~/*c2.log; the client outputs stay untouched (java.base is rebuilt: ~1 h).
#       The 21 build needs a JDK 21 as boot/build JDK and for jmod/jlink (class files are major 65); the script installs openjdk-21-jdk-headless.
#       JDKV=25 bash build_jdk.sh   -> OpenJDK 25 (jdk-25.0.4.1-ga, ps5_jdk_patch25.py): sources ~/jdk25u, build dir ~/jdkbuild25, outputs ~/ps5jvm25, logs ~/*25.log. Boot/build JDK 25 (openjdk-25-jdk-headless, Ubuntu 24.04 noble-updates).
#       JDKV=8 bash build_jdk.sh    -> OpenJDK 8 (jdk8u, ps5_jdk_patch8.py): sources ~/jdk8u, build dir ~/jdkbuild8, outputs ~/ps5jvm8, logs ~/*8.log. Boot JDK 8 (openjdk-8-jdk-headless). Different build system (autoconf + legacy hotspot/make, no modules/jlink): the
#         result is the same shape (whole-hotspot.a + static libs) but the class library is a classic jre dir (jdk/lib/rt.jar, ext/, security/) instead of lib/modules. C1 client VM only (C2=1 is not supported for 8).
set -e
unset JAVA_TOOL_OPTIONS _JAVA_OPTIONS   # configure refuses to continue when the boot JDK prints "Picked up JAVA_TOOL_OPTIONS"
S=${PS5_SDK:-$HOME/ps5probe/.deps/native/ps5-payload-sdk}
HERE=$(cd "$(dirname "$0")" && pwd)
JDKV=${JDKV:-17}
case $JDKV in
  17) SRC=$HOME/jdk17u; PATCH=ps5_jdk_patch.py; OUT=$HOME/ps5jvm; B=$HOME/jdkbuild; SUF=""; MODVER=17.0.20; PKG=openjdk-17-jdk-headless
      FETCHREF=3b0480102f8595deb241a666498f77644d5ec376; REPO=https://github.com/openjdk/jdk17u.git ;;
  21) SRC=$HOME/jdk21u; PATCH=ps5_jdk_patch21.py; OUT=$HOME/ps5jvm21; B=$HOME/jdkbuild21; SUF=21; MODVER=21.0.12; PKG=openjdk-21-jdk-headless
      FETCHREF=refs/tags/jdk-21.0.12-ga; REPO=https://github.com/openjdk/jdk21u.git ;;
  25) SRC=$HOME/jdk25u; PATCH=ps5_jdk_patch25.py; OUT=$HOME/ps5jvm25; B=$HOME/jdkbuild25; SUF=25; MODVER=25.0.4; PKG=openjdk-25-jdk-headless
      FETCHREF=refs/tags/jdk-25.0.4.1-ga; REPO=https://github.com/openjdk/jdk25u.git ;;
  8)  SRC=$HOME/jdk8u; PATCH=ps5_jdk_patch8.py; OUT=$HOME/ps5jvm8; B=$HOME/jdkbuild8; SUF=8; MODVER=8; PKG="openjdk-8-jdk-headless build-essential"
      FETCHREF=4efe36434f44bafb854e602ccbbe1b5696fce1a2; REPO=https://github.com/openjdk/jdk8u.git ;;
  *) echo "JDKV must be 8, 17, 21 or 25"; exit 1 ;;
esac
# JDK 21's config.guess turns "x86_64-pc-linux-gnu" (what this WSL reports) into "x86_64-pc-wsl" and configure then believes it runs on Windows ("could not be imported");
# JDK 17's pattern only matched "unknown-linux-gnu". Name the build platform explicitly.
JVMVAR=client; if [ "$C2" = 1 ]; then JVMVAR=server; SUF="${SUF}c2"; OUT="${OUT}c2"; B="${B}c2"; fi   # C2=1: server VM (C1 + C2) in its own build dir and output dir
BUILDARG=; [ "$JDKV" != 17 ] && BUILDARG="--build=x86_64-unknown-linux-gnu"
NOSSP=; [ "$JDKV" = 25 ] && NOSSP=" -fno-stack-protector"   # JDK 25 builds with -fstack-protector, which imports __stack_chk_guard/__stack_chk_fail: not worth the risk on this console, and 17/21 never had it
COMPRESS=0; [ "$JDKV" = 25 ] && COMPRESS=zip-0   # JDK 25 deprecates the numeric form of --compress
TARGETPLAT=bsd-amd64; [ "$JDKV" != 17 ] && TARGETPLAT=linux-amd64   # JDK 21 jmod/jlink parse the OS into an enum without BSD; the value only labels the image
LIBS="java zip net nio jimage verify fdlibm"; [ "$JDKV" != 17 ] && LIBS="java zip net nio jimage verify"   # JDK 21 has no native fdlibm any more (StrictMath is Java)
[ "$JDKV" = 8 ] && [ "$C2" = 1 ] && { echo "JDKV=8 has no C2 build (C1 client VM only)"; exit 1; }
JDK17=/usr/lib/jvm/java-$JDKV-openjdk-amd64   # boot JDK, build JDK and jmod/jlink: same major version as the one being built (variable name kept from the 17 script)
[ -x "$S/bin/prospero-clang" ] || { echo "SDK not found at $S (build ~/ps5probe first)"; exit 1; }

echo "== packages"
sudo apt-get install -y $PKG autoconf zip unzip clang-18 lld-18
sudo ln -sf /usr/bin/clang-18 /usr/local/bin/clang
sudo ln -sf /usr/bin/clang++-18 /usr/local/bin/clang++

echo "== SDK stubs"
for f in cups/cups.h cups/ppd.h alsa/asoundlib.h fontconfig/fontconfig.h; do
  mkdir -p "$S/target/include/$(dirname $f)"; echo "/* stub */" > "$S/target/include/$f"
done
if [ "$JDKV" != 17 ]; then
  # JDK 21+ configure wants the X11 headers even for --enable-headless-only (17 skipped them); java.desktop is not built here, so empty stubs and an explicit --x-includes/--x-libraries (which makes AC_PATH_X skip its search) are enough
  for f in X11/Xlib.h X11/Xutil.h X11/Intrinsic.h X11/extensions/shape.h X11/extensions/Xrender.h X11/extensions/XTest.h X11/extensions/Xrandr.h; do
    mkdir -p "$S/target/include/$(dirname $f)"; echo "/* stub */" > "$S/target/include/$f"
  done
  BUILDARG="$BUILDARG --x-includes=$S/target/include --x-libraries=$S/target/lib"
fi
if [ ! -f "$S/target/lib/libm.a" ]; then
  echo "" > /tmp/e.c; "$S/bin/prospero-clang" -c /tmp/e.c -o /tmp/e.o; "$S/bin/llvm-ar" rcs "$S/target/lib/libm.a" /tmp/e.o
fi

echo "== source (pinned commit)"
if [ ! -d $SRC/.git ]; then
  git init -q $SRC && cd $SRC
  git remote add origin $REPO
  git fetch --depth 1 origin $FETCHREF
  git checkout -q FETCH_HEAD
fi
python3 "$HERE/$PATCH" $SRC

if [ "$JDKV" = 8 ]; then
  # ---------------------------------------------------------------------------------------------------------------------------------------
  # OpenJDK 8: configure/make (JDK 8 style). Same outputs as the other versions: $OUT/whole-hotspot.a, $OUT/lib<name>.a, $OUT/jdk (runtime files).
  # ---------------------------------------------------------------------------------------------------------------------------------------
  echo "== configure (JDK 8)"
  mkdir -p $B && cd $B
  XF="-D_ALLBSD_SOURCE -DUSE_LIBRARY_BASED_TLS_ONLY -fno-stack-protector -Wno-implicit-function-declaration -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-incompatible-function-pointer-types"   # no stack protector: the SDK libc has no __stack_chk_*; clang 18 makes the K&R-era JDK 8 sources errors otherwise
  bash $SRC/configure --build=x86_64-unknown-linux-gnu --host=x86_64-unknown-freebsd13 --with-boot-jdk=$JDK17 --with-toolchain-type=clang --with-sysroot=$S \
    --with-jvm-variants=client --with-debug-level=release --disable-headful --with-native-debug-symbols=none --with-zlib=bundled \
    --with-cups-include=$S/target/include --with-freetype=bundled --with-alsa-include=$S/target/include \
    "--with-extra-cflags=$XF" "--with-extra-cxxflags=$XF" "--with-extra-ldflags=-Wl,--no-pie" \
    CC=$S/bin/prospero-clang CXX=$S/bin/prospero-clang++ \
    2>&1 | tee $HOME/jdk_configure$SUF.log | grep --line-buffered -E "^(checking (for|if) (cups|compress|zlib|Build|the Boot)|configure:|Using|\*\*\*|Configuration summary)"
  [ ${PIPESTATUS[0]} -eq 0 ] || { echo "configure FAILED:"; tail -20 $HOME/jdk_configure$SUF.log; exit 1; }
  sed -i '/^CFLAGS_JDKLIB *:=/ { /-D_WITH_GETLINE/! s|$| -include sys/types.h -include sys/socket.h -include netinet/in.h -include arpa/inet.h -include unistd.h -include stdio.h -D_WITH_GETLINE -DIPV6_ADD_MEMBERSHIP=IPV6_JOIN_GROUP -DIPV6_DROP_MEMBERSHIP=IPV6_LEAVE_GROUP| }' spec.gmk
  export OS=bsd   # the legacy hotspot makefiles pick their platform with `uname -s`
  N=$(nproc)
  echo "== HotSpot objects (C1)"
  rm -f hotspot/_hotspot.timestamp
  OBJS=$B/hotspot/bsd_amd64_compiler1/product
  # The legacy hotspot makefiles do not make precompiled.hpp.pch (or the objects) depend on the headers the patch edits: a changed patch needs a clean HotSpot build, or the old objects are packed again.
  PST=$(sha256sum "$HERE/$PATCH" | cut -d' ' -f1)
  if [ "$(cat $B/.hotspot_patch_stamp 2>/dev/null)" != "$PST" ]; then
    echo "   patch changed since the last HotSpot build: clean HotSpot objects"
    rm -rf $B/hotspot $B/.hotspot_patch_stamp   # the whole tree: the generated makefiles live next to the objects, deleting only the objects leaves make with nothing to run
  fi
  make hotspot JOBS=$N -k COMPILER_WARNINGS_FATAL=false "ADD_SA_BINARIES/x86=" 2>&1 | tee $HOME/hotspot$SUF.log | grep --line-buffered -E "^(Compiling|Linking|Finished|## )" || true
  if grep -qE "error:|\] Error [0-9]" $HOME/hotspot$SUF.log; then echo "HotSpot build FAILED (see ~/hotspot$SUF.log):"; grep -E "error:|\] Error [0-9]" $HOME/hotspot$SUF.log | head; exit 1; fi
  [ -f $OBJS/ps5_mem.o ] || { echo "HotSpot did not compile (see ~/hotspot$SUF.log)"; grep 'error:' $HOME/hotspot$SUF.log | head; exit 1; }
  echo "$PST" > $B/.hotspot_patch_stamp
  echo "== langtools / corba / jaxp / jaxws, class library, native libs (libnpt libsctp libjli libunpack errors are expected and ignored)"
  make langtools corba jaxp jaxws JOBS=$N -k > $HOME/java8_tools.log 2>&1 || true
  make jdk-only JDK_TARGET=classes JOBS=$N -k > $HOME/java8_classes.log 2>&1 || true
  make jdk-only JDK_TARGET=libs-only JOBS=$N -k > $HOME/java8_libs.log 2>&1 || true
  make jdk-only JDK_TARGET=securityjars-only JOBS=$N -k > $HOME/java8_secjars.log 2>&1 || true
  make jdk-only JDK_TARGET=images JOBS=$N -k > $HOME/java8_images.log 2>&1 || true   # rt.jar, resources.jar, charsets.jar, jsse.jar, ext/*.jar (the sa-jdi / nashorn / zipfs / src.zip failures do not matter)
  [ -f $B/images/lib/rt.jar ] || { echo "rt.jar missing (see ~/java8_images.log, ~/java8_classes.log)"; grep -E 'error:|\*\*\*' $HOME/java8_classes.log $HOME/java8_images.log | head; exit 1; }
  LIBS8="java zip net nio verify fdlibm management sunec"
  for l in $LIBS8; do ls $B/jdk/objs/lib$l/*.o > /dev/null 2>&1 || { echo "no objects for lib$l (see ~/java8_libs.log)"; grep 'error:' $HOME/java8_libs.log | head; exit 1; }; done

  echo "== collect outputs"
  rm -rf $OUT; mkdir -p $OUT; W=$OUT/.stage; mkdir -p $W
  "$S/bin/llvm-ar" rcs $OUT/whole-hotspot.a $(find $OBJS -name '*.o')
  # The JDK 8 libraries are linked into the title: JNI_OnLoad is renamed to JNI_OnLoad_<lib> (System.loadLibrary finds a "built-in" library by that name: ClassLoader.findBuiltinLib),
  # libraries without a JNI_OnLoad get a stub, and libjava's jio_* wrappers go (HotSpot defines them).
  for l in $LIBS8; do
    mkdir -p $W/$l
    HASOL=0
    for f in $B/jdk/objs/lib$l/*.o; do
      b=$(basename $f)
      [ "$l-$b" = "java-jio.o" ] && continue
      if "$S/bin/llvm-nm" -g --defined-only $f | grep -q " T JNI_OnLoad$"; then "$S/bin/llvm-objcopy" --redefine-sym JNI_OnLoad=JNI_OnLoad_real_$l $f $W/$l/$b; HASOL=1; else cp $f $W/$l/$b; fi
    done
    if [ $HASOL = 1 ]; then   # JDK 8's own JNI_OnLoad functions answer JNI 1.2/1.4; ClassLoader.c insists on >= 1.8 for built-in libraries: wrap the real one and raise the answer
      printf 'extern int JNI_OnLoad_real_%s(void *, void *);\nint JNI_OnLoad_%s(void *vm, void *reserved) { int v = JNI_OnLoad_real_%s(vm, reserved); return (v > 0 && v < 0x00010008) ? 0x00010008 : v; }\n' $l $l $l > $W/$l/onload_wrap.c
      $S/bin/prospero-clang -c -O1 -fno-stack-protector -o $W/$l/onload_wrap.o $W/$l/onload_wrap.c
    fi
    if [ "$l" != fdlibm ] && ! "$S/bin/llvm-nm" -g --defined-only $W/$l/*.o | grep -q " T JNI_OnLoad_$l$"; then
      printf 'int JNI_OnLoad_%s(void *vm, void *reserved) { (void)vm; (void)reserved; return 0x00010008; }\n' $l > $W/$l/onload_stub.c
      $S/bin/prospero-clang -c -O1 -fno-stack-protector -o $W/$l/onload_stub.o $W/$l/onload_stub.c
    fi
    "$S/bin/llvm-ar" rcs $OUT/lib$l.a $W/$l/*.o
  done
  rm -rf $W
  echo "== runtime files (jre layout: lib/rt.jar, ext/, security/ ...)"
  J=$OUT/jdk; mkdir -p $J/lib/ext $J/lib/amd64 $J/lib/security
  touch $J/lib/amd64/.keep
  # data files, security (java.security/java.policy/policy jars), tzdb.dat ... from the build; the CA store from the boot JDK (the Debian ca-certificates bundle is far newer than the one in the sources)
  (cd $B/jdk/lib && tar cf - --exclude=./amd64 --exclude=./jce.jar --exclude='./_the*' --exclude=./images --exclude=./jfr --exclude='*.idl' --exclude='jvm.hprof.txt' --exclude='psfont.properties.ja' .) | (cd $J/lib && tar xf -)
  cp -L $JDK17/jre/lib/security/cacerts $J/lib/security/cacerts 2>/dev/null || true
  cp $B/jdk/lib/ext/*.jar $J/lib/ext/
  cp $B/images/lib/ext/localedata.jar $B/images/lib/ext/cldrdata.jar $B/images/lib/ext/dnsns.jar $J/lib/ext/
  cp $B/images/lib/resources.jar $B/images/lib/charsets.jar $B/images/lib/jsse.jar $B/images/lib/jce.jar $J/lib/ 2>/dev/null || cp $B/jdk/lib/jce.jar $J/lib/
  [ -f $J/lib/jce.jar ] || cp $B/jdk/lib/jce.jar $J/lib/
  [ -f $J/lib/ext/zipfs.jar ] || OUT=$OUT bash "$(dirname "$0")/build_zipfs8.sh"   # the "jar" file system provider is a JDK 8 DEMO that this build never made: without it 1.12.2 / 1.16.5 fail with Provider "jar" not found
  # rt.jar: stored (uncompressed) 59 MB in the build; deflate it and leave out what a console without a display never runs (Swing, X11/Motif AWT, CORBA, sound, accessibility, applets, the imageio plugins)
  python3 -I - $B/images/lib/rt.jar $J/lib/rt.jar <<'PYRT'
import sys, zipfile
src, dst = sys.argv[1:3]
DROP = ("javax/swing/", "com/sun/java/swing/", "sun/swing/", "sun/awt/X11/", "sun/awt/motif/", "com/sun/corba/", "org/omg/", "javax/rmi/", "com/sun/media/sound/",
        "com/sun/java/accessibility/", "sun/applet/", "java/applet/", "com/sun/imageio/", "sun/print/", "javax/print/", "com/sun/rowset/", "sun/awt/windows/", "sun/lwawt/")
n = k = 0
with zipfile.ZipFile(src) as zi, zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as zo:
    for it in zi.infolist():
        n += 1
        if it.filename.startswith(DROP): continue
        zo.writestr(it.filename, zi.read(it.filename)); k += 1
print("rt.jar: %d of %d entries kept" % (k, n))
PYRT
  ls -la $OUT/whole-hotspot.a $J/lib/rt.jar | awk '{print $5, $9}'
  echo "== done (JDK 8). Expect whole-hotspot.a (~23 MB) and rt.jar (~23 MB) above."
  echo "   Next: bash make_jvm_title.sh <PC_IP> <PS5_IP> ...   (Java 8 is linked automatically next to 17/21/25 when ~/ps5jvm8 exists; JDKS=\"17 21 25\" leaves it out)"
  exit 0
fi

[ "$JDKV" != 17 ] && rm -f $B/support/gensrc/java.base/jdk/internal/util/PlatformProps.java $B/jdk/modules/java.base/jdk/internal/util/PlatformProps*.class   # regenerate with the macos OS string
echo "== configure"
mkdir -p $B && cd $B
bash $SRC/configure $BUILDARG --with-boot-jdk=$JDK17 --with-build-jdk=$JDK17 \
  --openjdk-target=x86_64-unknown-freebsd13 --with-toolchain-type=clang --with-sysroot=$S \
  --with-jvm-variants=$JVMVAR --with-debug-level=release --enable-headless-only \
  --with-native-debug-symbols=none --disable-warnings-as-errors --with-zlib=bundled \
  --with-cups-include=$S/target/include --with-freetype=bundled \
  --with-alsa-include=$S/target/include --with-fontconfig-include=$S/target/include \
  "--with-extra-cflags=-D_ALLBSD_SOURCE -DUSE_LIBRARY_BASED_TLS_ONLY$NOSSP" \
  "--with-extra-cxxflags=-D_ALLBSD_SOURCE -DUSE_LIBRARY_BASED_TLS_ONLY$NOSSP" \
  "--with-extra-ldflags=-Wl,--no-pie -Wl,--undefined-version" \
  CC=$S/bin/prospero-clang CXX=$S/bin/prospero-clang++ BUILD_CC=/usr/bin/clang-18 BUILD_CXX=/usr/bin/clang++-18 \
  2>&1 | tee $HOME/jdk_configure$SUF.log | grep --line-buffered -E "^(checking (for|if) (cups|fontconfig|compress|zlib|Build|the Boot)|configure:|Using|\*\*\*|Configuration summary)"
[ ${PIPESTATUS[0]} -eq 0 ] || { echo "configure FAILED:"; tail -20 $HOME/jdk_configure$SUF.log; exit 1; }
# JDK native libs need a few headers/defines the PS5 SDK does not pull in implicitly
sed -i '/^CFLAGS_JDKLIB *:=/ { /-D_WITH_GETLINE/! s|$| -include sys/types.h -include sys/socket.h -include netinet/in.h -include arpa/inet.h -include unistd.h -include stdio.h -D_WITH_GETLINE -DIPV6_ADD_MEMBERSHIP=IPV6_JOIN_GROUP -DIPV6_DROP_MEMBERSHIP=IPV6_LEAVE_GROUP| }' spec.gmk

# The SDK's prospero-clang -E emits a binary, so JDK source generation needs a plain preprocessor.
CPPX="/usr/lib/llvm-18/bin/clang -E -target x86_64-sie-ps5 -isysroot $S -isystem $S/target/include -isystem $S/target/include_common"

echo "== HotSpot objects"
make hotspot JOBS=$(nproc) -k 2>&1 | tee $HOME/hotspot$SUF.log | grep --line-buffered -E "^(Building target|Compiling|Creating|Linking|Finished)" || true
OBJS=$B/hotspot/variant-$JVMVAR/libjvm/objs
[ -f $OBJS/ps5_mem.o ] || { echo "HotSpot did not compile (see ~/hotspot.log)"; grep 'error:' $HOME/hotspot$SUF.log | head; exit 1; }

echo "== java.base classes + native libs (libjli errors are expected and ignored)"
make java.base-libs JOBS=$(nproc) -k CPP="$CPPX" 2>&1 | tee $HOME/javabase$SUF.log | grep --line-buffered -E "^(Building target|Compiling|Creating|Linking|Finished)" || true
make java.base-static-libs JOBS=$(nproc) -k CPP="$CPPX" 2>&1 | tee $HOME/javabase_static$SUF.log | grep --line-buffered -E "^(Building target|Compiling|Creating|Linking|Finished)" || true
N=$B/support/native/java.base
for l in $LIBS; do
  [ -f $N/lib$l/static/lib$l.a ] || { echo "missing static lib$l.a (see ~/javabase_static.log)"; grep 'error:' $HOME/javabase_static$SUF.log | grep -v libjli | head; exit 1; }
done
[ -f $B/jdk/modules/java.base/java/lang/Object.class ] || { echo "java.base classes missing (see ~/javabase.log)"; exit 1; }

echo "== extra modules (pure Java: logging, xml, naming, management, sql, ...)"
MODS="java.base java.logging java.naming java.xml java.management java.prefs java.sql java.transaction.xa jdk.unsupported jdk.crypto.ec java.security.sasl jdk.charsets jdk.zipfs java.compiler java.scripting java.net.http"   # java.net.http: Legacy4J (skin cloud sync, template downloads) uses java.net.http.HttpClient; without the module entering a world ends in "NoClassDefFoundError java/net/http/HttpClient" (crash report) and the launcher.   # java.scripting: javax.script, which Minecraft 1.16.5 (log4j 2.8) needs to load its main class. java.compiler: javax.annotation.processing/javax.lang.model/javax.tools, which Mixin (Fabric/Quilt) references
EXTRA_MAKE=""
# jdk.management (JDK 21 only): com.sun.management.HotSpotDiagnosticMXBean (JVM flags such as UseFMA, which Iris asks for in a static initialiser) lives there; its natives come from build_mgmt.sh (libmanagement_ext.a)
if [ "$JDKV" != 17 ]; then MODS="$MODS jdk.management"; EXTRA_MAKE="jdk.management-java"; fi
make java.logging-java java.naming-java java.xml-java java.management-java java.prefs-java java.sql-java java.transaction.xa-java jdk.unsupported-java jdk.crypto.ec-java java.security.sasl-java jdk.charsets-java jdk.zipfs-java java.compiler-java java.scripting-java java.net.http-java $EXTRA_MAKE \
  JOBS=$(nproc) -k CPP="$CPPX" > $HOME/javamods$SUF.log 2>&1 || true
# java.scripting is plain Java without generated sources: if make did not produce it, compile it directly (log: ~/javamods_scripting.log)
if [ -z "$(find $B/jdk/modules/java.scripting -name '*.class' 2>/dev/null | head -1)" ]; then
  mkdir -p $B/jdk/modules/java.scripting
  $JDK17/bin/javac -nowarn -d $B/jdk/modules/java.scripting $(find $SRC/src/java.scripting/share/classes -name '*.java') > $HOME/javamods_scripting$SUF.log 2>&1 || { echo "java.scripting: direct javac failed:"; tail -8 $HOME/javamods_scripting$SUF.log; }
fi
# java.net.http only needs java.base: if make did not produce it, compile it directly (log: ~/javamods_nethttp.log)
if [ -z "$(find $B/jdk/modules/java.net.http -name '*.class' 2>/dev/null | head -1)" ]; then
  mkdir -p $B/jdk/modules/java.net.http
  $JDK17/bin/javac -nowarn -d $B/jdk/modules/java.net.http $(find $SRC/src/java.net.http/share/classes -name '*.java') > $HOME/javamods_nethttp$SUF.log 2>&1 || { echo "java.net.http: direct javac failed:"; tail -8 $HOME/javamods_nethttp$SUF.log; }
fi
for m in $MODS; do
  [ -n "$(find $B/jdk/modules/$m -name '*.class' 2>/dev/null | head -1)" ] || { echo "module $m did not compile (see ~/javamods.log)"; grep 'error:' $HOME/javamods$SUF.log | head -5; exit 1; }
done

echo "== runtime image (lib/modules)"
rm -rf /tmp/ps5jm $OUT/jdk; mkdir -p /tmp/ps5jm $OUT
for m in $MODS; do
  $JDK17/bin/jmod create --class-path $B/jdk/modules/$m --module-version $MODVER --target-platform $TARGETPLAT /tmp/ps5jm/$m.jmod
done
$JDK17/bin/jlink --module-path /tmp/ps5jm --add-modules $(echo $MODS | tr ' ' ',') --output $OUT/jdk --strip-debug --no-header-files --no-man-pages --compress=$COMPRESS

echo "== collect outputs"
rm -f $OUT/libjvm.a $OUT/whole-hotspot.a
"$S/bin/llvm-ar" rcs $OUT/whole-hotspot.a $(find $OBJS -name '*.o')
for l in $LIBS; do cp $N/lib$l/static/lib$l.a $OUT/; done
# runtime files the class library reads from java.home (jlink only wrote lib/modules)
cp -rL $JDK17/conf $OUT/jdk/conf
mkdir -p $OUT/jdk/lib
cp -L $JDK17/lib/tzdb.dat $OUT/jdk/lib/ 2>/dev/null || true
cp -rL $JDK17/lib/security $OUT/jdk/lib/security 2>/dev/null || true
ls -la $OUT/whole-hotspot.a $OUT/jdk/lib/modules 2>&1 | awk '{print $5, $9}'
echo "== done. Expect whole-hotspot.a (~38 MB) and lib/modules (~9 MB) above, and no 'error' lines."
echo "   Next: bash make_jvm_title.sh <PC_IP> <PS5_IP>"
