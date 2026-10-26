# Building and deploying

Everything is built in WSL (Ubuntu) on a PC, deployed over the network to a PS5 running a homebrew payload loader.

## One-time setup
- ps5-payload-sdk (prospero-clang), the OpenGL SDK 1.0.2, `PS5_Vulkan` + `PS5_VulkanTemplate`, `PS5_Mesa` (headers), JDK source trees for 8/17/21/25. `tools/build/mc_deps.sh`, `make_gl_cube.sh` and `make_zink_obj.sh` fetch / build most of them; run them once, the main build tells you what is missing.
- Build folder: `~/ps5-mc-java` (created by `tools/sync.sh`). Title folder: `~/ps5probe` (from the boilerplate).
- Your own artwork (not in this repo): `icon0.png pic0.png pic0.dds pic1.dds pic2.dds snd0.at9` in one folder (default `/mnt/c/Users/Enzo/Downloads/ps5-mc-java`, override with `ASSETS=`), launcher backgrounds in `~/ps5-mc-java/kit_launcher/assets/backgrounds` and `unknown_pack.png` next to them.

## Every build (one command)
```
cd ~/ProsperoCraft && git pull && MSA_CLIENT_ID=<your Azure app id> bash tools/build.sh <PC ip> <PS5 ip>
```
`tools/build.sh` = `tools/sync.sh` (repo -> flat build folder, never deletes anything there) + forces the host to rebuild + `make_jvm_title.sh <PC> <PS5> jit gl mc launcher sdk100 nativeui vk` (default options; pass your own after the two IPs). The title deploys itself; `NODEPLOY=1` skips the upload.

Variables: `BUILD_DIR` (default `~/ps5-mc-java`), `ASSETS` (brand folder), `MSA_CLIENT_ID` (never stored in the sources: staged into `assets/mc/msa_client_id`; or put `msaClientId=` in launcher.cfg).

## After each console boot
Load the Vulkan control payload before the first build/run: `nc -w 2 <PS5 ip> 9021 < ~/PS5_Vulkan/build/ps5vkctl/ps5vkctl.elf`.

## Logs
The title sends its log over UDP port 9999. On the PC run `tools/debug/listen.ps1` (PowerShell) and open the firewall once with `tools/debug/pc_firewall_check.ps1`. Crash addresses: `tools/debug/resolve_crash.py`, `resolve_samples.py`, `resolve_prof.py` (need the deployed ELF).

## Checks without a console
- C shim: `gcc -w -std=gnu11 -fsyntax-only -DPS5_VK -DPS5_ZINK -I$JAVA_HOME/include -I$JAVA_HOME/include/linux -Itests -I/usr/include src/shim/ps5lwjgl.c` (the `__wrap_sceVideoOut*` / `sceAgcDcb` redefinition errors are normal).
- Launcher C++ (exceptions are disabled in the console build: no try/catch, no `std::stoi`): `cd launcher/src && g++ -std=c++20 -fno-exceptions -fsyntax-only -I. concepts/launcher.cpp`.
- Java: `javac -nowarn -d /tmp/jc src/java/McBoot.java src/java/Launcher.java` (the "package org.lwjgl does not exist" errors are normal without the LWJGL jars).
- `jvm_host.cpp` needs the console headers (pthread_np.h) and cannot be compiled on a plain PC.
- Test suite: `bash tests/run_all.sh` (about 4 minutes).

## Runtime switches (empty files in `<root>/.kit/`, root is `/app0/mc` on the console)
`no_vkdefer` (switch Zink -> Vulkan before the game starts instead of when it opens its window), `no_vkswitch` (no in-process switch: the app restarts for a Vulkan game), `vkprobe` (Vulkan hand-over probe), `vk_once` (written by the launcher: the next start is a Vulkan start).
