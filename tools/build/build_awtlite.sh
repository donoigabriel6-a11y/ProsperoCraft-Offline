#!/bin/bash
# build_awtlite.sh <jdk source tree> <javac> <desktop_lite dir>: AWT-lite + javax.script on top of the pure-Java java.desktop classes already compiled into <desktop_lite dir> (make_jvm_title.sh).
#   * javax.script (java.scripting module; log4j's ScriptManager needs it, 1.12.2 failed with NoClassDefFoundError javax/script/ScriptEngineManager): the JDK's own sources, compiled as they are.
#   * java.datatransfer (Transferable, Clipboard, DataFlavor, StringSelection ...; its own JDK module, not part of the java.desktop closure: Minecraft <= 1.12.2 asks for the clipboard in Minecraft.init and failed with NoClassDefFoundError java/awt/datatransfer/Transferable): the JDK's own sources, compiled as they are.
#   * awtlite.src (a bundle of Java sources, "//// FILE: path" separators, or the awtlite/ tree): BufferedImage / Image / Graphics2D in software, a PNG ImageIO, the headless PlatformGraphicsInfo.
#     The real BufferedImage needs the ICC / LittleCMS natives of java.desktop, the real ImageIO needs sun.awt.AppContext: neither exists on the console. Minecraft <= 1.12.2 reads every texture through ImageIO + BufferedImage.
#   The natives java.desktop classes request (every initIDs, JNI_OnLoad_awt) are no-ops in ps5awt.c, linked into the title by build_lwjgl.sh.
#   Java 8 mode:  build_awtlite.sh 8 <Java 8 rt.jar> <out.jar> <javac 8>   -> the AWT-lite classes only (Java 8's rt.jar already has java.beans, javax.script and java.awt.datatransfer, and its Toolkit is headless via
#   -Dawt.toolkit=sun.awt.HToolkit -Djava.awt.graphicsenv=sun.awt.PS5GraphicsEnvironment -Djava.awt.headless=true); the jar goes in FRONT of rt.jar (-Xbootclasspath/p) so BufferedImage / Image / ImageIO replace the real ones.
set -e
if [ "$1" = 8 ]; then
  RT=$2; OUTJAR=$3; JAVAC=${4:-/usr/lib/jvm/java-8-openjdk-amd64/bin/javac}; HERE=$(cd "$(dirname "$0")" && pwd)
  [ -f "$RT" ] || { echo "awtlite 8: missing $RT"; exit 1; }
  W=$(mktemp -d); trap 'rm -rf $W' EXIT
  mkdir -p $W/awt $W/out
  for SRCF in $HERE/awtlite.src $HERE/awtlite8.src; do   # awtlite8.src: Java 8 only classes (a Frame that works without a window system); Java 17+ keep the real java.awt.Frame
  [ -f $SRCF ] || continue
  python3 -I - $SRCF $W/awt <<'PY'
import os, sys
cur = None; buf = []
def flush():
    if cur is not None:
        p = os.path.join(sys.argv[2], cur); os.makedirs(os.path.dirname(p), exist_ok=True); open(p, "w").write("".join(buf))
for ln in open(sys.argv[1]):
    if ln.startswith("//// FILE: "): flush(); cur = ln[len("//// FILE: "):].strip(); buf = []
    else: buf.append(ln)
flush()
PY
  done
  rm -f $W/awt/sun/awt/PS5Toolkit.java $W/awt/sun/awt/PlatformGraphicsInfo.java $W/awt/sun/java2d/Disposer.java   # Java 9+ plumbing (Java 8 has sun.awt.HToolkit and the java.awt.graphicsenv property instead); Disposer: Java 8 still has GetPropertyAction, its own Disposer stays
  $JAVAC -nowarn -source 8 -target 8 -bootclasspath $RT -d $W/out $(find $W/awt -name '*.java') 2>&1 | grep -v -E '^Note|JAVA_TOOL|warning' || true
  [ -s $W/out/java/awt/image/BufferedImage.class ] && [ -s $W/out/javax/imageio/ImageIO.class ] && [ -s $W/out/sun/awt/PS5GraphicsEnvironment.class ] && [ -s $W/out/java/awt/Frame.class ] || { echo "awtlite 8: did not compile"; exit 1; }
  python3 -I - $W/out $OUTJAR <<'PY'
import os, sys, zipfile
root, out = sys.argv[1:3]; n = 0
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for r, _, fs in os.walk(root):
        for f in sorted(fs): z.write(os.path.join(r, f), os.path.relpath(os.path.join(r, f), root)); n += 1
print("awtlite 8: %d classes -> %s" % (n, out))
PY
  exit 0
fi
SRC=$1; JAVAC=$2; DL=$3; HERE=$(cd "$(dirname "$0")" && pwd)
[ -d "$SRC/src/java.datatransfer/share/classes/java/awt/datatransfer" ] || { echo "awtlite: missing $SRC/src/java.datatransfer/share/classes/java/awt/datatransfer"; exit 1; }
[ -d "$SRC/src/java.scripting/share/classes/javax/script" ] || { echo "awtlite: missing $SRC/src/java.scripting/share/classes/javax/script"; exit 1; }
W=$(mktemp -d); trap 'rm -rf $W' EXIT
mkdir -p $W/awt $W/out $W/outs $W/outd
$JAVAC --patch-module java.scripting=$SRC/src/java.scripting/share/classes -d $W/outs $(ls $SRC/src/java.scripting/share/classes/javax/script/*.java) 2>&1 | grep -v -E '^Note|JAVA_TOOL' || true
# DataFlavor calls sun.reflect.misc.ReflectUtil.checkPackageAccess (a security-manager check): java.base exports that package only to named JDK modules, not to boot-classpath code (IllegalAccessError at run time) -> the call is dropped
mkdir -p $W/dt && cp -r $SRC/src/java.datatransfer/share/classes/. $W/dt/
sed -i -e '/^import sun\.reflect\.misc\.ReflectUtil;/d' -e 's/ReflectUtil\.checkPackageAccess([^;]*);//' $W/dt/java/awt/datatransfer/DataFlavor.java
! grep -rq 'sun\.reflect\.misc\|ReflectUtil' $W/dt --include=*.java || { echo "awtlite: java.datatransfer still uses sun.reflect.misc"; exit 1; }
$JAVAC --patch-module java.datatransfer=$W/dt -d $W/outd $(find $W/dt -name '*.java' ! -name module-info.java ! -name package-info.java) 2>&1 | grep -v -E '^Note|JAVA_TOOL' || true
if [ -f $HERE/awtlite.src ]; then
  python3 -I - $HERE/awtlite.src $W/awt <<'PY'
import os, sys
cur = None; buf = []
def flush():
    if cur is not None:
        p = os.path.join(sys.argv[2], cur); os.makedirs(os.path.dirname(p), exist_ok=True); open(p, "w").write("".join(buf))
for ln in open(sys.argv[1]):
    if ln.startswith("//// FILE: "): flush(); cur = ln[len("//// FILE: "):].strip(); buf = []
    else: buf.append(ln)
flush()
PY
elif [ -d $HERE/awtlite ]; then cp -r $HERE/awtlite/. $W/awt/
else echo "awtlite: neither $HERE/awtlite.src nor $HERE/awtlite/ found"; exit 1; fi
$JAVAC --patch-module java.desktop=$W/awt -d $W/out $(find $W/awt -name '*.java') 2>&1 | grep -v -E '^Note|JAVA_TOOL' || true
[ -s $W/outd/java/awt/datatransfer/Transferable.class ] && [ -s $W/outd/sun/datatransfer/DataFlavorUtil.class ] && [ -s $W/out/java/awt/image/BufferedImage.class ] && [ -s $W/outs/javax/script/ScriptEngineManager.class ] && [ -s $W/out/javax/imageio/ImageIO.class ] || { echo "awtlite: did not compile"; exit 1; }
# only our classes + javax.script go into the boot jar tree (the compile pulled other java.desktop classes in implicitly: those are already there from make_jvm_title.sh)
python3 -I - $W/out $W/awt $DL $W/outs $W/outd <<'PY'
import os, shutil, sys
out, awt, dl, outs, outd = sys.argv[1:6]; n = 0
names = set()
for r, _, fs in os.walk(awt):
    for f in fs:
        if f.endswith(".java"): names.add(os.path.relpath(os.path.join(r, f), awt)[:-5])
for r, _, fs in os.walk(out):
    for f in fs:
        if not f.endswith(".class"): continue
        rel = os.path.relpath(os.path.join(r, f), out)
        top = rel[:-6].split("$")[0]
        if top in names:
            os.makedirs(os.path.dirname(os.path.join(dl, rel)), exist_ok=True); shutil.copy(os.path.join(r, f), os.path.join(dl, rel)); n += 1
for base in (outs, outd):
  for r, _, fs in os.walk(base):
    for f in fs:
        if f.endswith(".class"):
            rel = os.path.relpath(os.path.join(r, f), base); os.makedirs(os.path.dirname(os.path.join(dl, rel)), exist_ok=True); shutil.copy(os.path.join(r, f), os.path.join(dl, rel)); n += 1
print("awtlite: %d classes (javax.script + java.datatransfer + AWT-lite) added to %s" % (n, dl))
PY
