#!/bin/bash
# mc_vk_probe.sh [version]: what does Mojang's own Vulkan renderer need? Downloads the client jar (cached in ~/mc_<version>), lists its Vulkan classes and the LWJGL / SDL calls they make.
# Writes ~/mc_<version>/vk_probe.txt (send that file). Needs python3 and a JDK with javap.
set -e
V=${1:-26.3}; D=$HOME/mc_$V; mkdir -p $D; cd $D
python3 - "$V" <<'PY'
import json, os, sys, urllib.request
v = sys.argv[1]; get = lambda u: urllib.request.urlopen(u).read()
m = json.loads(get("https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"))
j = json.loads(get(next(x for x in m["versions"] if x["id"] == v)["url"]))
print("javaVersion:", j.get("javaVersion"), "mainClass:", j["mainClass"])
if not os.path.exists("client.jar"): open("client.jar", "wb").write(get(j["downloads"]["client"]["url"]))
open("libs.txt", "w").write("\n".join(l["name"] for l in j["libraries"]) + "\n")
PY
JP=$(command -v javap || ls /usr/lib/jvm/*/bin/javap | head -1)
rm -rf x; mkdir x; (cd x && unzip -q ../client.jar)
{
echo "== libraries (LWJGL parts)"; grep -i "lwjgl" libs.txt | sort -u
echo; echo "== classes with vulkan / vk in the name"; (cd x && find . -name '*.class' | grep -i -E "vulkan|/vk" | sort)
echo; echo "== text hits (strings in classes)"
for s in SDL_Vulkan SDLVulkan vkGetInstanceProcAddr VK.create VK_KHR_ VK_EXT_ shaderc spvc glfwCreateWindowSurface glfwGetRequiredInstanceExtensions preferredGraphicsApi graphicsApi "Graphics API" validation; do
  printf '%-34s %s classes\n' "$s" "$(cd x && grep -rl -a "$s" --include='*.class' . | wc -l)"; done
echo; echo "== classes that mention SDL_Vulkan / vkGetInstanceProcAddr / VK_KHR_"
(cd x && grep -rl -a -E "SDL_Vulkan|SDLVulkan|vkGetInstanceProcAddr|VK_KHR_|VK_EXT_" --include='*.class' . | sort | head -60)
echo; echo "== LWJGL Vulkan / SDL calls made by the Vulkan classes (javap, invoke lines)"
for c in $(cd x && find . -name '*.class' | grep -i -E "vulkan|/vk" | sed 's#^\./##; s#\.class$##; s#/#.#g' | head -80); do
  "$JP" -c -p -cp client.jar "$c" 2>/dev/null | grep -o -E "(org/lwjgl/(sdl|vulkan|util/shaderc|util/spvc)/[A-Za-z0-9_]+\.[A-Za-z0-9_]+)" | sort -u | sed "s#^#$c  #"
done | head -400
echo; echo "== window creation (SDL flags)"
(cd x && grep -rl -a -E "SDL_WINDOW_VULKAN|SDL_CreateWindow" --include='*.class' . | head -10)
} > vk_probe.txt 2>&1
wc -l vk_probe.txt; echo "send: $D/vk_probe.txt"
