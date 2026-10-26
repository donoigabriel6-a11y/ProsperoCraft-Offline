# Architecture

| Part | Where | What |
|---|---|---|
| Host | `src/host/jvm_host.cpp` | Starts the static HotSpot (8/17/21/25, chosen from the version), reads `launcher.cfg` / `instance.cfg` (memory, CPUs, JIT threads, GC, renderer), crash reporter, return-to-launcher (`sceSystemServiceLoadExec`). |
| LWJGL / GLFW / SDL shim | `src/shim/ps5lwjgl.c` + `*.inc` | The native side of LWJGL 3.3.3 / 3.4, GLFW and SDL3 on top of the console: window = display, pad -> keyboard/mouse emulation, USB input, audio (OpenAL), JNA libc stand-in, pthread pool guards. `ps5vk.inc` = Vulkan (VK_KHR_display on RADV), the Zink -> Vulkan hand-over and the HUD/cursor overlay. |
| Renderers | `src/shim/ps5gl4es.c`, Mesa Zink | `gl` = console OpenGL SDK, `zink` = OpenGL over Mesa Zink on RADV, `vulkan` = Minecraft's own Vulkan backend (26.2+) or VulkanMod. |
| Java side | `src/java/` | `McBoot` (launch, class loaders, Fabric/Quilt/Forge, renderer hand-over), `McMods` (mod fitting / set-aside), `McAuth` (Microsoft sign-in), `McDownload`, `McForge`, `Launcher` (instances, options), `Kit` (bridge to the native launcher). |
| Launcher UI | `launcher/` | Native C++ controller-driven launcher (GPL-3, based on BlackBearReloaded's Kit). `launcher/src/concepts/launcher.cpp` is the app, `launcher/src/launcher/kit_stage.cpp` runs it inside the game process and keeps the loading screen up while the game starts. |
| AWT / LWJGL 2 | `src/awtlite`, `src/lwjgl2` | Minimal AWT for mods, LWJGL 2 port for old versions. |
| Build | `tools/` | `build.sh` (sync + build), `sync.sh`, `build/make_jvm_title.sh` and helpers, `debug/` log + crash tools. |

## Zink launcher -> Vulkan game in one process ("vkdefer")
The launcher runs on Zink. For a Vulkan game `McBoot.trySwitchToVulkan` marks the run as Vulkan and starts the game while the launcher's loading screen is still on screen. When the game creates its window, `glfwCreateWindow` stops the loading screen, `ps5_vk_switch` tears Mesa's EGL down and destroys Zink's swapchain (the display allows one swapchain), proves a Vulkan swapchain can be made, and the game takes the display. When the game ends the app restarts into the launcher (LWJGL's Vulkan can only be created once per process).

## Memory
One global setting: `xmx=` in `launcher.cfg` (Memory slider in the launcher's Settings, 768-8192 MB, default 3072). `-Xmx` is exactly that; `-Xms` commits up to 4096 MB, the rest grows on demand. The console shares its 16 GB with the GPU, so very large heaps can leave the game short of video memory.
