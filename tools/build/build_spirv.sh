#!/bin/bash
# build_spirv.sh: shaderc (GLSL -> SPIR-V: glslang + SPIRV-Tools) and SPIRV-Cross (SPIR-V -> GLSL) as static libraries for the console.
# Minecraft 26.x (LWJGL 3.4) compiles its shaders at run time through org.lwjgl.util.shaderc and org.lwjgl.util.spvc, which dlopen() "libshaderc" / "libspirv-cross" and
# call function pointers from dlsym(); ps5lwjgl.c hands out these statically linked functions instead (ps5spirv_syms.inc is generated here).
# Slow (a few hundred C++ files) but done ONCE: the result is cached in ~/spirv_build/ (objects + ps5spirv_syms.inc) and build_lwjgl.sh just reuses it. Needs cmake + python3 (apt install cmake).
# SPIRV_FORCE=1 rebuilds. Sources are the revisions pinned by shaderc v2026.4 (glslang, SPIRV-Tools, SPIRV-Headers) and SPIRV-Cross vulkan-sdk-1.4.363.0.
set -e
T=${T:-$HOME/ps5probe}; PSDK=$T/.deps/native/ps5-payload-sdk
SRC=$HOME/spirv_src; B=$HOME/spirv_build
command -v cmake >/dev/null || { echo "build_spirv.sh: cmake is missing: sudo apt install -y cmake"; exit 1; }
command -v python3 >/dev/null || { echo "build_spirv.sh: python3 is missing: sudo apt install -y python3"; exit 1; }
LWM=${LWJGL_MODULES:-$HOME/lwjgl3-3.4.3/modules/lwjgl}      # to read the function names LWJGL's bindings ask for
if [ -z "$SPIRV_FORCE" ] && [ -f $B/done ] && [ -f $B/ps5spirv_syms.inc ]; then echo "SPIRV: cached ($(ls $B/obj | wc -l) objects)"; exit 0; fi
mkdir -p $SRC $B/obj $B/tc; rm -f $B/obj/*.o $B/done
fetchrev() { [ -d "$1/.git" ] && return 0; mkdir -p "$1"; ( cd "$1" && git init -q . && git fetch -q --depth 1 "$2" "$3" && git checkout -q FETCH_HEAD ); }
[ -d $SRC/shaderc/.git ] || git clone -q --depth 1 --branch v2026.4 https://github.com/google/shaderc $SRC/shaderc
fetchrev $SRC/shaderc/third_party/glslang       https://github.com/KhronosGroup/glslang       e1b562a8bed273a02f30b59b66a5d499793cede5
fetchrev $SRC/shaderc/third_party/spirv-tools   https://github.com/KhronosGroup/SPIRV-Tools   ef96ed763b43b59b33b31b362f09a02b729fa1c9
fetchrev $SRC/shaderc/third_party/spirv-headers https://github.com/KhronosGroup/SPIRV-Headers 04fd3caa1e8267e4d95c806cad901181728e1006
[ -d $SRC/spirv-cross/.git ] || git clone -q --depth 1 --branch vulkan-sdk-1.4.363.0 https://github.com/KhronosGroup/SPIRV-Cross $SRC/spirv-cross
# shaderc sets GLSLANG_ENABLE_INSTALL to the generator-expression string "$<NOT:...>", which is "true" to if(): glslang then tries to install and cmake rejects the export sets. Turn that off.
sed -i 's|set(GLSLANG_ENABLE_INSTALL \$<NOT:\${SKIP_GLSLANG_INSTALL}>)|set(GLSLANG_ENABLE_INSTALL OFF)|' $SRC/shaderc/third_party/CMakeLists.txt
grep -q "set(GLSLANG_ENABLE_INSTALL OFF)" $SRC/shaderc/third_party/CMakeLists.txt || { echo "build_spirv.sh: could not patch shaderc/third_party/CMakeLists.txt"; exit 1; }
# compiler wrappers (cmake wants plain executables): the console target, C++ with exceptions + RTTI (glslang / SPIRV-Cross throw internally and catch at their API)
CLANGW="env PS5_PAYLOAD_SDK=$PSDK PS5_CLANG=clang-18 sh $T/tooling/prospero-clang18"
printf '#!/bin/sh\nexec %s "$@"\n' "$CLANGW" > $B/tc/cc
printf '#!/bin/sh\nexec %s -fexceptions -frtti "$@"\n' "$CLANGW" > $B/tc/cxx
AR=$(ls $PSDK/bin/llvm-ar 2>/dev/null || command -v llvm-ar-18 || command -v llvm-ar || command -v ar)
RANLIB=$(ls $PSDK/bin/llvm-ranlib 2>/dev/null || command -v llvm-ranlib-18 || command -v llvm-ranlib || command -v ranlib)
chmod +x $B/tc/cc $B/tc/cxx
cat > $B/tc/ps5.cmake <<E
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER $B/tc/cc)
set(CMAKE_CXX_COMPILER $B/tc/cxx)
set(CMAKE_AR $AR)
set(CMAKE_RANLIB $RANLIB)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_CXX_COMPILER_WORKS 1)
set(CMAKE_THREAD_LIBS_INIT "")
set(CMAKE_HAVE_THREADS_LIBRARY 1)
set(CMAKE_USE_PTHREADS_INIT 1)
set(Threads_FOUND TRUE)
E
CFG="-G Ninja -DCMAKE_TOOLCHAIN_FILE=$B/tc/ps5.cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-w -DCMAKE_CXX_FLAGS=-w -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF -DCMAKE_HAVE_LIBC_PTHREAD=1 -DTHREADS_PREFER_PTHREAD_FLAG=OFF"
command -v ninja >/dev/null || CFG=${CFG#-G Ninja }
J=${JOBS:-$(nproc 2>/dev/null || echo 4)}
echo "== shaderc (glslang + SPIRV-Tools), $J jobs"
cmake -S $SRC/shaderc -B $B/shaderc $CFG -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -DSHADERC_SKIP_INSTALL=ON \
  -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON -DSPIRV_WERROR=OFF -DENABLE_GLSLANG_BINARIES=OFF -DENABLE_HLSL=OFF -DENABLE_CTEST=OFF -DENABLE_OPT=ON -DGLSLANG_ENABLE_INSTALL=OFF \
  -DSHADERC_ENABLE_WERROR_COMPILE=OFF -DSHADERC_ENABLE_SHARED_CRT=OFF > $B/cmake_shaderc.log 2>&1 || { echo "cmake (shaderc) failed:"; tail -25 $B/cmake_shaderc.log; exit 1; }
cmake --build $B/shaderc --target shaderc -j $J > $B/build_shaderc.log 2>&1 || { echo "build (shaderc) failed:"; grep -E "error|FAILED" $B/build_shaderc.log | head -20; exit 1; }
echo "== SPIRV-Cross (GLSL back end + C API)"
cmake -S $SRC/spirv-cross -B $B/cross $CFG -DSPIRV_CROSS_STATIC=ON -DSPIRV_CROSS_SHARED=OFF -DSPIRV_CROSS_CLI=OFF -DSPIRV_CROSS_ENABLE_TESTS=OFF \
  -DSPIRV_CROSS_ENABLE_GLSL=ON -DSPIRV_CROSS_ENABLE_HLSL=OFF -DSPIRV_CROSS_ENABLE_MSL=OFF -DSPIRV_CROSS_ENABLE_CPP=OFF -DSPIRV_CROSS_ENABLE_REFLECT=OFF -DSPIRV_CROSS_ENABLE_C_API=ON \
  -DSPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS=OFF -DSPIRV_CROSS_SKIP_INSTALL=ON > $B/cmake_cross.log 2>&1 || { echo "cmake (SPIRV-Cross) failed:"; tail -25 $B/cmake_cross.log; exit 1; }
cmake --build $B/cross -j $J > $B/build_cross.log 2>&1 || { echo "build (SPIRV-Cross) failed:"; grep -E "error|FAILED" $B/build_cross.log | head -20; exit 1; }
# every static library of both builds -> one object directory (archive members are renamed per library: several libraries have a file of the same name)
n=0
for a in $(find $B/shaderc $B/cross -name '*.a' | sort); do
  n=$((n+1)); X=$B/x$n; rm -rf $X; mkdir -p $X; ( cd $X && $AR x $a )
  for o in $X/*.o; do [ -f "$o" ] && mv "$o" $B/obj/spv${n}_$(basename "$o"); done; rm -rf $X
done
echo "SPIRV: $(ls $B/obj | wc -l) objects from $n libraries"
# the function tables: names LWJGL's bindings ask for that are defined by the objects
NM=$(ls $PSDK/bin/llvm-nm 2>/dev/null || command -v llvm-nm-18 || command -v llvm-nm || command -v nm)
$NM --defined-only -g $B/obj/*.o 2>/dev/null | awk 'NF==3 && $2 ~ /^[TtWw]$/ {print $3}' | grep -E '^(shaderc_|spvc_)' | sort -u > $B/names.txt
for lib in shaderc/src/generated/java/org/lwjgl/util/shaderc/Shaderc.java spvc/src/generated/java/org/lwjgl/util/spvc/Spvc.java; do
  grep -ho 'apiGetFunctionAddress([A-Za-z.()]*, "[A-Za-z0-9_]*")' $LWM/$lib | sed 's/.*"\(.*\)")/\1/'
done | sort -u > $B/wanted.txt
[ -s $B/wanted.txt ] || { echo "build_spirv.sh: no function names found in $LWM (LWJGL 3.4.3 checkout missing?)"; exit 1; }
comm -12 $B/names.txt $B/wanted.txt > $B/provided.txt
echo "SPIRV: LWJGL asks for $(wc -l < $B/wanted.txt) functions, $(wc -l < $B/provided.txt) are built in"
comm -13 $B/names.txt $B/wanted.txt | head -20 | sed 's/^/  not built in: /'
{ echo "// generated by build_spirv.sh: shaderc_* / spvc_* functions of the statically linked shaderc and SPIRV-Cross that LWJGL's bindings ask for"
  echo "static int h_shaderc, h_spvc;"
  sed 's/^/extern void /; s/$/(void);/' $B/provided.txt
  echo "static const Sym shaderc_syms[] = {"; grep '^shaderc_' $B/provided.txt | while read nm; do echo "    { \"$nm\", (void *)$nm },"; done; echo "    { 0, 0 } };"
  echo "static const Sym spvc_syms[] = {"; grep '^spvc_' $B/provided.txt | while read nm; do echo "    { \"$nm\", (void *)$nm },"; done; echo "    { 0, 0 } };"; } > $B/ps5spirv_syms.inc
touch $B/done
echo "SPIRV: done"
