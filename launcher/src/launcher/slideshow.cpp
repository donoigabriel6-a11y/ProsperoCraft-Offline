// Background photos: see slideshow.hpp.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <vector>
#include "launcher/slideshow.hpp"

#include "core/tween.hpp"
#include "launcher/result.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_FAILURE_STRINGS
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#include "gfx/stb_image.h"
#pragma GCC diagnostic pop

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <pthread.h>
#include <unistd.h>

namespace hui::launcher
{

namespace
{
constexpr float kDwell = 14.0f;     // seconds a picture stays
constexpr float kFade = 1.6f;       // the automatic change cross-fades
constexpr float kSlide = 0.85f;     // L1 / R1 slide
constexpr float kPan = 0.075f;      // how far the slow zoom goes in (share of the picture)

bool has_ext(const std::string &n, const char *ext)
{
    const std::size_t l = std::char_traits<char>::length(ext);
    if (n.size() <= l)
        return false;
    for (std::size_t i = 0; i < l; ++i)
        if (std::tolower(static_cast<unsigned char>(n[n.size() - l + i])) != ext[i])
            return false;
    return true;
}

unsigned hash_of(int v)
{
    unsigned h = 2166136261u ^ static_cast<unsigned>(v) * 2654435761u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return h;
}
} // namespace

void Slideshow::load(const std::string &dir)
{
    release();
    if (DIR *d = ::opendir(dir.c_str()))
    {
        while (const dirent *e = ::readdir(d))
        {
            const std::string n = e->d_name;
            if (n.empty() || n[0] == '.')
                continue;
            if (has_ext(n, ".jpg") || has_ext(n, ".jpeg") || has_ext(n, ".png"))
                files_.push_back(dir + "/" + n);
        }
        ::closedir(d);
    }
    std::sort(files_.begin(), files_.end());
    if (files_.empty())
        return;
    current_ = static_cast<int>(static_cast<unsigned>(std::time(nullptr)) % files_.size());   // a different picture each launch
    quit_ = false;
    pthread_t *t = new pthread_t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1u << 20);
    if (pthread_create(t, &attr, worker_entry, this) != 0)
    {
        delete t;
        files_.clear();
        return;
    }
    thread_ = t;
    request(current_);
}

void Slideshow::release()
{
    if (thread_)
    {
        quit_ = true;
        pthread_join(*static_cast<pthread_t *>(thread_), nullptr);
        delete static_cast<pthread_t *>(thread_);
        thread_ = nullptr;
    }
    if (out_px_)
    {
        stbi_image_free(out_px_);
        out_px_ = nullptr;
    }
    const TextureHooks &h = textures();
    if (h.release)
    {
        for (const Tex &t : cache_)
            if (t.tex)
                h.release(t.tex);
    }
    cache_.clear();
    tr_ = {};
    files_.clear();
    failed_.clear();
    busy_ = done_ = false;
    ready_to_draw_ = false;
    appear_ = 0.0f;
    want_step_ = 0;
}

void *Slideshow::worker_entry(void *self)
{
    static_cast<Slideshow *>(self)->worker();
    return nullptr;
}

void Slideshow::worker()
{
    while (!quit_.load())
    {
        if (busy_.load(std::memory_order_acquire) && !done_.load(std::memory_order_acquire))
        {
            int w = 0, h = 0, n = 0;
            out_px_ = stbi_load(job_path_.c_str(), &w, &h, &n, 4);
            out_w_ = out_px_ ? w : 0;
            out_h_ = out_px_ ? h : 0;
            done_.store(true, std::memory_order_release);
        }
        else
            usleep(4000);
    }
}

int Slideshow::wrap(int i) const
{
    const int n = static_cast<int>(files_.size());
    return n == 0 ? 0 : ((i % n) + n) % n;
}

const Slideshow::Tex *Slideshow::cached(int index) const
{
    for (const Tex &t : cache_)
        if (t.index == index)
            return &t;
    return nullptr;
}

void Slideshow::request(int index)
{
    if (busy_.load() || done_.load() || cached(wrap(index)))
        return;
    index = wrap(index);
    if (std::find(failed_.begin(), failed_.end(), index) != failed_.end())
        return;
    job_path_ = files_[static_cast<std::size_t>(index)];
    job_index_ = index;
    busy_.store(true, std::memory_order_release);
}

// Takes a finished decode to the GPU, then asks for whatever the neighbours still lack.
void Slideshow::pump()
{
    if (done_.load(std::memory_order_acquire))
    {
        const TextureHooks &h = textures();
        if (out_px_ && h.upload)
        {
            Tex t;
            t.index = job_index_;
            t.w = out_w_;
            t.h = out_h_;
            t.tex = h.upload(out_w_, out_h_, out_px_);
            if (t.tex)
                cache_.push_back(t);
        }
        else if (!out_px_)
            failed_.push_back(job_index_);
        if (out_px_)
        {
            stbi_image_free(out_px_);
            out_px_ = nullptr;
        }
        busy_.store(false);
        done_.store(false);
        evict();
    }
    if (busy_.load())
        return;
    // the picture a step is waiting for, then the next, then the previous
    if (want_step_ != 0)
        request(current_ + want_step_);
    request(current_);
    request(current_ + 1);
    request(current_ - 1);
}

void Slideshow::evict()
{
    const TextureHooks &h = textures();
    while (cache_.size() > 3)
    {
        std::size_t worst = 0;
        int far = -1;
        for (std::size_t i = 0; i < cache_.size(); ++i)
        {
            const Tex &t = cache_[i];
            if (tr_.running && (t.index == tr_.from.index || t.index == tr_.to.index))
                continue;
            const int n = static_cast<int>(files_.size());
            int d = std::abs(t.index - current_);
            d = std::min(d, n - d);
            if (d > far)
            {
                far = d;
                worst = i;
            }
        }
        if (far < 0)
            break;
        if (cache_[worst].tex && h.release)
            h.release(cache_[worst].tex);
        cache_.erase(cache_.begin() + static_cast<std::ptrdiff_t>(worst));
    }
}

void Slideshow::begin(int target, int direction, bool slide)
{
    const Tex *to = cached(target);
    const Tex *from = cached(current_);
    if (!to || !from)
        return;
    tr_.running = true;
    tr_.slide = slide;
    tr_.dir = direction;
    tr_.from = *from;
    tr_.to = *to;
    tr_.t = 0.0f;
    tr_.duration = slide ? kSlide : kFade;
    tr_.from_age = age_;
    current_ = target;
    age_ = 0.0f;
    want_step_ = 0;
}

void Slideshow::step(int direction)
{
    if (files_.size() < 2 || direction == 0)
        return;
    if (tr_.running)
    {
        // Pressing again while sliding: finish this change at once and take the next.
        tr_.running = false;
    }
    want_step_ = direction > 0 ? 1 : -1;
    const int target = wrap(current_ + want_step_);
    if (cached(target))
        begin(target, direction > 0 ? 1 : -1, false);
}

void Slideshow::update(float dt)
{
    if (files_.empty())
        return;
    pump();
    if (ready_to_draw_ && appear_ < 1.0f)
        appear_ = std::min(1.0f, appear_ + dt / 1.4f);
    if (!ready_to_draw_)
    {
        if (cached(current_))
            ready_to_draw_ = true;
        else if (std::find(failed_.begin(), failed_.end(), current_) != failed_.end())
            current_ = wrap(current_ + 1);   // this picture is unreadable: begin with the next one
        else
            return;
    }
    if (want_step_ != 0 && !tr_.running)
    {
        const int target = wrap(current_ + want_step_);
        if (cached(target))
            begin(target, want_step_, false);
        else if (std::find(failed_.begin(), failed_.end(), target) != failed_.end())
        {
            current_ = target;   // skip the unreadable picture
            want_step_ = 0;
        }
    }
    if (tr_.running)
    {
        tr_.t += dt;
        tr_.from_age += dt;
        if (tr_.t >= tr_.duration)
            tr_.running = false;
    }
    age_ += dt;
    if (!tr_.running && files_.size() > 1 && age_ > kDwell && want_step_ == 0)
    {
        const int target = wrap(current_ + 1);
        if (cached(target))
            begin(target, 1, false);
    }
}

// One picture covering the screen: cropped to 16:9, zoomed in slowly along a path that depends on the picture.
void Slideshow::draw_picture(gfx::DrawList &list, const Tex &t, float age, float dx, float alpha, float shade) const
{
    if (!t.tex || t.w <= 0 || t.h <= 0 || alpha <= 0.003f)
        return;
    const float aspect = static_cast<float>(t.w) / static_cast<float>(t.h);
    const float screen = gfx::kVirtualWidth / gfx::kVirtualHeight;
    float uw = 1.0f, uh = 1.0f;
    if (aspect > screen)
        uw = screen / aspect;
    else
        uh = aspect / screen;
    const float k = tween::smoothstep(age / (kDwell + kFade));
    const float zoom = 1.0f - kPan * k;
    const unsigned hsh = hash_of(t.index);
    const float px0 = static_cast<float>(hsh & 0xff) / 255.0f, py0 = static_cast<float>((hsh >> 8) & 0xff) / 255.0f;
    const float px1 = static_cast<float>((hsh >> 16) & 0xff) / 255.0f, py1 = static_cast<float>((hsh >> 24) & 0xff) / 255.0f;
    const float px = tween::lerp(px0, px1, k), py = tween::lerp(py0, py1, k);
    const float w = uw * zoom, h = uh * zoom;
    const gfx::Rect uv{(1.0f - uw) * 0.5f + (uw - w) * px, (1.0f - uh) * 0.5f + (uh - h) * py, w, h};
    const gfx::Color tint{shade, shade, shade, alpha};
    list.image(t.tex, {dx, 0.0f, gfx::kVirtualWidth, gfx::kVirtualHeight}, uv, tint);
}

void Slideshow::draw(gfx::DrawList &list) const
{
    if (!ready_to_draw_)
        return;
    list.push_opacity(tween::smoothstep(appear_));
    draw_all(list);
    list.pop_opacity();
}

void Slideshow::draw_all(gfx::DrawList &list) const
{
    const Tex *cur = cached(current_);
    if (!tr_.running)
    {
        if (cur)
            draw_picture(list, *cur, age_, 0.0f, 1.0f, 1.0f);
        return;
    }
    const float p = std::min(1.0f, tr_.t / tr_.duration);
    if (tr_.slide)
    {
        // The new picture pushes in over the old one, which drifts away slower and dims (a parallax slide).
        const float e = tween::quint_out(p);
        const float d = static_cast<float>(tr_.dir);
        draw_picture(list, tr_.from, tr_.from_age, -d * gfx::kVirtualWidth * 0.28f * e, 1.0f, 1.0f - 0.45f * e);
        draw_picture(list, tr_.to, age_, d * gfx::kVirtualWidth * (1.0f - e), 1.0f, 1.0f);
    }
    else
    {
        const float e = tween::smoothstep(p);
        draw_picture(list, tr_.from, tr_.from_age, 0.0f, 1.0f, 1.0f);
        draw_picture(list, tr_.to, age_, 0.0f, e, 1.0f);
    }
}

std::uint32_t load_texture_file(const std::string &path, int *width, int *height)
{
    const TextureHooks &h = textures();
    if (!h.upload)
        return 0;
    int w = 0, hh = 0, n = 0;
    std::uint8_t *px = stbi_load(path.c_str(), &w, &hh, &n, 4);
    if (!px)
        return 0;
    const std::uint32_t t = h.upload(w, hh, px);
    stbi_image_free(px);
    if (width)
        *width = w;
    if (height)
        *height = hh;
    return t;
}

std::uint32_t load_skin_face(const std::string &path, int size)
{
    const TextureHooks &h = textures();
    if (!h.upload || size < 8)
        return 0;
    int w = 0, hh = 0, n = 0;
    std::uint8_t *px = stbi_load(path.c_str(), &w, &hh, &n, 4);
    if (!px)
        return 0;
    if (w < 64 || hh < 16 || w % 64 != 0)   // a skin is 64 wide (or a multiple: HD skins), 32 or 64 high
    {
        stbi_image_free(px);
        return 0;
    }
    const int k = w / 64;   // pixels per skin pixel
    std::vector<std::uint8_t> out(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4u);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            const int fx = x * 8 / size, fy = y * 8 / size;   // which of the 8x8 face pixels
            const int sx = (8 + fx) * k + k / 2, sy = (8 + fy) * k + k / 2;
            const int hx = (40 + fx) * k + k / 2;
            const std::uint8_t *base = px + (static_cast<std::size_t>(sy) * static_cast<std::size_t>(w) + static_cast<std::size_t>(sx)) * 4u;
            const std::uint8_t *hat = px + (static_cast<std::size_t>(sy) * static_cast<std::size_t>(w) + static_cast<std::size_t>(hx)) * 4u;
            std::uint8_t *o = out.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x)) * 4u;
            const int a = hat[3];
            for (int ch = 0; ch < 3; ++ch)
                o[ch] = static_cast<std::uint8_t>((hat[ch] * a + base[ch] * (255 - a)) / 255);
            o[3] = 255;
        }
    stbi_image_free(px);
    return h.upload(size, size, out.data());
}

} // namespace hui::launcher
