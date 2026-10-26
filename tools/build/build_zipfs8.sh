#!/bin/bash
# build_zipfs8.sh: add the zip file system provider (scheme "jar"/"zip") to the Java 8 runtime: ~/ps5jvm8/jdk/lib/ext/zipfs.jar.
# Why: in JDK 8 it is a DEMO (jdk/src/share/demo/nio/zipfs, built by `make demos`), not part of rt.jar; build_jdk.sh JDKV=8 never built it, so
#      Minecraft 1.12.2 / 1.16.5 died with java.nio.file.ProviderNotFoundException: Provider "jar" not found (they read their own client jar through FileSystems.newFileSystem).
# Run once after build_jdk.sh JDKV=8 (build_jdk.sh calls it by itself now); safe to re-run. Then make_jvm_title.sh copies the tree into the title.
set -e
OUT=${OUT:-$HOME/ps5jvm8}; SRC=${JDK8SRC:-$HOME/jdk8u}/jdk/src/share/demo/nio/zipfs/src
JAVAC=${JAVAC:-/usr/lib/jvm/java-17-openjdk-amd64/bin/javac}; JAR=${JAR:-$(dirname $JAVAC)/jar}
[ -d "$SRC" ] || { echo "zipfs sources not found: $SRC"; exit 1; }
[ -d "$OUT/jdk/lib" ] || { echo "$OUT/jdk/lib missing: run JDKV=8 bash build_jdk.sh first"; exit 1; }
W=$(mktemp -d); trap 'rm -rf $W' EXIT
mkdir -p $W/classes
$JAVAC --release 8 -nowarn -d $W/classes $(find $SRC -name '*.java') 2>&1 | grep -v '^Note:' || true
cp -r $SRC/META-INF $W/classes/
mkdir -p $OUT/jdk/lib/ext
$JAR cf $OUT/jdk/lib/ext/zipfs.jar -C $W/classes .
echo "== $OUT/jdk/lib/ext/zipfs.jar ($(stat -c %s $OUT/jdk/lib/ext/zipfs.jar) bytes), providers: $(cat $SRC/META-INF/services/java.nio.file.spi.FileSystemProvider | tr '\n' ' ')"
