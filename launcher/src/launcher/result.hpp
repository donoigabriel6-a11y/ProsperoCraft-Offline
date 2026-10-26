// What the native launcher decided: read by the console stage once the concept finishes.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>

namespace hui::launcher
{

struct Result
{
    bool done = false;
    std::string action; // "play", "install", "mods", "login" or "launcher"
};

inline Result &result()
{
    static Result r;
    return r;
}

// The loading screen that keeps the window alive between "Play" and the game's own window: the console stage fills it from <root>/.kit/boot.txt
// (written by Kit.java: title, stage, last log line), the concept draws it. Both run on the stage thread.
struct Boot
{
    bool on = false;
    bool solid = false; // a resumed Play (the app was restarted for it): the launcher behind has only just started building itself, so the loading screen covers it almost completely
    std::string title, stage, detail;
    float seconds = 0.0f; // time since Play
};

inline Boot &boot()
{
    static Boot b;
    return b;
}

// Texture upload for pictures the launcher reads from disk (world icons). Set by the console stage while it runs; null in the PC preview.
struct TextureHooks
{
    std::uint32_t (*upload)(int width, int height, const std::uint8_t *rgba) = nullptr;
    void (*release)(std::uint32_t texture) = nullptr;
};

inline TextureHooks &textures()
{
    static TextureHooks h;
    return h;
}

} // namespace hui::launcher
