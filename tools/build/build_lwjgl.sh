#!/bin/bash
# -DX86_64 below: LWJGL's bundled ffitarget.h only picks the 64-bit ABI enum (FFI_DEFAULT_ABI=UNIX64) and the 32-byte closure trampoline when X86_64 is defined;
# without it LWJGL reports FFI_DEFAULT_ABI=1 and libffi rejects it (FFI_BAD_ABI = 2) and ffi_closure is too small.
# -Djvm=lwjgl_jvm below: LWJGL core and libmanagement.a both define a global "jvm"; rename LWJGL's so both link.
# build_lwjgl.sh: compile LWJGL's natives ($LWJGL_VER, default 3.3.1) (core, opengl, stb) + libffi + ps5lwjgl.c into ~/ps5jvm/liblwjgl_ps5.a
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
T=${T:-$HOME/ps5probe}; OUT=${OUT:-$HOME/ps5jvm}; SDKGL=${SDKGL:-$HOME/glsdk/ps5-opengl-sdk-0.3.0/sdk}
JNIINC=${JNIINC:-$HOME/jdk17u/src/java.base}
PSDK=$T/.deps/native/ps5-payload-sdk
OBJCOPY=$(ls $PSDK/bin/llvm-objcopy 2>/dev/null || command -v llvm-objcopy-18 || command -v llvm-objcopy || command -v objcopy)
# ccache (the wrapper uses it when USE_CCACHE=1): an unchanged source compiles in milliseconds. NOCCACHE=1 turns it off.
CCX=; [ "$NOCCACHE" != 1 ] && command -v ccache >/dev/null && CCX="USE_CCACHE=1"
CC="env PS5_PAYLOAD_SDK=$PSDK PS5_CLANG=clang-18 $CCX sh $T/tooling/prospero-clang18"
CCID="$(clang-18 --version 2>&1 | head -1)|$(md5sum < $T/tooling/prospero-clang18 | cut -c1-12)"   # compiler identity for the object caches below
mkdir -p $OUT $HOME/lwjgl_build/obj; cd $HOME
LV=${LWJGL_VER:-3.3.1}   # LWJGL_VER=3.3.3 (make_jvm_title.sh sets it for launcher builds: Sodium and newer Minecraft versions want 3.3.3); the sources go to ~/lwjgl3-$LV
# LWJGL_VERS="3.3.3 3.4.3": natives of SEVERAL LWJGL generations in one title (ascending). 3.4's natives are not a superset of 3.3's (138 core JNI functions went away) and 3.3's lack 3.4's new ones,
# so each generation's objects go into the archive; the older ones are made weak (objcopy --weaken) so that names both have resolve to the newest copy and names only the older has still link.
# 3.4 has its own FreeBSD platform (the console reports os.name FreeBSD; 3.3.x maps that to LINUX): its org.lwjgl.system.freebsd.DynamicLinkLoader JNI is built too.
# The Java side picks the matching jar set per Minecraft version (McBoot). 26.x (LWJGL 3.4.3) also needs FreeType (below).
VERS=${LWJGL_VERS:-$LV}
mkdir -p $HOME/lwjgl_build/shim && printf '#pragma once\n#include <stdlib.h>\n#ifndef alloca\n#define alloca(n) __builtin_alloca(n)\n#endif\n' > $HOME/lwjgl_build/shim/alloca.h
[ -d libffi-3.4.6 ] || curl -sL https://github.com/libffi/libffi/releases/download/v3.4.6/libffi-3.4.6.tar.gz | tar -xz
F=$HOME/libffi-3.4.6
O=$HOME/lwjgl_build/obj; rm -f $O/*.o; mkdir -p $O; n=0; nver=0; NEWEST=${VERS##* }
for V in $VERS; do
  if [ ! -d lwjgl3-$V/modules ]; then
    if [ "$V" = 3.3.1 ] && [ -d lwjgl3/modules ]; then ln -sfn lwjgl3 lwjgl3-$V   # the older, unversioned checkout is 3.3.1
    else git clone -q --depth 1 --branch $V https://github.com/LWJGL/lwjgl3 lwjgl3-$V; fi
  fi
  M=$HOME/lwjgl3-$V/modules/lwjgl; nver=$((nver+1)); P=v${nver}_
  FL="-std=gnu11 -O2 -fPIC -DX86_64 -Djvm=lwjgl_jvm -DLWJGL_LINUX -D_ALLBSD_SOURCE -include string.h -I$HOME/lwjgl_build/shim -I$JNIINC/share/native/include -I$JNIINC/unix/native/include -I$M/core/src/main/c -I$M/core/src/main/c/linux -I$M/core/src/main/c/libffi -I$M/core/src/main/c/libffi/x86 -I$M/opengl/src/main/c -I$M/opengl/src/generated/c -I$M/stb/src/main/c -I$M/stb/src/generated/c"
  vn=0
  # object cache: the natives of one LWJGL generation only depend on its sources, FL and the compiler; FORCE=1 rebuilds
  WK=strong; [ "$V" != "$NEWEST" ] && WK=weak
  JH=$(cat $JNIINC/share/native/include/jni.h $JNIINC/unix/native/include/jni_md.h 2>/dev/null | md5sum | cut -c1-12)   # the JDK's JNI headers, not their path: JDKV=17/21/25 builds share the objects
  CK=$(printf '%s' "rev1|$V|$P|$WK|${FL//$JNIINC/JNI}|$JH|$CCID|$(git -C $HOME/lwjgl3-$V rev-parse HEAD 2>/dev/null)" | md5sum | cut -c1-16); CDV=$HOME/lwjgl_build/cache/$V-$P$CK
  if [ -z "$FORCE" ] && [ -f $CDV/.done ]; then
    cp $CDV/*.o $O/; vn=$(cat $CDV/.n); n=$((n+vn)); echo "LWJGL $V natives: $vn objects (from cache $CK)"; continue
  fi
  for f in $M/core/src/main/c/*.c $M/core/src/generated/c/*.c $M/core/src/generated/c/linux/org_lwjgl_system_linux_DynamicLinkLoader.c $(ls $M/core/src/generated/c/freebsd/org_lwjgl_system_freebsd_DynamicLinkLoader.c 2>/dev/null) $(find $M/opengl -name '*.c' ! -name 'WindowsLWJGL*' ! -path '*windows*') $(find $M/stb -name '*.c' ! -name stb_vorbis.c); do
    b=$(basename $f .c); $CC $FL -c $f -o $O/$P$b.o 2>$O/$P$b.err || { grep -q "Windows\|WGL" $O/$P$b.err && continue; echo "FAILED $V $b"; head -3 $O/$P$b.err; exit 1; }; vn=$((vn+1)); n=$((n+1))
  done
  echo "LWJGL $V natives: $vn objects"
  if [ "$V" != "$NEWEST" ]; then for o in $O/$P*.o; do $OBJCOPY --weaken $o; done; echo "LWJGL $V objects made weak (a newer generation is linked too)"; fi
  mkdir -p $CDV; ls -dt $HOME/lwjgl_build/cache/$V-$P* | tail -n +5 | xargs -r rm -rf   # keep the 4 newest per generation
  cp $O/$P*.o $CDV/; echo $vn > $CDV/.n; touch $CDV/.done   # save (only reached when every file compiled)
done
M=$HOME/lwjgl3-$NEWEST/modules/lwjgl
echo "LWJGL natives: $n objects ($nver generation(s): $VERS)"
# LWJGL_VMA=1 (make_jvm_title.sh vk): the Vulkan memory allocator natives (org.lwjgl.util.vma). Minecraft 26.x loads VMA at start-up in every renderer (it fails with UnsatisfiedLinkError without them).
# Built with the Vulkan project's compiler wrapper (VKCC, PS5_PAYLOAD_SDK=$VKSDK) as make_mcvk_title.sh does: C++, renamed malloc hooks (core has its own), NDEBUG (VMA's debug asserts killed the first Vulkan start).
case " $VERS " in *" 3.4"*) if [ "$LWJGL_VMA" = 1 ]; then
  VM=$HOME/lwjgl3-$NEWEST/modules/lwjgl; VKCCW=${VKCC:-$HOME/PS5_Vulkan/tooling/prospero-clang18}
  [ -d $VM/vma/src/generated/c ] && [ -f "$VKCCW" ] && [ -n "$VKSDK" ] || { echo "LWJGL_VMA=1 needs $VM/vma, $VKCCW and VKSDK"; exit 1; }
  VFL="-O2 -fPIC -DX86_64 -Djvm=lwjgl_jvm -DLWJGL_LINUX -D_ALLBSD_SOURCE -include string.h -I$HOME/lwjgl_build/shim -I$JNIINC/share/native/include -I$JNIINC/unix/native/include -I$VM/core/src/main/c -I$VM/core/src/main/c/linux -I$VM/vma/src/main/c -I$VM/vma/src/generated/c -I$VM/vulkan/src/main/c -std=gnu++17 -fno-exceptions -fno-rtti -DNDEBUG"
  for nm in malloc calloc realloc free aligned_alloc aligned_free; do VFL="$VFL -Dorg_lwjgl_$nm=org_lwjgl_vma_$nm"; done
  VK_=$(printf '%s' "rev1|$NEWEST|$VFL|$(md5sum < $VKCCW | cut -c1-12)|$(cat $VM/vma/src/generated/c/*.cpp $VM/vma/src/main/c/*.h 2>/dev/null | md5sum)" | md5sum | cut -c1-16); VCD=$HOME/lwjgl_build/cache/vma-$VK_
  if [ -z "$FORCE" ] && [ -f $VCD/.done ]; then cp $VCD/vma_*.o $O/; vmn=$(cat $VCD/.n); else
    vmn=0; for f in $VM/vma/src/generated/c/*.cpp; do b=$(basename $f .cpp)
      PS5_PAYLOAD_SDK=$VKSDK sh $VKCCW $VFL -c $f -o $O/vma_$b.o 2>$O/vma_$b.err || { echo "FAILED VMA $b"; head -8 $O/vma_$b.err; exit 1; }; vmn=$((vmn+1)); done
    mkdir -p $VCD; ls -dt $HOME/lwjgl_build/cache/vma-* | tail -n +3 | xargs -r rm -rf; cp $O/vma_*.o $VCD/; echo $vmn > $VCD/.n; touch $VCD/.done
  fi
  echo "VMA natives: $vmn objects"
fi;; esac
# FreeType (Minecraft 26.x, LWJGL 3.4: org.lwjgl.util.freetype binds FreeType through function pointers found with dlsym("freetype")): built statically, FT_* table generated for ps5lwjgl.c
FTDEF=
case " $VERS " in *" 3.4"*)
  FTV=${FREETYPE_TAG:-VER-2-14-3}; FTD=$HOME/freetype-$FTV
  [ -d $FTD/src ] || git clone -q --depth 1 --branch $FTV https://github.com/freetype/freetype $FTD
  FS=$HOME/lwjgl_build/ftshim; mkdir -p $FS
  cat > $FS/ps5ftmodule.h <<'FTM'
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, t1_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, cff_driver_class )
FT_USE_MODULE( FT_Module_Class, psaux_module_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, pshinter_module_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )
FT_USE_MODULE( FT_Renderer_Class, ft_raster1_renderer_class )
FTM
  FTFL="-std=gnu11 -O2 -fPIC -DFT2_BUILD_LIBRARY -DFT_CONFIG_MODULES_H=<ps5ftmodule.h> -I$FS -I$FTD/include -include string.h"
  fn=0
  FK=$(printf '%s' "rev1|$FTV|$FTFL|$CCID|$(cat $FS/ps5ftmodule.h | md5sum)" | md5sum | cut -c1-16); FCD=$HOME/lwjgl_build/cache/freetype-$FK
  if [ -z "$FORCE" ] && [ -f $FCD/.done ]; then cp $FCD/ft_*.o $O/; fn=$(cat $FCD/.n); else
  for f in src/base/ftsystem.c src/base/ftinit.c src/base/ftdebug.c src/base/ftbase.c src/base/ftbbox.c src/base/ftbitmap.c src/base/ftglyph.c src/base/ftmm.c src/base/ftsynth.c src/base/ftgasp.c \
           src/base/ftfstype.c src/base/ftstroke.c src/base/fttype1.c src/base/ftotval.c src/gzip/ftgzip.c src/truetype/truetype.c src/cff/cff.c src/type1/type1.c src/psaux/psaux.c src/psnames/psmodule.c \
           src/pshinter/pshinter.c src/sfnt/sfnt.c src/smooth/smooth.c src/raster/raster.c; do
    b=$(basename $f .c); $CC $FTFL -c $FTD/$f -o $O/ft_$b.o 2>$O/ft_$b.err || { echo "FAILED FreeType $f"; head -5 $O/ft_$b.err; exit 1; }; fn=$((fn+1))
  done
  mkdir -p $FCD; ls -dt $HOME/lwjgl_build/cache/freetype-* | tail -n +3 | xargs -r rm -rf; cp $O/ft_*.o $FCD/; echo $fn > $FCD/.n; touch $FCD/.done
  fi
  echo "FreeType $FTV: $fn objects"
  # the FT_* functions LWJGL asks for (all org.lwjgl.util.freetype classes), keep those the build defines
  FTJ=$HOME/lwjgl3-$NEWEST/modules/lwjgl/freetype/src/generated/java/org/lwjgl/util/freetype
  grep -rhoE 'apiGetFunctionAddress\([A-Za-z_]+, "FT_[A-Za-z0-9_]+"\)' $FTJ | sed 's/.*"\(.*\)")/\1/' | sort -u > $O/ft_wanted.txt
  $PSDK/bin/llvm-nm --defined-only -g $O/ft_*.o 2>/dev/null | awk '$2 ~ /^[TDBR]$/ {print $3}' | sort -u > $O/ft_defined.txt
  comm -12 $O/ft_wanted.txt $O/ft_defined.txt > $O/ft_names.txt
  echo "FreeType: LWJGL asks for $(wc -l < $O/ft_wanted.txt) functions, $(wc -l < $O/ft_names.txt) are built in"
  { echo "// generated by build_lwjgl.sh: the FT_* functions of the statically linked FreeType that LWJGL's binding asks for"; echo "static int h_ft;";
    while read nm; do echo "extern void $nm(void);"; done < $O/ft_names.txt
    echo "static const Sym ft_syms[] = {"; while read nm; do echo "    { \"$nm\", (void *)$nm },"; done < $O/ft_names.txt; echo "    { 0, 0 } };"; } > $HOME/lwjgl_build/ps5ft_syms.inc
  FTDEF="-DPS5_FREETYPE -I$HOME/lwjgl_build"
  ;;
esac
# shaderc + SPIRV-Cross (Minecraft 26.x compiles its shaders at run time: org.lwjgl.util.shaderc / spvc dlopen "libshaderc" / "libspirv-cross"): built once by build_spirv.sh, cached in ~/spirv_build
SPDEF=; SPOBJ=
case " $VERS " in *" 3.4"*)
  T=$T LWJGL_MODULES=$HOME/lwjgl3-$NEWEST/modules/lwjgl bash $HERE/build_spirv.sh || { echo "build_spirv.sh failed"; exit 1; }
  SPDEF="-DPS5_SPIRV -I$HOME/spirv_build"; SPOBJ="$HOME/spirv_build/obj/*.o"
  ;;
esac
# libffi: build with its own generated headers (LWJGL's bundled ffi.h is older than these sources)
FD=$HOME/lwjgl_build/ffi; mkdir -p $FD
sed -e 's/@VERSION@/3.4.6/' -e 's/@TARGET@/X86_64/' -e 's/@HAVE_LONG_DOUBLE@/1/' -e 's/@FFI_EXEC_TRAMPOLINE_TABLE@/0/' $F/include/ffi.h.in > $FD/ffi.h
cp $F/src/x86/ffitarget.h $FD/ffitarget.h
cat > $FD/fficonfig.h <<'CFG'
#define HAVE_ALLOCA 1
#define HAVE_ALLOCA_H 1
#define HAVE_MMAP 1
#define HAVE_MMAP_ANON 1
#define HAVE_MEMCPY 1
#define STDC_HEADERS 1
#define HAVE_AS_X86_PCREL 1
#define HAVE_AS_ASCII_PSEUDO_OP 1
#define HAVE_AS_STRING_PSEUDO_OP 1
#define EH_FRAME_FLAGS "a"
#define HAVE_HIDDEN_VISIBILITY_ATTRIBUTE 1
#ifdef LIBFFI_ASM
#define FFI_HIDDEN(name) .hidden name
#else
#define FFI_HIDDEN __attribute__((visibility("hidden")))
#endif
#define FFI_EXEC_TRAMPOLINE_TABLE 0
CFG
FFL="-std=gnu11 -O2 -fPIC -DHAVE_CONFIG_H -I$HOME/lwjgl_build/shim -I$FD -I$F/src/x86 -I$F/include -I$F/src"
# closures.c is omitted on purpose: ps5lwjgl.c provides ffi_closure_alloc/free (RWX mmap)
for f in prep_cif types x86/ffi64; do $CC $FFL -c $F/src/$f.c -o $O/ffi_$(basename $f).o; done
$CC $FFL -c $F/src/x86/unix64.S -o $O/ffi_unix64.o
[ "$MDLOOP" = 1 ] && LWJGL_EXTRA="$LWJGL_EXTRA -DPS5_MDLOOP"   # experiment: Sodium's multi-draw issued as separate glDrawElementsBaseVertex calls (see ps5lwjgl.c)
[ "$NOSODFIX" = 1 ] && LWJGL_EXTRA="$LWJGL_EXTRA -DNO_SODFIX"   # keep Sodium's integer vertex formats as they are (A/B switch for the R32_UINT conversion)
[ "$NOPRESENT" = 1 ] && LWJGL_EXTRA="$LWJGL_EXTRA -DNO_PRESENT_DRAW"   # keep the real present blit (A/B switch for the textured-triangle replacement)
[ "$NOFENCEFAKE" = 1 ] && LWJGL_EXTRA="$LWJGL_EXTRA -DNO_FENCEFAKE"   # keep the SDK's own GL sync objects (ps5lwjgl.c: they cost a GPU round trip each)
[ -n "$FENCE_AGE" ] && LWJGL_EXTRA="$LWJGL_EXTRA -DFENCE_AGE=$FENCE_AGE"
[ "$NOBLITFIX" = 1 ] && LWJGL_EXTRA="$LWJGL_EXTRA -DNO_BLITFIX"   # A/B switch for the no-op blit fix (ps5lwjgl.c)
[ "$LAZYLINK" = 1 ] && LWJGL_EXTRA="$LWJGL_EXTRA -DPS5_LAZYLINK"   # experiment: answer glGetProgramiv(LINK_STATUS) / glGetShaderiv(COMPILE_STATUS) "true" at once, check the real link status at the first glUseProgram (ps5lwjgl.c "shader build time probe")
[ "$GLCPU" = 1 ] && LWJGL_EXTRA="$LWJGL_EXTRA -DPS5_GLALL -DPS5_GLCPU"   # diagnostic: CPU time per GL function (no glFinish), cheap now that clock_gettime is a TSC read
[ "$GLALL" = 1 ] && LWJGL_EXTRA="$LWJGL_EXTRA -DPS5_GLALL"   # diagnostic: per-GL-function GPU time (see ps5lwjgl.c "GLALL=1"); needs libffi's header from $FD
# fixed-function OpenGL (Minecraft 1.16.5 and older): gl4es + ps5gl4es.c, see build_gl4es.sh. GL4ES=0 builds without it.
G4OBJ=
if [ "${GL4ES:-1}" != 0 ]; then T=$T OUT=$OUT bash $HERE/build_gl4es.sh || { echo "build_gl4es.sh failed (GL4ES=0 builds the title without fixed-function support)"; exit 1; }; G4OBJ="$(ls $HOME/gl4es_build/*.o)"; fi
# LWJGL 2 (Minecraft 1.12.2 and older): build_lwjgl2.sh, objects l2_*.o + the Java jar (make_jvm_title.sh stages it under assets/lib2). LWJGL2=0 builds without it.
L2OBJ=
if [ "${LWJGL2:-1}" != 0 ]; then T=$T bash $HERE/build_lwjgl2.sh || { echo "build_lwjgl2.sh failed (LWJGL2=0 builds the title without LWJGL 2 support)"; exit 1; }; L2OBJ="$(ls ${LWJGL2_BUILD:-$HOME/lwjgl2_build}/obj/*.o)"; fi
$CC -std=gnu11 -O2 -fPIC -I$SDKGL/include -I$JNIINC/share/native/include -I$JNIINC/unix/native/include -I$FD -I$HERE $LWJGL_EXTRA $FTDEF $SPDEF -c $HERE/ps5lwjgl.c -o $O/ps5lwjgl.o
$CC -std=gnu11 -O2 -fPIC -I$JNIINC/share/native/include -I$JNIINC/unix/native/include -c $HERE/ps5awt.c -o $O/ps5awt.o   # no-op AWT natives (initIDs, JNI_OnLoad_awt) for the AWT-lite classes, see build_awtlite.sh
[ -n "$SPDEF" ] && $CC -std=gnu11 -O2 -fPIC -c $HERE/ps5locale.c -o $O/ps5locale.o   # *_l locale functions libc++'s iostreams need (shaderc / SPIRV-Cross), see ps5locale.c
rm -f $OUT/liblwjgl_ps5.a; $PSDK/bin/llvm-ar rcs $OUT/liblwjgl_ps5.a $O/*.o $SPOBJ $G4OBJ $L2OBJ
echo "== $OUT/liblwjgl_ps5.a ($(du -h $OUT/liblwjgl_ps5.a | cut -f1))"
