// Console stage: runs the native launcher in the shim's window, inside the JVM (Kit.run -> kit_stage_run).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The window, EGL context and pad are the shim's (ps5lwjgl.c), so Minecraft's later glfwCreateWindow just
// reuses them, exactly as it does after the Java menu. The UI never calls back into Java: downloads are jobs
// that Kit.java runs on its own thread (launcher::Jobs talks to it through files in <root>/.kit).

#include "app/concept.hpp"
#include "audio/cues.hpp"
#include "audio/mc_sounds.hpp"
#include "audio/mixer.hpp"
#include "audio/ps5_out.hpp"
#include "concepts/concepts.hpp"
#include "core/input.hpp"
#include "core/save_file.hpp"
#include "core/settings.hpp"
#include "demo/catalog.hpp"
#include "gfx/renderer.hpp"
#include "launcher/result.hpp"
#include "launcher/store.hpp"
#include "platform/ps5/system.hpp"
#include "ui/fonts.hpp"

#include <GL/glcorearb.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <pthread.h>
#include <unistd.h>

// ---- shim (ps5lwjgl.c) and host (jvm_host.cpp) ----
extern "C"
{
int glfwInit(void);
void *glfwCreateWindow(int w, int h, const char *title, void *mon, void *share);
void glfwMakeContextCurrent(void *w);
void glfwSwapBuffers(void *w);
void glfwGetFramebufferSize(void *w, int *fw, int *fh);
int ps5_pad_handle(void);
struct Ps5UsbFrame   // ps5usbin.inc
{
    float x, y;
    int active, moved, left_down, left_click, right_click, mid_click, wheel;
    char typed[24];
    int typed_n, backspaces, enter, esc;
    int kbd, hwheel, caret_l, caret_r, home, end, del;
};
std::uint32_t ps5_usb_poll(Ps5UsbFrame *f);   // a USB keyboard / mouse: virtual pad buttons (arrows, Enter, Esc, ...) plus pointer / clicks / wheel / typed text in *f
void ps5_gl_release(void);
extern "C" void ps5_hud_kit(int) __attribute__((weak));
extern "C" void ps5_vk_handover_probe(void) __attribute__((weak));
extern "C" void ps5_host_quit(const char *) __attribute__((weak));
void kit_log(const char *line);
struct KitPadState
{
    std::uint32_t buttons;
    std::uint8_t lx, ly, rx, ry, l2, r2, pad0[2];
    std::uint8_t mid[64];
    std::int32_t connected;
    std::uint8_t tail[40];
};
int scePadReadState(int handle, KitPadState *state);
}

extern "C" int hui_fb_w, hui_fb_h;   // concepts/launcher.cpp: the real framebuffer size (stats overlay)
extern "C" bool hui_sfx_muted;   // concepts/launcher.cpp: launcher.cfg sfx=off

namespace hui::sys
{

std::int64_t monotonic_us()
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<std::int64_t>(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
}

void log(const char *format, ...)
{
    char line[300];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    kit_log(line);
}

void sleep_us(std::uint32_t microseconds)
{
    usleep(microseconds);
}

} // namespace hui::sys

namespace
{

bool load_font(hui::gfx::Renderer &renderer, const std::string &path, hui::gfx::Font *font, hui::ui::FontRef *ref)
{
    std::string data;
    if (!hui::save::read_file(path, &data) || !font->load(data))
    {
        hui::sys::log("[kit] font %s failed", path.c_str());
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

Ps5UsbFrame g_usb_frame;
bool g_kbm_last = false;   // which device spoke last: false = the pad, true = the USB keyboard / mouse

hui::PadSample read_pad(int handle)
{
    hui::PadSample sample;
    KitPadState raw;
    std::memset(&raw, 0, sizeof raw);
    if (handle >= 0 && scePadReadState(handle, &raw) >= 0 && raw.connected)
    {
        sample.buttons = raw.buttons & 0x7fffffffu;
        sample.left_x = raw.lx;
        sample.left_y = raw.ly;
        sample.right_x = raw.rx;
        sample.right_y = raw.ry;
        sample.l2 = raw.l2;
        sample.r2 = raw.r2;
        sample.connected = true;
    }
    {   // the pad speaking (a button or a stick out of its dead zone) hands the launcher back to the pad at once
        const bool stick = std::abs(static_cast<int>(sample.left_x) - 128) > 40 || std::abs(static_cast<int>(sample.left_y) - 128) > 40 ||
                           std::abs(static_cast<int>(sample.right_x) - 128) > 40 || std::abs(static_cast<int>(sample.right_y) - 128) > 40;
        if (sample.connected && ((sample.buttons & 0xffffu) != 0 || (sample.buttons & 0xff0000u) != 0 || stick || sample.l2 > 40 || sample.r2 > 40))
            g_kbm_last = false;
    }
    const std::uint32_t usb = ps5_usb_poll(&g_usb_frame);   // USB keyboard / mouse: arrows = D-pad, Enter = confirm, Esc = back, Tab / PgDn = next page, wheel = up / down, clicks = confirm / back
    if (g_usb_frame.moved || g_usb_frame.left_click || g_usb_frame.right_click || g_usb_frame.mid_click || g_usb_frame.wheel || g_usb_frame.hwheel || g_usb_frame.kbd || g_usb_frame.typed_n)
        g_kbm_last = true;
    if (usb != 0)
    {
        sample.buttons |= usb;
        sample.connected = true;
    }
    sample.timestamp_us = static_cast<std::uint64_t>(hui::sys::monotonic_us());
    return sample;
}

hui::gfx::Renderer *g_renderer = nullptr;
std::uint32_t upload_icon(int w, int h, const std::uint8_t *rgba)
{
    return g_renderer ? g_renderer->batch().create_texture(w, h, rgba) : 0;
}
void release_icon(std::uint32_t texture)
{
    const GLuint t = texture;
    if (t)
        glDeleteTextures(1, &t);
}

// The launcher runs on its own thread (started by kit_stage_run, which waits for Play and returns). After Play that thread keeps the window up as a loading
// screen until the game asks for its own window (the shim calls kit_boot_stop from glfwCreateWindow) or the next launcher start.
std::atomic<int> g_ret{0};          // 0 = the launcher is running, 1 = Play pressed, -1 = it could not start
std::atomic<bool> g_stop{false};    // the game wants the window: leave the loading screen
std::atomic<bool> g_thr_valid{false};
pthread_t g_thr;

// <root>/.kit/boot.txt, written by Kit.java: title, stage, newest game log line.
void read_boot(const std::string &root)
{
    std::string data;
    if (!hui::save::read_file(root + "/.kit/boot.txt", &data))
        return;
    std::string line[3];
    std::size_t at = 0;
    for (int i = 0; i < 3 && at <= data.size(); ++i)
    {
        std::size_t e = data.find('\n', at);
        if (e == std::string::npos)
            e = data.size();
        line[i] = data.substr(at, e - at);
        at = e + 1;
    }
    hui::launcher::Boot &b = hui::launcher::boot();
    b.title = line[0];
    b.stage = line[1];
    b.detail = line[2];
}

// Runs the concept until it finishes. Returns the action ("" if the pad never answered and we gave up).
std::string run_concept(void *window, const std::string &root, const std::string &assets)
{
    using namespace hui;
    gfx::Renderer renderer;
    gfx::Font regular, semibold, display_font;
    ui::Fonts fonts;
    if (!renderer.init() || !load_font(renderer, assets + "/fonts/inter-regular.huifont", &regular, &fonts.regular) ||
        !load_font(renderer, assets + "/fonts/inter-semibold.huifont", &semibold, &fonts.semibold) ||
        !load_font(renderer, assets + "/fonts/montserrat-medium.huifont", &display_font, &fonts.display))
    {
        sys::log("[kit] renderer or fonts failed");
        return "";
    }
    fonts.mono = fonts.pixel = fonts.hand = fonts.regular; // unused by the launcher
    demo::Catalog catalog;                                 // covers are not built: the launcher draws its own art
    app::Telemetry telemetry;
    Settings settings;
    app::Context context{fonts, catalog, telemetry, settings};
    setenv("HUI_LAUNCHER_ROOT", root.c_str(), 1);
    setenv("HUI_ASSETS", assets.c_str(), 1);   // backgrounds/ and unknown_pack.png live next to the fonts
    g_renderer = &renderer;
    launcher::textures() = {upload_icon, release_icon};
    std::unique_ptr<app::Concept> concept_ = concepts::make_launcher(context);
    launcher::result() = {};
    concept_->enter();

    // Sounds: Minecraft's own UI sounds (decoded from the installed assets) through the mixer into the console's audio port; tones when there are none.
    audio::Mixer mixer;
    audio::SoundBank bank;
    audio::ConsoleOut audio_out;
    {
        std::string note;
        const int n = audio::load_minecraft_sounds(root, bank, &note);
        const bool ok = audio_out.start(mixer);
        sys::log("[kit] sound: %s, %d Minecraft cues (%s)", ok ? "output open" : "output NOT available", n, note.c_str());
    }
    InputTracker tracker;
    app::Feedback feedback;
    const int pad = ps5_pad_handle();
    int fw = 1920, fh = 1080;
    glfwGetFramebufferSize(window, &fw, &fh);
    hui_fb_w = fw > 0 ? fw : 1920;
    hui_fb_h = fh > 0 ? fh : 1080;
    if (fw <= 0 || fh <= 0)
        fw = 1920, fh = 1080;
    sys::log("[kit] stage running: pad=%d framebuffer %dx%d root=%s", pad, fw, fh, root.c_str());

    std::int64_t last = sys::monotonic_us();
    std::uint64_t frames = 0;
    double st_sum = 0, st_worst = 0, st_swap = 0; int st_n = 0;   // frame-time probe: the launcher lags after a heavy game exit; mean / worst frame time and the share spent in the swap
    app::Frame frame;
    bool armed = false;
    int idle_frames = 0;
    auto step = [&](bool booting)
    {
        const std::int64_t now = sys::monotonic_us();
        if (frames > 0) { const double ms = (now - last) / 1000.0; st_sum += ms; if (ms > st_worst) st_worst = ms; st_n++; }
        float dt = frames == 0 ? 1.0f / 60.0f : static_cast<float>(now - last) / 1e6f;
        last = now;
        if (dt > 0.05f)
            dt = 0.05f;
        PadSample sample;   // while the loading screen is up the pad is not read: a press must not reach the launcher behind it
        if (!booting)
            sample = read_pad(pad);
        sample.timestamp_us = static_cast<std::uint64_t>(now);
        InputFrame input = tracker.update(std::span<const PadSample>(&sample, 1), static_cast<std::uint64_t>(now));
        if (!booting)
        {
            const Ps5UsbFrame &u = g_usb_frame;
            hui::PointerInput &q = input.ptr;
            q.active = u.active != 0; q.moved = u.moved != 0; q.left_down = u.left_down != 0; q.left_click = u.left_click != 0;
            q.right_click = u.right_click != 0; q.mid_click = u.mid_click != 0; q.x = u.x; q.y = u.y; q.wheel = u.wheel;
            std::memcpy(q.typed, u.typed, sizeof q.typed); q.typed_n = u.typed_n; q.backspaces = u.backspaces; q.enter = u.enter != 0; q.esc = u.esc != 0;
            q.hwheel = u.hwheel; q.caret_l = u.caret_l; q.caret_r = u.caret_r; q.home = u.home; q.end = u.end; q.del = u.del; q.kbm = g_kbm_last;
        }
        if (!booting && !armed)
        {   // a button still held from the game (or a stuck USB key / click) must not press anything here: the launcher listens once everything has been up for a moment
            if (input.held == 0 && !input.ptr.left_down && !input.ptr.enter && input.ptr.typed_n == 0)
                ++idle_frames;
            else
                idle_frames = 0;
            if (idle_frames >= 20)
                armed = true;
            else
            {
                input.pressed = 0; input.nav = Direction::none; input.nav_repeat = false;
                input.ptr.left_click = input.ptr.right_click = input.ptr.mid_click = false;
                input.ptr.enter = input.ptr.esc = false; input.ptr.typed_n = 0; input.ptr.backspaces = 0; input.ptr.wheel = input.ptr.hwheel = 0;
            }
        }
        const std::int64_t p0 = sys::monotonic_us();
        feedback = app::Feedback{};
        concept_->update(input, dt, feedback);
        const std::int64_t p1 = sys::monotonic_us();
        if (!hui_sfx_muted)
            for (const audio::CueEvent &cue : feedback.cues)
                bank.play(mixer, audio::SoundSet::minecraft, cue);

        frame.reset();
        frame.glass_texture = renderer.glass_texture();
        concept_->draw(frame);
        const std::int64_t p2 = sys::monotonic_us();
        renderer.begin();
        renderer.backdrop(frame.backdrop);
        renderer.draw(frame.scene);
        if (frame.glass)
            renderer.glass();
        renderer.draw(frame.overlay);
        renderer.backdrop(frame.post);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        renderer.present(0, fw, fh);
        const std::int64_t sw0 = sys::monotonic_us();
        {   // slow launcher frame (an idle launcher once went from 0.5 to 154 ms of CPU a frame after 19 minutes): where the time goes, first 12 times only
            static int slow_logged;
            if (!booting && sw0 - p0 > 60000 && slow_logged < 12)
            {
                ++slow_logged;
                sys::log("[kit] slow frame #%llu: update %.1f ms, draw %.1f ms, render %.1f ms (frames since start %llu, dt %.3f)", static_cast<unsigned long long>(frames), (p1 - p0) / 1000.0, (p2 - p1) / 1000.0, (sw0 - p2) / 1000.0, static_cast<unsigned long long>(frames), dt);
            }
        }
        glfwSwapBuffers(window); // paced by the swap interval
        st_swap += (sys::monotonic_us() - sw0) / 1000.0;
        if (++frames == 1)
            sys::log("[kit] first frame presented");
        if (!booting && st_n >= 60 && (frames <= 600 || frames % 600 == 0))
        {
            sys::log("[kit] frame time: mean %.1f ms worst %.1f ms (swap %.1f ms) over %d frames, at frame %llu", st_sum / st_n, st_worst, st_swap / st_n, st_n, static_cast<unsigned long long>(frames));
            st_sum = st_worst = st_swap = 0; st_n = 0;
        }
        if (!booting && frames % 600 == 0)
            sys::log("[kit] frames=%llu", static_cast<unsigned long long>(frames));
    };
    bool resumed = false;
    {   // Kit.resume: the Play was pressed before the app restarted (Forge needs other VM options): no launcher screen, straight to the loading screen
        const std::string flag = root + "/.kit/bootonly";
        if (access(flag.c_str(), F_OK) == 0)
        {
            unlink(flag.c_str());
            launcher::result().done = true;
            launcher::result().action = "play";
            resumed = true;
            sys::log("[kit] resumed Play: loading screen only");
        }
    }
    while (!launcher::result().done)
        step(false);
    const std::string action = launcher::result().action;
    sys::log("[kit] done after %llu frames: %s", static_cast<unsigned long long>(frames), action.c_str());
    if (action == "play")
    {
        // Play: Java now boots the game (a minute or more with mods). The window stays up as a loading screen until the game asks for it.
        audio_out.stop(); // frees the audio port for the game
        launcher::boot() = {};
        launcher::boot().on = true;
        launcher::boot().solid = resumed;
        g_ret.store(1);   // Kit.run returns now; this thread carries on
        std::int64_t next_read = 0;
        sys::log("[kit] loading screen up");
        while (!g_stop.load())
        {
            const std::int64_t now = sys::monotonic_us();
            if (now >= next_read)
            {
                read_boot(root);
                next_read = now + 150000;
            }
            step(true);
        }
        sys::log("[kit] loading screen ended after %.1f s: the game takes the window", launcher::boot().seconds);
        launcher::boot() = {};
    }

    // Wait for the buttons to come up so the game's first poll does not see a held Cross as a new press.
    for (int i = 0; i < 90; ++i)
    {
        if ((read_pad(pad).buttons & 0xffffu) == 0 && (read_pad(pad).buttons & 0xff0000u) == 0)
            break;
        usleep(16000);
    }
    audio_out.stop(); // frees the audio port for the game (no-op when the loading screen already did)
    concept_.reset(); // releases its textures while the renderer still exists
    launcher::textures() = {};
    g_renderer = nullptr;
    glFinish();
    return action;
}


struct StageArgs
{
    std::string root, assets;
};

void *stage_thread(void *p)
{
    StageArgs *a = static_cast<StageArgs *>(p);
    int ret = -1;
    if (!glfwInit())
        hui::sys::log("[kit] glfwInit failed");
    else if (void *window = glfwCreateWindow(1920, 1080, "Minecraft", nullptr, nullptr))
    {
        glfwMakeContextCurrent(window);
        std::string action;
        {
            if (ps5_hud_kit) ps5_hud_kit(1); // the launcher draws its own stats overlay: hide the shim's
            action = run_concept(window, a->root, a->assets); // all GL objects of the kit are destroyed at the end of this scope
        }
        if (ps5_hud_kit) ps5_hud_kit(0);
        if (!action.empty() && ps5_vk_handover_probe && access((a->root + "/.kit/vkprobe").c_str(), F_OK) == 0)
        {   // opt-in experiment: can the native Vulkan game take the display after this launcher ran on Zink? (log lines "vkh:"), then the app ends
            unlink((a->root + "/.kit/vkprobe").c_str());
            ps5_vk_handover_probe();
            if (ps5_host_quit)
                ps5_host_quit("vk probe");
            action.clear();
        }
        ps5_gl_release(); // Minecraft's render thread makes the context current next
        if (!action.empty())
        {
            hui::sys::log("[kit] launcher ended cleanly: %s", action.c_str());
            ret = 1;
        }
    }
    else
        hui::sys::log("[kit] no window");
    delete a;
    int expect = 0;
    g_ret.compare_exchange_strong(expect, ret); // 1 is already set when Play started the loading screen
    return nullptr;
}

} // namespace

// Called by the shim's glfwCreateWindow when the game asks for its window, and at the start of every launcher run: ends the loading screen, waits for its thread
// (which destroys its GL objects and releases the context) and returns.
extern "C" void kit_boot_stop(void)
{
    if (!g_thr_valid.load() || pthread_equal(pthread_self(), g_thr))
        return;
    g_stop.store(true);
    pthread_join(g_thr, nullptr);
    g_thr_valid.store(false);
}

// Called from Kit.run() (registered in jvm_host.cpp) on the Java main thread, inside the JVM's lifetime.
// Returns 1 when the player pressed Play (launcher.cfg "instance" is then set), -1 when the UI could not start.
// Kit.java owns the crash marker and falls back to the Java menu on -1 or a native crash on the next boot.
extern "C" int kit_stage_run(const char *root_c, const char *assets_c)
{
    kit_boot_stop(); // a loading screen of an earlier round (the game ended before it opened its window)
    g_ret.store(0);
    g_stop.store(false);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    StageArgs *a = new StageArgs{root_c, assets_c};
    // After a long game session the console has only a few MB of flexible memory left for thread stacks: a 16 MB stack is refused. Take the biggest one that is granted.
    int made = -1;
    for (unsigned mb : {16u, 8u, 6u, 4u, 3u, 2u})
    {
        pthread_attr_setstacksize(&attr, static_cast<std::size_t>(mb) << 20);
        made = pthread_create(&g_thr, &attr, stage_thread, a);
        if (made == 0)
        {
            if (mb != 16u)
                hui::sys::log("[kit] launcher thread: a 16 MB stack was refused, running on %u MB", mb);
            break;
        }
    }
    if (made != 0)
    {
        hui::sys::log("[kit] cannot start the launcher thread");
        delete a;
        return -1;
    }
    g_thr_valid.store(true);
    while (g_ret.load() == 0)
        usleep(5000);
    const int r = g_ret.load();
    if (r < 0)
    {
        pthread_join(g_thr, nullptr);
        g_thr_valid.store(false);
    }
    return r;
}
