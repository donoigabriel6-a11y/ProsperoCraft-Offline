#!/bin/bash
# awt_test.sh <workdir>: AWT-lite + javax.script on a JVM image WITHOUT java.desktop / java.scripting (like the console's), compared with the real JDK's AWT.
# Builds: a jlink'd minimal image, desktop-lite (java.beans closure + AWT-lite + javax.script, as make_jvm_title.sh does), libawt.so from ps5awt.c. Prints "fails=N".
# Needs a JDK 17+ with jmods (jlink) and the matching JDK sources (JDKSRC=<tree with src/java.desktop>, default ~/work/jdk21u or ~/jdk21u or ~/jdk17u); otherwise "SKIP".
T=$(cd "$(dirname "$0")" && pwd); P=$T/..; W=$1; mkdir -p "$W"; export JAVA_TOOL_OPTIONS=
JH=$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")"); JV=$("$JH/bin/javac" -version 2>&1 | grep -o '[0-9]*' | head -1)
SRC=${JDKSRC:-}; for c in $HOME/jdk${JV}u $HOME/work/jdk${JV}u /home/claude/work/jdk${JV}u $HOME/jdk17u; do [ -z "$SRC" ] && [ -d $c/src/java.desktop/share/classes/java/beans ] && SRC=$c; done
[ -n "$SRC" ] && [ -d "$SRC/src/java.scripting" ] && [ -d "$JH/jmods" ] || { echo "SKIP: no JDK sources / jmods for a JDK $JV (fails=0)"; exit 0; }
fails=0; bad() { fails=$((fails+1)); echo "   FAIL: $*"; }
rm -rf $W/img $W/dl $W/ref $W/mine; mkdir -p $W/dl $W/ref $W/mine $W/cls $W/lib
"$JH/bin/jlink" --module-path "$JH/jmods" --add-modules java.base,java.logging,jdk.unsupported --output $W/img 2>&1 | grep -v Picked
SRCD=$SRC/src/java.desktop/share/classes; DLS=""
for f in PropertyChangeEvent PropertyChangeListener PropertyChangeListenerProxy PropertyChangeSupport IndexedPropertyChangeEvent ChangeListenerMap VetoableChangeListener VetoableChangeListenerProxy VetoableChangeSupport PropertyVetoException ConstructorProperties Transient JavaBean BeanProperty; do DLS="$DLS $SRCD/java/beans/$f.java"; done
"$JH/bin/javac" --patch-module java.desktop=$SRCD -d $W/dl $DLS 2>&1 | grep -v -E "^Note|Picked"
# the same ImageIO / PlatformGraphicsInfo situation as make_jvm_title.sh: the real ImageIO class is in the closure; awtlite replaces it
bash $P/build_awtlite.sh $SRC "$JH/bin/javac" $W/dl > $W/awtlite.log 2>&1 || { cat $W/awtlite.log; echo "fails=1"; exit 1; }
tail -1 $W/awtlite.log
( cd $W/dl && "$JH/bin/jar" cf $W/desktop-lite.jar . )
gcc -shared -fPIC -w -I$JH/include -I$JH/include/linux -o $W/lib/libawt.so $P/ps5awt.c || { echo "FAIL: ps5awt.c"; echo "fails=1"; exit 1; }
cp $W/lib/libawt.so $W/img/lib/   # boot-loader classes look in the image's lib dir (on the console the library is linked in statically)
python3 $T/awt_fixtures.py $W/fx
"$JH/bin/javac" --release 17 -nowarn -d $W/cls $T/AwtTest.java 2>&1 | grep -v Picked
"$JH/bin/java" -cp $W/cls AwtTest $W/fx $W/ref > $W/ref.txt 2>&1 || { echo "reference run failed"; tail -5 $W/ref.txt; }
$W/img/bin/java -Xbootclasspath/a:$W/desktop-lite.jar -Djava.library.path=$W/lib -cp $W/cls AwtTest $W/fx $W/mine > $W/mine.txt 2>&1 || { bad "run on the minimal image"; tail -12 $W/mine.txt; }
grep -q '^done$' $W/mine.txt || { bad "test did not finish"; tail -12 $W/mine.txt; }
python3 -I - $W/fx $W/ref.txt $W/mine.txt <<'PY' || fails=$((fails+1))
import sys, os, zlib, struct
fx, ref, mine = sys.argv[1:4]
def crc(vals):
    c = 0
    for v in vals: c = zlib.crc32(struct.pack(">I", v), c)
    return "%x" % (c & 0xffffffff)
R = {l.split(" ", 1)[0] + (" " + l.split(" ")[1] if l.startswith("png") else ""): l.rstrip("\n") for l in open(ref)}
M = {l.split(" ", 1)[0] + (" " + l.split(" ")[1] if l.startswith("png") else ""): l.rstrip("\n") for l in open(mine)}
bad = 0; n = 0
for f in sorted(os.listdir(fx)):
    if not f.endswith(".png"): continue
    exp = [int(x, 16) for x in open(os.path.join(fx, f[:-4] + ".expect")).read().split()]
    line = M.get("png " + f, "")
    want_hash = crc(exp); n += 1
    if not line.endswith(" " + want_hash): bad += 1; print("   FAIL: %s: got '%s', expected hash %s" % (f, line, want_hash))
for k in ("skin32", "skin64", "scale", "sub", "raster", "written", "color", "script", "clip"):
    n += 1
    if R.get(k) != M.get(k): bad += 1; print("   FAIL: %s differs from the real AWT:\n     real: %s\n     mine: %s" % (k, R.get(k), M.get(k)))
n += 1
a = [int(x, 16) for x in R["blend"].split()[1:]]; b = [int(x, 16) for x in M["blend"].split()[1:]]
# the real AWT quantises through premultiplied 8-bit values: a colour channel of a pixel with alpha A can differ by about 256/A (that is not a defect); opaque results must agree to 1
d = 0; badpx = 0
if len(a) == len(b) == 256:
    for x, y in zip(a, b):
        al = max(x >> 24, 1); allow = 1 + 256 // al
        for s in (0, 8, 16): 
            df = abs(((x >> s) & 255) - ((y >> s) & 255)); d = max(d, df)
            if df > allow: badpx += 1
        if (x >> 24) != (y >> 24) and abs((x >> 24) - (y >> 24)) > 1: badpx += 1
else: badpx = 1
if badpx: bad += 1; print("   FAIL: blend differs from the real AWT beyond quantisation (%d channels)" % badpx)
print("   %d comparisons, blend max diff %d" % (n, d)); print("fails=%d" % bad); sys.exit(1 if bad else 0)
PY
# what we wrote, read by the real ImageIO
cat > $W/Rd.java <<'JEOF'
import javax.imageio.ImageIO; import java.io.File; import java.awt.image.BufferedImage; import java.util.zip.CRC32;
public class Rd { public static void main(String[] a) throws Exception { BufferedImage im = ImageIO.read(new File(a[0])); CRC32 c = new CRC32(); for (int y = 0; y < im.getHeight(); y++) for (int x = 0; x < im.getWidth(); x++) { int v = im.getRGB(x, y); c.update(v >>> 24); c.update(v >>> 16); c.update(v >>> 8); c.update(v); } System.out.println(Long.toHexString(c.getValue())); } }
JEOF
"$JH/bin/javac" -d $W/cls $W/Rd.java 2>&1 | grep -v Picked
got=$("$JH/bin/java" -cp $W/cls Rd $W/mine/written.png); want=$(grep '^written' $W/mine.txt | cut -d' ' -f2)
[ "$got" = "$want" ] && echo "   PNG written by AWT-lite read back by the real ImageIO: same pixels" || bad "written PNG: real ImageIO reads $got, expected $want"
echo "fails=$fails"; [ $fails = 0 ]
