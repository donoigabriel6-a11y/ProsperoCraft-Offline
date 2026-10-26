// Minecraft's UI sounds for the launcher: decoded from the game's own asset files already on the console.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Nothing of Minecraft is shipped with the launcher. Kit.java writes <root>/.kit/sounds.txt (the newest asset index in <root>/assets/indexes);
// the index maps "minecraft/sounds/random/click.ogg" to a hash and the file lives in <root>/assets/objects/<hh>/<hash>.
// Without an index (nothing downloaded yet) the launcher keeps its synthesized tones.

#pragma once

#include "audio/cues.hpp"

#include <string>

namespace hui::audio
{

// Adds the sounds to `bank` under SoundSet::minecraft; returns how many cues got a recording.
int load_minecraft_sounds(const std::string &root, SoundBank &bank, std::string *report = nullptr);

} // namespace hui::audio
