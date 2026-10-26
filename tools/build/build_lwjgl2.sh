#!/bin/bash
# build_lwjgl2.sh: LWJGL 2 for Minecraft 1.12.2 and older. Builds ~/lwjgl2_build/lwjgl2_ps5.jar (Java) and ~/lwjgl2_build/obj/*.o (natives).
#   * LWJGL 2.9.3 (BSD licence) is cloned from GitHub (tag lwjgl2.9.3) into ~/lwjgl2: its generator (a javac annotation processor) writes the OpenGL / OpenAL Java classes and their JNI stubs
#   * the X11 / Windows / macOS window layer is replaced by l2src/PS5Display.java & co. (console window, pad-> keyboard/mouse and EGL context come from the GLFW shim of ps5lwjgl.c);
#     AWT is cut out (the console's Java has none); a handful of source patches below, each asserted: the build stops if LWJGL's sources are not what it expects
#   * natives: the generated stubs + LWJGL's common_tools.c / extal.c / ALC*.c + ps5lwjgl2.c, every global symbol renamed l2_*: LWJGL 2 and LWJGL 3 share JNI names, so
#     JNI_OnLoad_lwjgl2ps5 binds the LWJGL 2 ones with RegisterNatives from a generated table (l2_natives.c)
# build_lwjgl.sh calls this (LWJGL2=0 skips it) and links the objects into liblwjgl_ps5.a; make_jvm_title.sh puts the jar into the title (assets/lib2).
# needs: a JDK 9+ (javac --release 8, javap, jar), git, python3.  Sandbox test (no console): HOME=/some/dir L2CC=gcc L2OBJCOPY=objcopy L2NM=nm L2AR=ar L2JNI=/usr/lib/jvm/java-21-openjdk-amd64/include bash build_lwjgl2.sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
T=${T:-$HOME/ps5probe}; L=${LWJGL2_DIR:-$HOME/lwjgl2}; TAG=${LWJGL2_TAG:-lwjgl2.9.3}; B=${LWJGL2_BUILD:-$HOME/lwjgl2_build}
PSDK=$T/.deps/native/ps5-payload-sdk
if [ -n "$L2CC" ]; then CC="$L2CC"; OBJCOPY=${L2OBJCOPY:-objcopy}; NM=${L2NM:-nm}
  JNIFL="-I${L2JNI:-/usr/lib/jvm/default-java/include} -I${L2JNI:-/usr/lib/jvm/default-java/include}/linux"
else
  CCX=; [ "$NOCCACHE" != 1 ] && command -v ccache >/dev/null && CCX="USE_CCACHE=1"   # unchanged sources compile in milliseconds
  CC="env PS5_PAYLOAD_SDK=$PSDK PS5_CLANG=clang-18 $CCX sh $T/tooling/prospero-clang18"
  OBJCOPY=$(ls $PSDK/bin/llvm-objcopy 2>/dev/null || command -v llvm-objcopy-18 || command -v llvm-objcopy || command -v objcopy)
  NM=$(ls $PSDK/bin/llvm-nm 2>/dev/null || command -v llvm-nm-18 || command -v llvm-nm || command -v nm)
  JNIINC=${JNIINC:-$HOME/jdk17u/src/java.base}; JNIFL="-I$JNIINC/share/native/include -I$JNIINC/unix/native/include"
fi
[ -d /usr/lib/jvm/java-17-openjdk-amd64/bin ] && ! command -v javac >/dev/null && PATH=$PATH:/usr/lib/jvm/java-17-openjdk-amd64/bin
command -v javac >/dev/null && command -v javap >/dev/null && command -v jar >/dev/null || { echo "lwjgl2: javac / javap / jar not found (install a JDK 17 or newer)"; exit 1; }
JAVAC="javac --release 8 -nowarn -Xlint:none"
[ -f $HERE/l2src/PS5Display.java ] || { echo "lwjgl2: $HERE/l2src/*.java missing: unzip l2src.zip into $HERE"; exit 1; }
[ -d $L/src/java ] || git clone -q --depth 1 --branch $TAG https://github.com/LWJGL/lwjgl $L
# nothing changed since the last successful build -> keep the jar and the objects (FORCE=1 rebuilds). The key covers this script, l2src/*, ps5lwjgl2.c, the LWJGL 2 checkout, the compilers and the JNI include paths.
KEY=$( { cat $0 $HERE/ps5lwjgl2.c $HERE/l2src/*.java; echo "$CC|$(for d in ${JNIFL//-I/ }; do cat $d/jni.h $d/jni_md.h 2>/dev/null; done | md5sum)|$(git -C $L rev-parse HEAD 2>/dev/null)|$(javac -version 2>&1)|$(${L2CC:-clang-18} --version 2>&1 | head -1)"; } | md5sum | cut -c1-32)
if [ -z "$FORCE" ] && [ -s $B/lwjgl2_ps5.jar ] && [ -s $B/obj/l2_natives.o ] && [ "$(cat $B/.key 2>/dev/null)" = "$KEY" ]; then
  echo "lwjgl2: unchanged since the last build, skipped (jar + $(ls $B/obj/*.o | wc -l) objects reused; FORCE=1 rebuilds)"; exit 0
fi
rm -rf $B; mkdir -p $B/gensrc $B/gen $B/genjava $B/genc/openal $B/genc/opengl $B/jsrc $B/classes $B/hdr $B/obj
S=$L/src/java

# ---- 1. the generator: compiled from LWJGL's sources; one patch (it asks javac's Filer to create sources whose names equal the template files', which newer javac refuses) ----
cp -r $S/org $B/gensrc/ 2>/dev/null; rm -rf $B/gensrc; mkdir -p $B/gensrc/org/lwjgl/util; cp -r $S/org/lwjgl/util/generator $B/gensrc/org/lwjgl/util/
python3 -I - $B/gensrc/org/lwjgl/util/generator/GeneratorVisitor.java <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
a = 'java_writer = new PrintWriter(env.getFiler().createSourceFile(Utils.getQualifiedClassName(e), env.getElementUtils().getPackageOf(e)).openWriter());'
assert a in s, "GeneratorVisitor: createSourceFile line not found"
b = '{ java.io.File jf = new java.io.File(System.getProperty("gen.out") + "/" + Utils.getQualifiedClassName(e).replace(\'.\', \'/\') + ".java"); jf.getParentFile().mkdirs(); java_writer = new PrintWriter(new java.io.FileWriter(jf)); }'
open(p, "w").write(s.replace(a, b))
PY
GENSRC=$(find $B/gensrc/org/lwjgl/util/generator -name '*.java' | grep -v -E '/(opengles|opencl)/')
$JAVAC -proc:none -sourcepath $B/gensrc:$S -d $B/gen $GENSRC $S/org/lwjgl/PointerWrapper.java $S/org/lwjgl/PointerBuffer.java $S/org/lwjgl/opengl/GLSync.java \
  $S/org/lwjgl/opengl/AMDDebugOutputCallback.java $S/org/lwjgl/opengl/ARBDebugOutputCallback.java $S/org/lwjgl/opengl/KHRDebugCallback.java 2>&1 | grep -v -E '^Note|JAVA_TOOL' || true
$JAVAC -proc:none -sourcepath $S -cp $B/gen -d $B/gen $S/org/lwjgl/opencl/CLPlatform.java $S/org/lwjgl/opencl/CLDevice.java $S/org/lwjgl/opencl/CLContext.java $S/org/lwjgl/opencl/CLCommandQueue.java \
  $S/org/lwjgl/opencl/CLMem.java $S/org/lwjgl/opencl/CL*Callback.java $S/org/lwjgl/opencl/CLNativeKernel.java $S/org/lwjgl/opencl/CLFunctionAddress.java 2>&1 | grep -v -E '^Note|JAVA_TOOL' || true
[ -s $B/gen/org/lwjgl/util/generator/GeneratorProcessor.class ] || { echo "lwjgl2: the generator did not compile"; exit 1; }
gen() {   # gen <template dir> <file patterns> <processor> [javac options]
  local dir=$1 pat=$2 proc=$3; shift 3
  local files=$(cd $dir && for p in $(echo $pat | tr ',' ' '); do ls $p 2>/dev/null; done | sort -u | sed "s|^|$dir/|")
  javac --release 8 -nowarn -Xlint:none -J-Dgen.out=$B/genjava -proc:only -processor $proc -cp "$S:$L/src/templates:$B/gen" -s $B/genjava "$@" $files 2>&1 | grep -v -E '^Note|JAVA_TOOL' || true
}
TG=$L/src/templates/org/lwjgl
GLPAT="GL*.java,ARB*.java,EXT*.java,KHR*.java,AMD*.java,APPLE*.java,ATI*.java,NV*.java,NVX*.java,HP*.java,IBM*.java,SUN*.java,SGIS*.java,GREMEDY*.java,INTEL*.java"
gen $TG/openal "AL*.java,EFX*.java" org.lwjgl.util.generator.GeneratorProcessor -d $B/genc/openal -Abinpath=$B/gen -Atypemap=org.lwjgl.util.generator.openal.ALTypeMap
gen $TG/opengl "$GLPAT" org.lwjgl.util.generator.GeneratorProcessor -d $B/genc/opengl -Abinpath=$B/gen -Atypemap=org.lwjgl.util.generator.opengl.GLTypeMap -Acontextspecific
gen $TG/opengl "$GLPAT" org.lwjgl.util.generator.opengl.GLGeneratorProcessor -Acontextspecific
gen $TG/opengl "$GLPAT" org.lwjgl.util.generator.opengl.GLReferencesGeneratorProcessor
NJ=$(find $B/genjava -name '*.java' | wc -l); NC=$(find $B/genc -name '*.c' | wc -l)
[ $NJ -gt 300 ] && [ $NC -gt 150 ] || { echo "lwjgl2: the generator produced only $NJ Java / $NC C files (expected ~330 / ~160)"; exit 1; }
echo "lwjgl2: generator: $NJ Java classes, $NC C files"

# ---- 2. the Java tree: LWJGL's sources minus the platform / AWT / OpenCL / test parts, plus the generated classes and l2src/*.java ----
python3 -I - $S $B/jsrc $B/genjava $HERE/l2src <<'PY'
import os, re, shutil, sys
src, dst, gen, ours = sys.argv[1:5]
skip_dirs = {"examples", "test", "opencl", "opengles", "applet", "generator", "jinput", "mapped"}
skip_files = re.compile(r"^(Linux|Windows|MacOSX|AWT)|^(KeyboardEventQueue|MouseEventQueue|XRandR|NVPresentVideoUtil|NVVideoCaptureUtil|Controllers|JInputController|WaveData|ContextGLES|DrawableGLES|ARBCLEvent|MacOSXSysImplementation|WindowsSysImplementation)\.java$")
n = 0
for root, dirs, files in os.walk(os.path.join(src, "org", "lwjgl")):
    dirs[:] = [d for d in dirs if d not in skip_dirs]
    rel = os.path.relpath(root, src)
    for f in files:
        if not f.endswith(".java") or skip_files.search(f): continue
        os.makedirs(os.path.join(dst, rel), exist_ok=True); shutil.copy(os.path.join(root, f), os.path.join(dst, rel, f)); n += 1
for root, dirs, files in os.walk(gen):
    rel = os.path.relpath(root, gen)
    for f in files:
        if f.endswith(".java") and f != "ARBCLEvent.java": os.makedirs(os.path.join(dst, rel), exist_ok=True); shutil.copy(os.path.join(root, f), os.path.join(dst, rel, f)); n += 1
for f in os.listdir(ours):
    if not f.endswith(".java"): continue
    pkg = "org/lwjgl" if f == "LinuxSysImplementation.java" else "org/lwjgl/input" if f == "Controllers.java" else "org/lwjgl/opengl"
    os.makedirs(os.path.join(dst, pkg), exist_ok=True); shutil.copy(os.path.join(ours, f), os.path.join(dst, pkg, f)); n += 1
print("lwjgl2: %d Java files in the build tree" % n)
PY
python3 -I - $B/jsrc <<'PY'
import re, sys
root = sys.argv[1]
def edit(path, fn, what):
    p = root + "/" + path; s = open(p).read(); t = fn(s)
    assert t != s, "lwjgl2 source patch did not apply: " + what
    open(p, "w").write(t)
def drop_awt(s):
    s = re.sub(r"^import java\.awt\.[^\n]*\n", "", s, flags=re.M)
    return re.sub(r"\bCanvas\b", "PS5Parent", s)
def display(s):
    s = drop_awt(s)
    s = re.sub(r"\tprivate static final ComponentListener component_listener = new ComponentAdapter\(\) \{.*?\n\t\};\n", "", s, count=1, flags=re.S)
    s = re.sub(r"[ \t]*(tmp_parent|parent)\.(add|remove)ComponentListener\(component_listener\);\n", "", s)
    s = s.replace("return new LinuxDisplay();", "return new PS5Display();")
    return re.sub(r"return new (WindowsDisplay|MacOSXDisplay)\(\);", 'throw new IllegalStateException("Unsupported platform");', s)
def cut_method(s, start_marker, with_javadoc=True):
    i = s.index(start_marker); j = s.index("{", i); d = 0; k = j
    while True:
        if s[k] == "{": d += 1
        elif s[k] == "}":
            d -= 1
            if d == 0: break
        k += 1
    if with_javadoc: i = s.rindex("\t/**", 0, i)
    return s[:i] + s[k + 1:].lstrip("\n")
def display2(s):
    s = display(s)
    s = s.replace("import org.lwjgl.input.Controllers;\n", "")
    s = s.replace("\t\tif ( Controllers.isCreated() ) {\n\t\t\tControllers.poll();\n\t\t}\n", "")
    s = cut_method(s, "public static void create(PixelFormatLWJGL pixel_format) throws")
    s = cut_method(s, "public static void create(PixelFormatLWJGL pixel_format, Drawable shared_drawable) throws")
    s = cut_method(s, "public static void create(PixelFormatLWJGL pixel_format, org.lwjgl.opengles.ContextAttribs attribs)")
    s = cut_method(s, "public static void create(PixelFormatLWJGL pixel_format, Drawable shared_drawable, org.lwjgl.opengles.ContextAttribs attribs)")
    # Vanilla 1.12.x caps menus at 30 fps; run them at 60 (-Dps5.menufps.off=true disables). The frame limiter is our own (LWJGL's Sync spins in Thread.yield loops that
    # steal the cores the GL worker and audio threads need; -Dps5.sync.lwjgl=true brings it back) and every 2 s a "[ps5] frame:" line says where the time went.
    a = "\t\tSync.sync(fps);\n\t}\n"
    assert a in s, "Display.sync body not found"
    members = """\t\tps5Frame(fps == 30 && !Boolean.getBoolean("ps5.menufps.off") ? 60 : fps);
\t}

\tprivate static long ps5Win, ps5Next, ps5LastEnd, ps5SyncNs, ps5UpdNs, ps5PerNs; private static int ps5N;
\tprivate static final boolean PS5_LWJGLSYNC = Boolean.getBoolean("ps5.sync.lwjgl");
\tprivate static void ps5Frame(int fps) {
\t\tlong t0 = System.nanoTime();
\t\tif (fps > 0) {
\t\t\tif (PS5_LWJGLSYNC) Sync.sync(fps);
\t\t\telse {
\t\t\t\tlong period = 1000000000L / fps;
\t\t\t\tif (ps5Next == 0 || t0 - ps5Next > period) ps5Next = t0;   // already late: do not try to catch up
\t\t\t\tps5Next += period;
\t\t\t\tfor (;;) {
\t\t\t\t\tlong left = ps5Next - System.nanoTime();
\t\t\t\t\tif (left <= 0) break;
\t\t\t\t\tif (left > 2000000L) java.util.concurrent.locks.LockSupport.parkNanos(Math.min(left - 1000000L, 4000000L)); else Thread.yield();
\t\t\t\t}
\t\t\t}
\t\t}
\t\tlong t1 = System.nanoTime();
\t\tps5SyncNs += t1 - t0; if (ps5LastEnd != 0) ps5PerNs += t1 - ps5LastEnd; ps5LastEnd = t1;
\t\tif (ps5Win == 0) ps5Win = t1;
\t\tps5N++;
\t\tif (t1 - ps5Win >= 2000000000L) {   // every ~2 s, so a 13 fps stretch shows too
\t\t\tps5Win = t1;
\t\t\tdouble n = ps5N, per = ps5PerNs / 1e6 / n, sy = ps5SyncNs / 1e6 / n, up = ps5UpdNs / 1e6 / n;
\t\t\tSystem.out.println("[ps5] frame: " + String.format("%.1f", per) + " ms = game " + String.format("%.1f", per - sy - up) + " + swap/input " + String.format("%.1f", up) + " + limiter " + String.format("%.1f", sy) + " (" + (PS5_LWJGLSYNC ? "LWJGL Sync" : "ps5 limiter") + ", " + ps5N + " frames, want " + fps + " fps)");
\t\t\tps5N = 0; ps5SyncNs = ps5UpdNs = ps5PerNs = 0;
\t\t}
\t}
"""
    s = s.replace(a, members, 1)
    u = "\tpublic static void update(boolean processMessages) {"
    assert s.count(u) == 1, "Display.update(boolean) not found"
    # Minecraft 1.0 - 1.5 call Display.setParent(java.awt.Canvas) with the Canvas of their Frame (a window-less stub, awtlite8.src); drop_awt turned LWJGL's own parameter into PS5Parent, so the Canvas overload is added: nothing embeds the window
    s = s.replace(u, "\t/** PS5: accepts the Canvas Minecraft 1.0 - 1.5 pass; the window is the console's, nothing is embedded. */\n\tpublic static void setParent(java.awt.Canvas parent) throws LWJGLException {\n\t}\n\n" + u, 1)
    s = s.replace(u, "\tpublic static void update(boolean processMessages) {\n\t\tlong ps5t = System.nanoTime();\n\t\ttry { update0(processMessages); } finally { ps5UpdNs += System.nanoTime() - ps5t; }\n\t}\n\n\tprivate static void update0(boolean processMessages) {", 1)
    return s
edit("org/lwjgl/opengl/Display.java", display2, "Display.java")
def memsun(s):
    s = s.replace("import sun.reflect.FieldAccessor;\n", "")
    return cut_method(s, "private static class AccessorReflectFast", True)
edit("org/lwjgl/MemoryUtilSun.java", memsun, "MemoryUtilSun.java")
edit("org/lwjgl/openal/AL.java", lambda s: s.replace("String[] oalPaths = LWJGLUtil.getLibraryPaths(libname, library_names, AL.class.getClassLoader());", 'String[] oalPaths = new String[]{"ps5-openal"};'), "AL.java")
edit("org/lwjgl/opengl/DrawableGL.java", lambda s: cut_method(s, "public void setCLSharingProperties", False), "DrawableGL.java")
edit("org/lwjgl/opengl/Drawable.java", lambda s: re.sub(r"\tvoid setCLSharingProperties\(PointerBuffer properties\) throws LWJGLException;\n", "", s), "Drawable.java")
edit("org/lwjgl/opengl/DisplayImplementation.java", drop_awt, "DisplayImplementation.java")
def ctx(s):
    s = re.sub(r"^import org\.lwjgl\.opencl\.[^\n]*\n", "", s, flags=re.M)
    s = s.replace("return new LinuxContextImplementation();", "return new PS5ContextImplementation();")
    s = re.sub(r"return new (WindowsContextImplementation|MacOSXContextImplementation)\(\);", 'throw new IllegalStateException("Unsupported platform");', s)
    i = s.index("\tpublic synchronized void setCLSharingProperties"); j = s.index("{", i); d = 0; k = j
    while True:
        if s[k] == "{": d += 1
        elif s[k] == "}":
            d -= 1
            if d == 0: break
        k += 1
    return s[:i] + s[k + 1:]
edit("org/lwjgl/opengl/ContextGL.java", ctx, "ContextGL.java")
def sysj(s):
    s = s.replace('JNI_LIBRARY_NAME = "lwjgl";', 'JNI_LIBRARY_NAME = "lwjgl2ps5";').replace('POSTFIX64BIT = "64";', 'POSTFIX64BIT = "";')
    return re.sub(r"return new (WindowsSysImplementation|MacOSXSysImplementation)\(\);", 'throw new IllegalStateException("Unsupported platform");', s)
edit("org/lwjgl/Sys.java", sysj, "Sys.java")
PY
mkdir -p $B/stub/sun/misc
cat > $B/stub/sun/misc/Unsafe.java <<'EOF'
package sun.misc;
public final class Unsafe { public native long getLong(Object o, long off); public native long objectFieldOffset(java.lang.reflect.Field f); }
EOF
$JAVAC -proc:none -h $B/hdr -d $B/classes $(find $B/jsrc -name '*.java') $B/stub/sun/misc/Unsafe.java 2>&1 | grep -v -E '^Note|JAVA_TOOL' | head -80
rm -rf $B/classes/sun
[ -s $B/classes/org/lwjgl/opengl/Display.class ] && [ -s $B/classes/org/lwjgl/opengl/GL11.class ] || { echo "lwjgl2: the Java tree did not compile"; exit 1; }
( cd $B/classes && jar cf $B/lwjgl2_ps5.jar org )
echo "lwjgl2: $(find $B/classes -name '*.class' | wc -l) classes -> $B/lwjgl2_ps5.jar ($(du -h $B/lwjgl2_ps5.jar | cut -f1))"

# ---- 3. natives ----
N=$L/src/native
FL="-std=gnu11 -O2 -w -fPIC -DLWJGL_LINUX $JNIFL -I$B/hdr -I$N/common -I$N/common/opengl -I$B/genc"
SRCS="$(ls $B/genc/openal/*.c $B/genc/opengl/*.c) $N/common/common_tools.c $N/common/extal.c $N/common/org_lwjgl_BufferUtils.c $N/common/org_lwjgl_openal_AL.c $N/common/org_lwjgl_openal_ALC10.c $N/common/org_lwjgl_openal_ALC11.c $N/common/opengl/org_lwjgl_opengl_CallbackUtil.c $HERE/ps5lwjgl2.c"
n=0
for f in $SRCS; do o=$B/obj/l2_$(basename $f .c).o; $CC $FL -c $f -o $o 2>$o.err || { echo "lwjgl2: FAILED $f"; head -8 $o.err; exit 1; }; n=$((n+1)); done
echo "lwjgl2: $n native files compiled"
# the registration table: every native method of every class, bound to the l2_Java_* symbol the generated / LWJGL C files define
$NM --defined-only -g $B/obj/*.o 2>/dev/null | awk '$2 ~ /^[TDBRCVWS]$/ {print $3}' | sort -u > $B/defined.txt
( cd $B/classes && javap -p -s -cp . $(find org -name '*.class' | sed 's/\.class$//; s|/|.|g' | grep -v '\$') ) > $B/javap.txt 2>/dev/null
python3 -I - $B/javap.txt $B/defined.txt $B/l2_natives.c <<'PY'
import re, sys
javap, defined, out = sys.argv[1:4]
defs = set(open(defined).read().split())
def mangle(s): return s.replace("_", "_1").replace(";", "_2").replace("[", "_3").replace("$", "_00024").replace("/", "_").replace(".", "_")
cls = None; rows = []; pending = None; missing = 0
for ln in open(javap):
    m = re.match(r"^(?:public |protected |private |final |abstract |static )*(?:class|interface) ([\w.$]+)", ln)
    if m: cls = m.group(1); continue
    if " native " in ln and cls:
        mm = re.search(r"([A-Za-z_$][\w$]*)\(", ln); pending = mm.group(1) if mm else None; continue
    if pending and ln.strip().startswith("descriptor:"):
        sig = ln.split(":", 1)[1].strip(); sym = "Java_" + mangle(cls) + "_" + mangle(pending)
        if sym in defs: rows.append((cls.replace(".", "/"), pending, sig, "l2_" + sym))
        else: missing += 1
        pending = None
rows.sort(key=lambda r: (r[0], r[1]))
with open(out, "w") as f:
    f.write("/* generated by build_lwjgl2.sh: native method table of the LWJGL 2 classes */\n#include <stddef.h>\ntypedef struct { const char *cls, *name, *sig; void *fn; } L2Nat;\n")
    for r in rows: f.write("extern char %s[];\n" % r[3])
    f.write("const L2Nat l2_generated_natives[] = {\n")
    for r in rows: f.write('  { "%s", "%s", "%s", (void *)%s },\n' % r)
    f.write("  { 0, 0, 0, 0 } };\nconst int l2_generated_natives_n = %d;\n" % len(rows))
print("lwjgl2: %d native methods bound (%d without a C implementation: Linux / Windows / OpenCL / unsupported-extension stubs)" % (len(rows), missing))
PY
$CC $FL -c $B/l2_natives.c -o $B/obj/l2_natives.o
# rename: everything the LWJGL 2 objects define gets l2_ (LWJGL 3, HotSpot, gl4es and FreeType live in the same archive); the table refers to the renamed names
KEEP="^(JNI_OnLoad_lwjgl2ps5)$"
$NM --defined-only -g $B/obj/*.o 2>/dev/null | awk '$2 ~ /^[TDBRCVWS]$/ {print $3}' | sort -u | grep -v -E "$KEEP" | grep -v '^l2_' | awk '{print $1" l2_"$1}' > $B/rename.txt
for o in $B/obj/*.o; do $OBJCOPY --redefine-syms=$B/rename.txt $o; done
echo "lwjgl2: $(wc -l < $B/rename.txt) global symbols renamed l2_*; objects in $B/obj, jar $B/lwjgl2_ps5.jar"
echo "$KEY" > $B/.key
