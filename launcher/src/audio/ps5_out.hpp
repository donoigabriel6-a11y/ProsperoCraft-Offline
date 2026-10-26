// Launcher audio output: renders the mixer on its own thread into the console's sceAudioOut main port.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The port is closed again when the launcher ends: the game's software OpenAL (ps5al.inc) opens the same port later.

#pragma once

#include "audio/mixer.hpp"

namespace hui::audio
{

class ConsoleOut
{
  public:
    ~ConsoleOut()
    {
        stop();
    }
    // False when the port cannot be opened (the launcher then stays silent). Starts the render thread.
    bool start(Mixer &mixer);
    void stop();

  private:
    void *state_ = nullptr;
};

} // namespace hui::audio
