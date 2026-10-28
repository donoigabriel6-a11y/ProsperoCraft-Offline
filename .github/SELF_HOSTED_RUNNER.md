# GitHub Actions Self-Hosted Runner Setup for PS5 Build

## Why Self-Hosted?

GitHub-hosted runners **cannot** build PS5 homebrew because:
- PS5 SDK is proprietary (Sony NDA)
- Requires pre-built PS5 JDKs (~30 min each to build)
- Needs OpenGL SDK + LWJGL native compilation
- Final linking requires PS5 toolchain (clang + lld from SDK)
- Deployment needs a PS5 console on the network

## Self-Hosted Runner Requirements

### Hardware
- Linux machine (Ubuntu 22.04+ recommended) or Windows with WSL2
- 16GB+ RAM, 100GB+ free disk
- PS5 console on same network (for deploy step, optional for build-only)

### Software Stack
```bash
# 1. PS5 Payload SDK
git clone https://github.com/ps5-payload-dev/sdk ~/.deps/native/ps5-payload-sdk
cd ~/.deps/native/ps5-payload-sdk && ./build.sh

# 2. OpenGL SDK (1.0.2)
mkdir -p ~/glsdk && cd ~/glsdk
curl -L -O https://github.com/blackbearreloaded/ps5-opengl/releases/download/v1.0.2/ps5-opengl-sdk-1.0.2.tar.gz
tar xzf ps5-opengl-sdk-1.0.2.tar.gz

# 3. Build PS5 JDKs (run once, takes ~30 min each)
# Java 17 (primary)
bash build_jdk.sh 17
# Java 21
bash build_jdk.sh 21
# Java 25
bash build_jdk.sh 25
# Java 8 (optional, for old versions)
bash build_jdk.sh 8

# 4. LWJGL jars (auto-downloaded on first build, or pre-cache)
mkdir -p ~/lwjgl_jars_3.3.3
for j in lwjgl lwjgl-glfw lwjgl-opengl lwjgl-stb; do
  curl -fL -o ~/lwjgl_jars_3.3.3/$j.jar \
    https://repo1.maven.org/maven2/org/lwjgl/$j/3.3.3/$j-3.3.3.jar
done

# 5. Vulkan/Zink (optional, for 'vk' build option)
# See make_mcvk_title.sh for one-time setup
```

### Runner Registration
```bash
# On your build machine:
mkdir actions-runner && cd actions-runner
curl -o actions-runner-linux-x64-2.317.0.tar.gz -L https://github.com/actions/runner/releases/download/v2.317.0/actions-runner-linux-x64-2.317.0.tar.gz
tar xzf ./actions-runner-linux-x64-2.317.0.tar.gz

# Configure with your repo URL and token
./config.sh --url https://github.com/donoigabriel6-a11y/ProsperoCraft-Offline --token <TOKEN> --labels ps5-sdk

# Run as service
sudo ./svc.sh install
sudo ./svc.sh start
```

### Repository Secrets (Settings > Secrets > Actions)
| Secret | Required | Description |
|--------|----------|-------------|
| `MSA_CLIENT_ID` | Yes | Azure App ID for Microsoft sign-in |
| `PS5_HOST` | No | PS5 IP for deploy step |

### Workflow Trigger
The workflow runs on:
- **Manual**: Actions tab → "Build .pkg for PS5" → "Run workflow"
- **Push to main**: Automatic build on every push
- **Tags**: Build on version tags (v*)

### Artifacts Produced
- `ProsperoCraft-PPSA27272.pkg` — Installable PS5 package
- `ProsperoCraft-PPSA27272-exfat.img` — exFAT disk image (manual runs only)

### Download & Install
1. Go to Actions tab → latest run → Artifacts
2. Download `.pkg` (or `.img`)
3. **For .pkg**: Copy to exFAT USB → PS5 Settings → Install Package Files
4. **For .img**: `sudo dd if=ProsperoCraft.img of=/dev/sdX bs=4M conv=fsync`

## Troubleshooting

| Issue | Solution |
|-------|----------|
| "PS5 SDK not found" | Ensure `~/.deps/native/ps5-payload-sdk` exists and was built |
| "JDK not found" | Run `build_jdk.sh` for required versions |
| "LWJGL jars missing" | First build downloads them; pre-cache to speed up |
| "Out of memory" | Increase runner RAM or add swap |
| "Build timeout" | Increase `timeout-minutes` in workflow |

## Cost Considerations
- Self-hosted runners are **free** for public repos
- You pay for the hardware/electricity
- Build takes ~15-30 minutes on modern hardware