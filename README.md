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

## Status

Work in progress.

*Not affiliated with Mojang Studios, Microsoft, or Sony Interactive Entertainment. Minecraft is a trademark of Mojang Synergies AB.*
