#!/bin/bash
# make_gl_cube.sh <PS5_IP>: build and deploy ps5-opengl's rotating-cube sample as its own title (PPSA99005).
# Proves OpenGL 4.6 Core + EGL fullscreen presentation on your console, independent of the JVM title.
# Needs: ~/ps5probe built once (provides the SDK/runtime), git, curl, python3, clang-18.
set -e
PS5=${1:?usage: make_gl_cube.sh <PS5_IP>}
GLREV=dce74910af7f4274c48d05e0ae001b3038462588            # ps5-opengl commit this was tested against
SDKURL=https://github.com/blackbearreloaded/ps5-opengl/releases/download/v0.3.0/ps5-opengl-sdk-0.3.0.tar.gz
SDKSHA=a7bd6b85f00398eaf8d87ecc58fa0bde7cab79e065acc783268070d2f14b403c
T=$HOME/ps5probe
[ -d $T/.deps/native ] || { echo "$T is missing its .deps (build ~/ps5probe once first)"; exit 1; }

echo "== ps5-opengl sources (pinned commit)"
if [ ! -d $HOME/ps5gl/.git ]; then
  git init -q $HOME/ps5gl && cd $HOME/ps5gl && git remote add origin https://github.com/blackbearreloaded/ps5-opengl.git
  git fetch -q --depth 1 origin $GLREV && git checkout -q FETCH_HEAD
fi
cd $HOME/ps5gl

echo "== prebuilt OpenGL SDK 0.3.0 (240 MB, checksum verified)"
mkdir -p $HOME/glsdk && cd $HOME/glsdk
[ -f sdk.tgz ] || curl -L -C - -o sdk.tgz $SDKURL
echo "$SDKSHA  sdk.tgz" | sha256sum -c - || { echo "download is corrupt: delete ~/glsdk/sdk.tgz and re-run"; exit 1; }
[ -d ps5-opengl-sdk-0.3.0/sdk ] || tar -xzf sdk.tgz
export PS5_OPENGL_PREFIX=$HOME/glsdk/ps5-opengl-sdk-0.3.0/sdk

echo "== pinned sokol sources for the sample"
cd $HOME/ps5gl && mkdir -p third_party
python3 - <<'PY'
import json, os, subprocess
pins = json.load(open("dependencies.json"))["repositories"]
for name in ("sokol", "sokol-samples"):
    p = pins[name]; d = "third_party/" + name
    if os.path.isdir(d + "/.git"): continue
    os.makedirs(d, exist_ok=True)
    g = lambda *a: subprocess.check_output(["git", "-C", d, *a], text=True, stderr=subprocess.STDOUT)
    g("init", "--quiet"); g("remote", "add", "origin", p["url"])
    g("fetch", "--depth=1", "origin", p["revision"]); g("checkout", "--quiet", "--detach", "FETCH_HEAD")
    print(name, "ok")
PY

echo "== build the title (uses ~/ps5probe as the boilerplate template)"
export PS5_NATIVE_APP_TEMPLATE=$T PS5_CLANG=clang-18
bash tools/build-native-test-app.sh egl_public_core33_sokol_cube

echo "== deploy"
make -C $HOME/ps5gl/build/native-app/PPSA99005 deploy PS5_HOST=$PS5
echo "== deployed as PPSA99005. Restart ShadowMountPlus, then launch the new title from the home screen."
echo "   Success = a rotating cube on the TV. The app also writes /download0/ps5-opengl.log on the console."
