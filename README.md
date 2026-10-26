# ProsperoCraft

A non-commercial, hobby launcher and runtime for **Minecraft: Java Edition** on PS5 homebrew (a console with homebrew support enabled). The Microsoft/Azure app used for sign-in is registered under the display name **ConsoleCraft**.

## What it is

- A native launcher UI (controller driven) plus a Java runtime so Minecraft: Java Edition can be launched on the console.
- Per-instance settings (version, mods, resolution, render distance and similar options).
- Written and maintained by one developer as a personal project. No ads, no monetization.

## What it does not do

- It does **not** include or distribute Minecraft game files. Game files are downloaded from Mojang's own servers at runtime, as the official launcher does.
- It does **not** bypass ownership checks. Online play requires signing in with a Microsoft account that owns Minecraft: Java Edition.
- It has a local offline mode (username only, no Microsoft services used), the same way the game itself can run offline.

## Microsoft sign-in

Sign-in uses the standard Microsoft device-code flow (scope `XboxLive.signin offline_access`), then the usual chain:

1. Microsoft OAuth device code
2. Xbox Live user token
3. XSTS token
4. `api.minecraftservices.com` `login_with_xbox`
5. Minecraft profile lookup

It is a public client (no client secret). The only thing stored is the player's own refresh token, on their own device.

## Repository layout

```
src/host      Java host (jvm_host.cpp)
src/shim      LWJGL / GLFW / SDL / Vulkan shim (C)
src/java      game launch, mods, sign-in, launcher logic
src/awtlite   minimal AWT        src/lwjgl2   LWJGL 2 port
launcher/     native controller-driven launcher UI (C++)
tools/        build.sh, sync.sh, build + debug scripts
config/       log4j2 and mod option defaults
tests/        sandbox tests (no console needed)
docs/         BUILDING.md, ARCHITECTURE.md
```

Build: see [docs/BUILDING.md](docs/BUILDING.md) (`MSA_CLIENT_ID=<id> bash tools/build.sh <PC ip> <PS5 ip>`). Design notes: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). For AI assistants continuing the work: [docs/ai/HANDOFF.md](docs/ai/HANDOFF.md) and [docs/ai/NEW_CHAT_PROMPT.md](docs/ai/NEW_CHAT_PROMPT.md).

## Status

Work in progress. Working on console: Minecraft Java 1.x up to 26.3 on OpenGL, OpenGL via Zink (shaders) and Vulkan; Fabric / Quilt / Forge / NeoForge mods; launcher with instances, Microsoft sign-in, global memory setting; Zink launcher to Vulkan game without restarting the app.

*Not affiliated with Mojang Studios, Microsoft, or Sony Interactive Entertainment. Minecraft is a trademark of Mojang Synergies AB.*
