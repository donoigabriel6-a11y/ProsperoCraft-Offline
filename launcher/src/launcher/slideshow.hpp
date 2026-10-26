// Background photos for the launcher: a slideshow with a slow pan, L1 / R1 to go back / forward.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The pictures are <assets>/backgrounds/*.jpg (the build upscales and sharpens whatever is put in
// kit_launcher/assets/backgrounds). They are decoded on a worker thread and kept as at most three textures
// (previous, current, next), so a step is instant and the memory stays small.

#pragma once

#include "gfx/draw_list.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace hui::launcher
{

class Slideshow
{
  public:
    Slideshow() = default;
    Slideshow(const Slideshow &) = delete;
    Slideshow &operator=(const Slideshow &) = delete;
    ~Slideshow()
    {
        release();
    }

    // Lists the pictures of a folder (a random one first) and starts the decoder.
    void load(const std::string &dir);
    void release();
    bool active() const
    {
        return !files_.empty();
    }
    // False until the first picture is on the GPU: the caller shows the plain backdrop meanwhile.
    bool ready() const
    {
        return ready_to_draw_;
    }
    // 0..1: fades in once the first picture is on the GPU.
    float appear() const
    {
        return appear_;
    }
    void update(float dt);
    // -1 = previous picture (L1), +1 = next (R1). Ignored with fewer than two pictures.
    void step(int direction);
    void draw(gfx::DrawList &list) const;

  private:
    struct Tex
    {
        int index = -1;
        std::uint32_t tex = 0;
        int w = 0, h = 0;
    };
    struct Transition
    {
        bool running = false;
        bool slide = false; // L1 / R1: the pictures slide; the automatic change cross-fades
        int dir = 1;
        Tex from, to;
        float t = 0.0f, duration = 1.0f;
        float from_age = 0.0f;
    };

    int wrap(int i) const;
    const Tex *cached(int index) const;
    void request(int index);
    void pump();
    void evict();
    void begin(int target, int direction, bool slide);
    void draw_all(gfx::DrawList &list) const;
    void draw_picture(gfx::DrawList &list, const Tex &t, float age, float dx, float alpha, float shade) const;
    static void *worker_entry(void *self);
    void worker();

    std::vector<std::string> files_;
    std::vector<Tex> cache_;
    int current_ = 0;
    float age_ = 0.0f; // seconds the current picture has been shown (the pan and the dwell)
    int want_step_ = 0; // a step waiting for its picture to be decoded
    bool ready_to_draw_ = false;
    float appear_ = 0.0f;
    Transition tr_;

    // decoder handshake (main thread <-> worker)
    void *thread_ = nullptr;
    std::atomic<bool> quit_{false};
    std::atomic<bool> busy_{false};  // a job is set (main -> worker)
    std::atomic<bool> done_{false};  // its result is waiting (worker -> main)
    std::string job_path_;
    int job_index_ = -1;
    std::uint8_t *out_px_ = nullptr;
    int out_w_ = 0, out_h_ = 0;
    std::vector<int> failed_;
};

// Decodes a PNG or JPEG file and uploads it as a texture through the console stage's hook (0 when it fails).
std::uint32_t load_texture_file(const std::string &path, int *width = nullptr, int *height = nullptr);

// The face of a Minecraft skin (64x64 or 64x32 png): the head and its hat layer, scaled up with nearest-neighbour to size x size. 0 when the file is missing or not a skin.
std::uint32_t load_skin_face(const std::string &path, int size = 64);

} // namespace hui::launcher
