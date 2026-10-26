#!/bin/bash
# mc_deps.sh [version]: download an MC client + libraries and list which JDK modules they need (jdeps).
# Output dir: ~/mc_<version>   Needs: curl-free (python3 urllib), a JDK with jdeps.
set -e
V=${1:-1.20.1}; D=$HOME/mc_$V; mkdir -p $D/lib; cd $D
python3 - "$V" <<'PY'
import json, os, sys, urllib.request
v = sys.argv[1]
get = lambda u: urllib.request.urlopen(u).read()
m = json.loads(get("https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"))
j = json.loads(get(next(x for x in m["versions"] if x["id"] == v)["url"]))
print("javaVersion:", j.get("javaVersion"), "mainClass:", j["mainClass"])
if not os.path.exists("client.jar"): open("client.jar", "wb").write(get(j["downloads"]["client"]["url"]))
n = 0
for l in j["libraries"]:
    a = l.get("downloads", {}).get("artifact")
    if not a or "natives" in a["path"]: continue     # natives jars are not needed for module analysis
    p = "lib/" + os.path.basename(a["path"])
    if not os.path.exists(p): open(p, "wb").write(get(a["url"])); n += 1
print("libraries downloaded:", n, "total jars:", len(os.listdir("lib")))
PY
JD=$(command -v jdeps || echo /usr/lib/jvm/java-17-openjdk-amd64/bin/jdeps)
OURS="java.base java.logging java.naming java.xml java.management java.prefs java.sql java.transaction.xa jdk.unsupported jdk.crypto.ec java.security.sasl jdk.charsets jdk.zipfs"
echo "== modules needed by client + libraries"
NEED=$($JD --multi-release 17 --ignore-missing-deps --print-module-deps client.jar lib/*.jar)
echo "$NEED"
echo "== needed but NOT in our runtime image:"
for m in $(echo "$NEED" | tr ',' ' '); do case " $OURS " in *" $m "*) ;; *) echo "  $m";; esac; done
echo "== which jars reference them (summary):"
$JD --multi-release 17 --ignore-missing-deps -summary client.jar lib/*.jar 2>/dev/null \
  | grep -E -- "-> (java|jdk)\." | grep -v -E -- "-> (java\.base|java\.logging|java\.naming|java\.xml|java\.management|java\.prefs|java\.sql|java\.transaction\.xa|jdk\.unsupported|jdk\.crypto\.ec|java\.security\.sasl|jdk\.charsets|jdk\.zipfs)( |$)" | sort -u
