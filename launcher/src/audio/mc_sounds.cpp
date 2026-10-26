// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/mc_sounds.hpp"

#include "core/save_file.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

// stb_vorbis comes with the LWJGL archive the title links (LWJGL's own stb module); a second copy here would be a duplicate symbol. Weak: a build without it stays silent.
extern "C"
{
struct stb_vorbis;
struct stb_vorbis_info
{
    unsigned int sample_rate;
    int channels;
    unsigned int setup_memory_required;
    unsigned int temp_memory_required;
    int max_frame_size;
};
stb_vorbis *stb_vorbis_open_memory(const unsigned char *data, int len, int *error, void *alloc) __attribute__((weak));
stb_vorbis_info stb_vorbis_get_info(stb_vorbis *f) __attribute__((weak));
unsigned int stb_vorbis_stream_length_in_samples(stb_vorbis *f) __attribute__((weak));
int stb_vorbis_get_samples_short_interleaved(stb_vorbis *f, int channels, short *buffer, int num_shorts) __attribute__((weak));
void stb_vorbis_close(stb_vorbis *f) __attribute__((weak));
}

namespace hui::audio
{

namespace
{

struct Pick
{
    Cue cue;
    float gain;
    float pitch;
    const char *files[4]; // first one the index knows wins; paths relative to minecraft/sounds/, without ".ogg"
};

// Which Minecraft sound stands for which launcher cue (https://minecraft.wiki/w/Category:UI_sounds and the game's click / toast / level-up sounds).
// Names differ a little between game versions, so each cue lists alternatives.
constexpr Pick kPicks[] = {
    {Cue::focus, 0.30f, 1.45f, {"random/click", "ui/button/click", nullptr, nullptr}},
    {Cue::select, 0.80f, 1.00f, {"random/click", "ui/button/click", nullptr, nullptr}},
    {Cue::back, 0.55f, 0.80f, {"random/click", "ui/button/click", nullptr, nullptr}},
    {Cue::tab, 0.60f, 1.20f, {"random/click", "ui/button/click", nullptr, nullptr}},
    {Cue::open, 0.55f, 1.10f, {"random/pop", "entity/item/pickup", "random/click", nullptr}},
    {Cue::modal_open, 0.60f, 0.90f, {"random/pop", "entity/item/pickup", "random/click", nullptr}},
    {Cue::modal_close, 0.55f, 0.75f, {"random/pop", "entity/item/pickup", "random/click", nullptr}},
    {Cue::toggle, 0.70f, 1.30f, {"random/click", "ui/button/click", nullptr, nullptr}},
    {Cue::slider, 0.25f, 1.60f, {"random/click", "ui/button/click", nullptr, nullptr}},
    {Cue::error, 0.60f, 1.00f, {"mob/villager/no1", "entity/villager/no1", "mob/villager/no2", "entity/villager/no2"}},
    {Cue::notify, 0.70f, 1.00f, {"ui/toast/in", "random/orb", "entity/experience_orb/pickup", nullptr}},
    {Cue::launch, 0.70f, 1.00f, {"random/levelup", "entity/player/levelup", nullptr, nullptr}},
    {Cue::saved, 0.55f, 1.00f, {"random/orb", "entity/experience_orb/pickup", nullptr, nullptr}},
    {Cue::welcome, 0.60f, 1.00f, {"ui/toast/in", "random/orb", nullptr, nullptr}},
    {Cue::complete, 0.70f, 1.00f, {"ui/toast/challenge_complete", "random/levelup", "entity/player/levelup", nullptr}},
    {Cue::type, 0.35f, 1.40f, {"random/click", "ui/button/click", nullptr, nullptr}},
    {Cue::erase, 0.40f, 0.70f, {"random/click", "ui/button/click", nullptr, nullptr}},
    {Cue::tick, 0.20f, 1.50f, {"random/click", "ui/button/click", nullptr, nullptr}},
};

// "key": {"hash": "<40 hex>"  -> the hash, or "" when the index has no such key.
std::string find_hash(const std::string &json, const std::string &key)
{
    const std::string needle = "\"" + key + "\"";
    const std::size_t at = json.find(needle);
    if (at == std::string::npos)
        return "";
    const std::size_t h = json.find("\"hash\"", at);
    if (h == std::string::npos || h - at > 200)
        return "";
    const std::size_t q1 = json.find('"', json.find(':', h) + 1);
    const std::size_t q2 = q1 == std::string::npos ? q1 : json.find('"', q1 + 1);
    if (q2 == std::string::npos || q2 - q1 != 41)
        return "";
    return json.substr(q1 + 1, 40);
}

// Ogg Vorbis -> interleaved stereo float at 48 kHz (linear resampling), at most max_seconds long.
bool decode_ogg(const std::string &data, std::vector<float> *out, std::size_t *frames, float max_seconds)
{
    if (!stb_vorbis_open_memory || !stb_vorbis_get_info || !stb_vorbis_stream_length_in_samples || !stb_vorbis_get_samples_short_interleaved || !stb_vorbis_close)
        return false;
    int error = 0;
    stb_vorbis *vorbis = stb_vorbis_open_memory(reinterpret_cast<const unsigned char *>(data.data()), static_cast<int>(data.size()), &error, nullptr);
    if (!vorbis)
        return false;
    const stb_vorbis_info vi = stb_vorbis_get_info(vorbis);
    const int channels = vi.channels, rate = static_cast<int>(vi.sample_rate);
    int n = static_cast<int>(std::min<unsigned>(stb_vorbis_stream_length_in_samples(vorbis), 48000u * 6u));
    short *pcm = nullptr;
    if (n > 0 && channels >= 1 && channels <= 8 && rate >= 8000)
    {
        pcm = static_cast<short *>(std::malloc(sizeof(short) * static_cast<std::size_t>(n) * channels));
        if (pcm)
        {
            const int got = stb_vorbis_get_samples_short_interleaved(vorbis, channels, pcm, n * channels);
            if (got <= 0)
            {
                std::free(pcm);
                pcm = nullptr;
            }
            else
                n = got;
        }
    }
    stb_vorbis_close(vorbis);
    if (!pcm)
        return false;
    const double ratio = 48000.0 / rate;
    std::size_t total = static_cast<std::size_t>(n * ratio);
    total = std::min<std::size_t>(total, static_cast<std::size_t>(max_seconds * 48000.0f));
    out->assign(total * 2, 0.0f);
    for (std::size_t i = 0; i < total; ++i)
    {
        const double pos = i / ratio;
        const std::size_t a = std::min<std::size_t>(static_cast<std::size_t>(pos), static_cast<std::size_t>(n - 1));
        const std::size_t b = std::min<std::size_t>(a + 1, static_cast<std::size_t>(n - 1));
        const float f = static_cast<float>(pos - std::floor(pos));
        for (int c = 0; c < 2; ++c)
        {
            const int ch = channels == 1 ? 0 : c;
            const float x = pcm[a * channels + ch] / 32768.0f, y = pcm[b * channels + ch] / 32768.0f;
            (*out)[i * 2 + c] = x + (y - x) * f;
        }
    }
    // A short fade-out so a clip cut at max_seconds never clicks.
    const std::size_t fade = std::min<std::size_t>(total, 480);
    for (std::size_t i = 0; i < fade; ++i)
    {
        const float g = static_cast<float>(i) / static_cast<float>(fade);
        (*out)[(total - 1 - i) * 2] *= g;
        (*out)[(total - 1 - i) * 2 + 1] *= g;
    }
    *frames = total;
    std::free(pcm);
    return true;
}

} // namespace

int load_minecraft_sounds(const std::string &root, SoundBank &bank, std::string *report)
{
    std::string note;
    int loaded = 0;
    std::string pointer;
    if (!save::read_file(root + "/.kit/sounds.txt", &pointer, 4096))
    {
        note = "no .kit/sounds.txt";
    }
    else
    {
        while (!pointer.empty() && (pointer.back() == '\n' || pointer.back() == '\r' || pointer.back() == ' '))
            pointer.pop_back();
        std::string json;
        if (pointer.empty() || !save::read_file(pointer, &json, 8u << 20))
        {
            note = "no asset index yet (download a Minecraft version first)";
        }
        else
        {
            std::string base = pointer;
            const std::size_t cut = base.rfind("/indexes/");
            base = cut == std::string::npos ? root + "/assets" : base.substr(0, cut);
            for (const Pick &pick : kPicks)
            {
                for (const char *file : pick.files)
                {
                    if (!file)
                        break;
                    const std::string hash = find_hash(json, std::string("minecraft/sounds/") + file + ".ogg");
                    if (hash.empty())
                        continue;
                    std::string ogg;
                    if (!save::read_file(base + "/objects/" + hash.substr(0, 2) + "/" + hash, &ogg, 2u << 20))
                        continue;
                    std::vector<float> samples;
                    std::size_t frames = 0;
                    if (!decode_ogg(ogg, &samples, &frames, 2.5f))
                        continue;
                    bank.add(SoundSet::minecraft, pick.cue, std::move(samples), frames, pick.gain, pick.pitch);
                    ++loaded;
                    break;
                }
            }
            note = std::to_string(loaded) + " cues from " + pointer;
        }
    }
    if (report)
        *report = note;
    return loaded;
}

} // namespace hui::audio
