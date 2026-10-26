#!/bin/bash
# lw2_test.sh <workdir>: host test of the LWJGL 2 port (Minecraft 1.12.2 and older). Builds LWJGL 2 from source (build_lwjgl2.sh, sandbox mode), gl4es (build_gl4es.sh),
# links them with tests/lw2_shim.c (fake GLFW on a headless EGL core context + the real software OpenAL of ps5al.inc) into liblwjgl2ps5.so and runs tests/lw2/Lw2Drive.java
# on a real JVM the way McBoot starts a 1.12.2 game: child-first loader, org.lwjgl.Sys first. Prints "fails=N". Needs: javac/java 11+, gcc, git (+ network for the first run), libEGL + Mesa.
T=$(cd "$(dirname "$0")" && pwd); P=$T/..; W=$1; mkdir -p "$W"; OH=$HOME; export HOME=$W
[ -d $OH/lwjgl2/src/java ] && export LWJGL2_DIR=${LWJGL2_DIR:-$OH/lwjgl2}   # sources already cloned: no network needed
[ -d $OH/gl4es/src/gl ] && export GL4ES_DIR=${GL4ES_DIR:-$OH/gl4es}
JH=$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")
JI=$JH/include; [ -f $JI/jni.h ] || { echo "SKIP: no JDK headers (fails=0)"; exit 0; }
L2CC=gcc L2OBJCOPY=objcopy L2NM=nm L2JNI=$JI JAVA_TOOL_OPTIONS= bash $P/build_lwjgl2.sh > $W/b2.log 2>&1 || { echo "FAIL: build_lwjgl2.sh"; tail -15 $W/b2.log; echo "fails=1"; exit 1; }
G4CC=gcc G4OBJCOPY=objcopy G4NM=nm G4AR=ar bash $P/build_gl4es.sh > $W/b4.log 2>&1 || { echo "FAIL: build_gl4es.sh"; tail -8 $W/b4.log; echo "fails=1"; exit 1; }
B=$W/lwjgl2_build; G=$W/gl4es_build
mkdir -p $W/lib $W/cls
gcc -w -std=gnu11 -O1 -fPIC -shared -I$JI -I$JI/linux -I$P -o $W/lib/liblwjgl2ps5.so $T/lw2_shim.c $B/obj/*.o -Wl,--whole-archive $G/libgl4es_ps5.a -Wl,--no-whole-archive -lEGL -lm -lpthread -Wl,--allow-multiple-definition 2> $W/link.log || { echo "FAIL: link"; head -8 $W/link.log; echo "fails=1"; exit 1; }
javac --release 8 -nowarn -Xlint:none -cp $B/lwjgl2_ps5.jar -d $W/cls $T/lw2/Lw2Inject.java $T/lw2/Lw2Drive.java $T/Lw2Test.java 2>&1 | grep -v -E "^Note|JAVA_TOOL|warning" 
( cd $W/cls && jar cf $W/lw2test.jar org ) 
JAVA_TOOL_OPTIONS= java -Xshare:auto -Djava.library.path=$W/lib -cp $W/cls Lw2Test $B/lwjgl2_ps5.jar $W/lw2test.jar 2>&1
