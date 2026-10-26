#!/bin/bash
# Runs every sandbox check of the shim and the merge layer (none of this needs the console). Usage: bash tests/run_all.sh   (from anywhere)
# Needs gcc, python3, nm; optional: glslangValidator (apt install glslang-tools) for the shader checks.
cd "$(dirname "$0")"; T=$PWD; INC="-I$T/inc -I$T/.. -DPS5_USB_NO_STRONG"; W="-w -std=gnu11 -O0 -g -no-pie"; OUT=$(mktemp -d); fails=0
LIBS="-lpthread -lm"
# unresolved console symbols (sce*, egl*, __real_*) are stubbed so the shim links on Linux: generated from the object itself
mkstubs() { nm -u "$1" | awk '{print $2}' | grep -E "^(JNI_OnLoad|sce|egl|getdirentries|__real_|__wrap_)" | sort -u | grep -v "^eglGetProcAddress$" $2 | python3 -c "
import sys
print('#include <stddef.h>')
for s in sys.stdin.read().split(): print('long %s(long a,long b,long c,long d,long e,long f){return 0;}'%s)" > "$3"; }
run() { printf "%-46s" "$1"; shift; if out=$("$@" 2>&1); then echo "$out" | grep -E "fails=[0-9]+" | tail -1 | grep -q "fails=0" && echo ok || { echo FAIL; echo "$out" | grep -E "FAIL|fails=" | head -5; fails=$((fails+1)); }; else echo "FAIL (exit)"; echo "$out" | tail -3; fails=$((fails+1)); fi; }
run_noname() { if out=$("$@" 2>&1); then echo "$out" | grep -E "fails=[0-9]+" | tail -1 | grep -q "fails=0" && echo ok || { echo FAIL; echo "$out" | grep -E "FAIL|fails=" | head -5; fails=$((fails+1)); }; else echo "FAIL (exit)"; fails=$((fails+1)); fi; }
# 1. merge layer, every mode (fence / verify / index cache / small ring)
gcc $W $INC -c mgh.c -o $OUT/m.o 2>/dev/null; mkstubs $OUT/m.o "" $OUT/stubs.c
for FL in "" "-DMEGA_IDXCACHE=0" "-DMEGA_VERIFY" "-DMEGA_FENCE" "-DMEGA_VERIFY -DMEGA_FENCE -DMEGA_RING=3"; do
  gcc $W $FL $INC -o $OUT/mgh mgh.c $OUT/stubs.c $LIBS 2>/dev/null; run "mega layer [$FL]" $OUT/mgh; done
gcc $W $INC -o $OUT/mgh mgh.c $OUT/stubs.c $LIBS 2>/dev/null
for lag in 10 4; do printf "%-46s" "GPU-lag hazard simulation (lag $lag)"; HAZARD_LAG=$lag $OUT/mgh 2>&1 | grep -E "hazard simulation" | sed 's/.*checked, //'; done
printf "%-46s" "hazard detector has teeth (list mode, ring 2)"; gcc $W -DMEGA_IDXCACHE=0 -DMEGA_RING=2 $INC -o $OUT/mgh2 mgh.c $OUT/stubs.c $LIBS 2>/dev/null; HAZARD_LAG=6 HAZARD_EXPECT=1 $OUT/mgh2 2>&1 | grep -E "fails=0" >/dev/null && echo ok || { echo FAIL; fails=$((fails+1)); }
gcc -w -std=gnu11 -O1 -g -fsanitize=undefined,bounds -fno-sanitize-recover=all -no-pie -DMEGA_VERIFY $INC -o $OUT/mgub mgh.c $OUT/stubs.c $LIBS 2>/dev/null; run "mega layer under UBSan" $OUT/mgub
# 2. VSync/uncap + GPU timestamps
gcc $W -DPS5_UNCAP -DPS5_GPUTIME $INC -c ut.c -o $OUT/u.o 2>/dev/null; mkstubs $OUT/u.o "-e ^__real_sceVideoOutSubmitFlip$|^__real_sceVideoOutWaitVblank$" $OUT/stubs2.c 2>/dev/null
nm -u $OUT/u.o | awk '{print $2}' | grep -E "^(JNI_OnLoad|sce|egl|getdirentries|__real_|__wrap_)" | sort -u | grep -v "^__real_sceVideoOutSubmitFlip$\|^__real_sceVideoOutWaitVblank$\|^eglGetProcAddress$" | python3 -c "
import sys
print('#include <stddef.h>')
for s in sys.stdin.read().split(): print('long %s(long a,long b,long c,long d,long e,long f){return 0;}'%s)" > $OUT/stubs2.c
gcc $W $INC -o $OUT/ut ut.c $OUT/stubs2.c $LIBS 2>/dev/null; run "uncap + GPU timestamps" $OUT/ut
# 3. overlay (query-free state restore) and input / on-screen keyboard
for t in ov_test osk_test sdl_test fence_test; do gcc -w -std=gnu11 -O0 $INC -o $OUT/$t $t.c $LIBS -Wl,--unresolved-symbols=ignore-all 2>/dev/null; run "$t" $OUT/$t; done
gcc -w -std=gnu11 -O1 -g $INC -o $OUT/fdv fdv_test.c -lpthread 2>/dev/null; run "jar virtual descriptors (400 jars, limit 120)" $OUT/fdv
# Vulkan build of the shim (make_mcvk_title.sh): vulkan.h is looked up in the Mesa include dir, LWJGL 3.4.3 / 3.3.3 sources or the system; skipped when none has it
VKH=; for d in $HOME/PS5_Mesa/include $HOME/PS5_VulkanTemplate/external $HOME/lwjgl3-3.4.3/modules/lwjgl/vulkan/src/main/c $HOME/lwjgl3-3.3.3/modules/lwjgl/vulkan/src/main/c /tmp/lw/lwjgl3-3.4.3/modules/lwjgl/vulkan/src/main/c /usr/include; do [ -f $d/vulkan/vulkan.h ] && { VKH=$d; break; }; done
printf "%-46s" "vk_test (Vulkan shim: SDL_Vulkan_*, display surface)"; if [ -n "$VKH" ]; then gcc -w -std=gnu11 -O0 $INC -I$VKH -o $OUT/vk_test vk_test.c $LIBS -Wl,--unresolved-symbols=ignore-all 2>/dev/null; run_noname $OUT/vk_test; else echo "skipped (no vulkan.h)"; fi
printf "%-46s" "SDL struct offsets vs LWJGL 3.4.3 sources"; sl=$(python3 $T/sdl_layout.py 2>&1); echo "$sl" | grep -q "^fails=0" && { echo "$sl" | grep -q skipped && echo "skipped (no ~/lwjgl3-3.4.3)" || echo ok; } || { echo FAIL; echo "$sl" | head -5; fails=$((fails+1)); }
printf "%-46s" "launcher sounds (Ogg decode + mixer)"; ms=$(bash $T/mc_sounds_test.sh 2>&1); echo "$ms" | grep -q "fails=0" && echo ok || { echo "$ms" | grep -q skipped && echo "$ms" | tail -1 || { echo FAIL; echo "$ms" | tail -4; fails=$((fails+1)); }; }
# 4. vertex-format fix + chunk shader rewrite (glslang optional)
gcc -w -std=gnu11 -O0 -DPS5_VFIX $INC -o $OUT/h h.c $LIBS -Wl,--unresolved-symbols=ignore-all 2>/dev/null
( cd $OUT && ./h $T/shaders/solid.vsh $T/shaders/entity.vsh 2>&1 | grep -E "IPointer\(2xSHORT\) -> size=1|Pointer\(3xBYTE norm\) -> size=4" | wc -l | xargs printf "%-46s%s of 2 pointer conversions\n" "vertex-format rewrite" )
if command -v glslangValidator >/dev/null; then for f in solid entity; do cp $T/shaders/$f.vsh.out $OUT/$f.vert 2>/dev/null && printf "%-46s" "glslang: rewritten $f shader" && { glslangValidator -S vert $OUT/$f.vert >/dev/null 2>&1 && echo ok || echo FAIL; }; done; fi
# 5. fixed-function OpenGL (Minecraft 1.16.5 and older): gl4es + ps5gl4es.c rendering legacy GL on a real OpenGL 4.5 CORE context (Mesa software GL, headless EGL). Needs libEGL + Mesa and the gl4es sources (build_gl4es.sh clones them).
printf "%-46s" "fixed-function layer (gl4es on a core context)"
G4H=$OUT/g4home; mkdir -p $G4H; GL4ES_SRC=${GL4ES_DIR:-$HOME/gl4es}; [ -d "$GL4ES_SRC/src/gl" ] || GL4ES_SRC=$G4H/gl4es
if G4OUT=$(HOME=$G4H GL4ES_DIR=$GL4ES_SRC G4CC=gcc G4OBJCOPY=objcopy G4NM=nm G4AR=ar bash $T/../build_gl4es.sh 2>&1); then
  gcc -w -std=gnu11 -O1 -I$T/.. -o $OUT/g4_test $T/g4_test.c $G4H/gl4es_build/libgl4es_ps5.a -lEGL -lm -lpthread 2>$OUT/g4_link.txt && { out=$($OUT/g4_test 2>&1); echo "$out" | grep -q "^fails=0" && echo "ok ($(echo "$out" | grep -o '[0-9]* checks'))$(echo "$out" | grep -q SKIP && echo ' SKIPPED: no software GL here')" || { echo FAIL; echo "$out" | grep -E "FAIL|MISSING|fails=" | head -8; fails=$((fails+1)); }; } || { echo "FAIL (link)"; head -3 $OUT/g4_link.txt; fails=$((fails+1)); }
else echo "skipped (gl4es could not be built here: $(echo "$G4OUT" | tail -1 | cut -c1-90))"; fi
# 6. LWJGL 2 (Minecraft 1.12.2 and older): the port built from LWJGL 2.9.3's sources, run on a real JVM the way McBoot starts a game: Display / GL (through gl4es) / Keyboard / Mouse / OpenAL. Needs javac+java, gcc, git (first run clones lwjgl2), Mesa EGL.
printf "%-46s" "LWJGL 2 port (Display, GL, input, OpenAL)"
if command -v javac >/dev/null && command -v java >/dev/null && command -v git >/dev/null; then
  l2out=$(bash $T/lw2_test.sh $OUT/lw2 2>&1); echo "$l2out" | grep -q "^fails=0" && echo "ok ($(echo "$l2out" | grep -o '[0-9]* checks'))" || { echo FAIL; echo "$l2out" | grep -E "FAIL|Exception|Error|fails=" | head -8; fails=$((fails+1)); }
else echo "skipped (needs javac, java, git)"; fi
# 7. AWT-lite + javax.script (Minecraft <= 1.12.2 texture loading): PNG ImageIO / BufferedImage / Graphics2D on a JVM image without java.desktop, compared with the real AWT (needs the JDK sources, jlink)
printf "%-46s" "AWT-lite (PNG, BufferedImage, Graphics2D)"
if command -v javac >/dev/null && command -v gcc >/dev/null; then
  awout=$(bash $T/awt_test.sh $OUT/awt 2>&1); echo "$awout" | grep -q "^SKIP" && echo "skipped ($(echo "$awout" | grep SKIP | cut -c7-80))" || { echo "$awout" | tail -1 | grep -q "^fails=0" && echo "ok ($(echo "$awout" | grep -o '[0-9]* comparisons'))" || { echo FAIL; echo "$awout" | grep -E "FAIL|fails=" | head -8; fails=$((fails+1)); }; }
else echo "skipped (needs javac, gcc)"; fi
# 8. the title's own malloc (ps5malloc.inc): 8 threads, every size class, realloc, aligned blocks, calloc, dedicated mappings, libc-owned pointers, ASan + UBSan
printf "%-46s" "own malloc (8 threads, ASan/UBSan)"
if gcc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -o $OUT/om_test $T/om_test.c -lpthread 2>/dev/null; then omo=$($OUT/om_test 2>&1); echo "$omo" | grep -q "fails=0" && echo ok || { echo FAIL; echo "$omo" | tail -4; fails=$((fails+1)); }; else echo FAIL; fails=$((fails+1)); fi
# 9. USB keyboard / mouse (ps5usbin.inc, pure part): HID usage -> GLFW key, characters, record decoding, diffing, launcher pad bits, ASan + UBSan
printf "%-46s" "USB keyboard/mouse decode (ASan/UBSan)"
if gcc -w -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -o $OUT/usb_test $T/usb_test.c 2>/dev/null; then uo=$($OUT/usb_test 2>&1); echo "$uo" | grep -q "fails=0" && echo ok || { echo FAIL; echo "$uo" | tail -4; fails=$((fails+1)); }; else echo "FAIL (build)"; fails=$((fails+1)); fi
# 10. NID hashing used for the system-module symbol lookups (ps5lwjgl.c in_nid, extracted): known libkernel NIDs
printf "%-46s" "NID hash (known libkernel NIDs)"
sed -n '/^static void in_sha1/,/^extern int sceKernelGetModuleInfo/p' $T/../ps5lwjgl.c | grep -v '^extern int sceKernelGetModuleInfo' > $OUT/nid_extract.inc
if gcc -w -std=gnu11 -I$OUT -o $OUT/nid_test $T/nid_test.c 2>/dev/null; then no=$($OUT/nid_test 2>&1); echo "$no" | grep -q "fails=0" && echo ok || { echo FAIL; echo "$no" | tail -4; fails=$((fails+1)); }; else echo "FAIL (build)"; fails=$((fails+1)); fi
rm -f $T/shaders/*.out; echo; [ $fails = 0 ] && echo "ALL CHECKS PASSED" || echo "$fails FAILED"; rm -rf $OUT; exit $fails
