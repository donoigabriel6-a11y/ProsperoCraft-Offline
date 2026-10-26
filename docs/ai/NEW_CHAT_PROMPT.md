I'm continuing the ProsperoCraft project (Minecraft Java Edition on a jailbroken PS5: static HotSpot 8/17/21/25, LWJGL/GLFW/SDL shim in C, gl4es / Mesa Zink on RADV / native RADV Vulkan, native C++ "Kit" launcher). The repo is attached or cloned. You have no other state.

1. Read docs/ai/HANDOFF.md first (top block C5 is current), then docs/ARCHITECTURE.md and docs/BUILDING.md.
2. Do not start over or redo settled work (listed in C5.1). Open items are in C5.2.
3. Build with `MSA_CLIENT_ID=<id> bash tools/build.sh <PC ip> <PS5 ip>`. Never hard-code IPs, paths or account ids in the repo.
4. Deliver whole changed files, one combined build command, and say clearly what is console-tested versus only compile-checked. Batch fixes into one build; fixes must be universal across Minecraft versions; check logs with probes, not guesses.
5. Start by telling me in 3-4 lines what you understood, then wait for my first log or request.
