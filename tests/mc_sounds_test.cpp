#include "audio/mc_sounds.hpp"
#include "audio/mixer.hpp"
#include <cstdio>
using namespace hui::audio;
int main(int, char **v) {
    SoundBank bank; Mixer mixer; std::string note;
    int n = load_minecraft_sounds(v[1], bank, &note);
    printf("loaded %d: %s\n", n, note.c_str());
    int fails = 0;
    if (n < 6) { puts("FAIL too few"); fails++; }
    if (!bank.has_recording(Cue::select, SoundSet::minecraft)) { puts("FAIL no select"); fails++; }
    if (bank.has_recording(Cue::error, SoundSet::minecraft)) { puts("FAIL error should be tone (no villager in index)"); fails++; }
    CueEvent e; e.cue = Cue::select; bank.play(mixer, SoundSet::minecraft, e);
    CueEvent e2; e2.cue = Cue::error; bank.play(mixer, SoundSet::minecraft, e2);
    static short out[2048]; mixer.render(out, 1024); int peak = 0; for (int i = 0; i < 2048; i++) peak = std::max(peak, abs((int)out[i]));
    printf("peak after render: %d\n", peak); if (peak < 500) { puts("FAIL silent"); fails++; }
    SoundBank empty; int m = load_minecraft_sounds("/nonexistent", empty, &note); printf("no assets: %d (%s)\n", m, note.c_str()); if (m) fails++;
    printf("fails=%d\n", fails); return fails;
}
