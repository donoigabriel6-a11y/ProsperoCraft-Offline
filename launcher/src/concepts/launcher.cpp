// Minecraft launcher for the PS5: Aurora-style home plus an instance settings sheet.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Structure (the point of the conversion):
//   - launcher::Store reads and writes the same files as Launcher.java
//     (launcher.cfg, instances/<id>/instance.cfg, versions/*), so the native UI
//     and the Java screens share state without any IPC.
//   - Home: hero for the focused entry, a shelf of instance cards, a shelf of tools.
//   - Cross on an instance = Play, Square = settings sheet (ui::Form, frosted).
//   - Tools are pages of this UI: Game Versions (Fabric, Quilt), Content (Modrinth search
//     and install), Accounts, Settings. Downloads run as jobs in the Java side (launcher::Jobs, files in
//     <root>/.kit) while this UI stays up and shows the progress. In the PC preview a simulation answers.

#include "concepts/concepts.hpp"

extern "C" int ps5_hud_hz(void) __attribute__((weak));
extern "C" { bool hui_sfx_muted = false; int hui_fb_w = 1920, hui_fb_h = 1080; }   // hui_fb_*: the real framebuffer size, set by kit_stage.cpp (stats overlay)   // launcher.cfg sfx=off: the stage (kit_stage.cpp) skips every UI sound
extern "C" int ps5_cursor_pixels(unsigned char *rgba, int cap) __attribute__((weak));   // ps5lwjgl.c: the game's pointer sprite (w << 16 | h)
extern "C" void ps5_usb_set_text_mode(int on) __attribute__((weak));   // ps5usbin.inc: letters typed on a USB keyboard are text, not buttons
extern "C" int ps5_running_jdk(void) __attribute__((weak));
extern "C" int ps5_running_renderer(void) __attribute__((weak));   // jvm_host.cpp (merged title): 0 Vulkan, 1 OpenGL (SDK), 2 OpenGL via Zink
extern "C" void ps5_set_hz(int hz) __attribute__((weak));       // ps5lwjgl.c (merged title): refresh rate 60|120, takes effect at once
extern "C" int ps5_renderer_have(int r) __attribute__((weak));     // 1 when this build carries that renderer
extern "C" int ps5_java_for(int forced, int need) __attribute__((weak));   // jvm_host.cpp: the Java a start boots for a version needing `need` with launcher.cfg jdk=<forced>
extern "C" int ps5_java_need_of(const char *version_id) __attribute__((weak));   // jvm_host.cpp: the Java a version asks for (0 = unknown)
extern "C" int ps5_java_expect(int forced) __attribute__((weak));   // jvm_host.cpp: the Java the next start boots for launcher.cfg jdk=<forced> (0 = automatic)   // jvm_host.cpp: the Java of this run

#include "core/tween.hpp"
#include "launcher/jobs.hpp"
#include "launcher/result.hpp"
#include "launcher/slideshow.hpp"
#include "launcher/store.hpp"
#include "ui/components/form.hpp"
#include "ui/components/keyboard.hpp"
#include "ui/glyphs.hpp"
#include "ui/motion.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <string>
#include <vector>

namespace hui::concepts
{

namespace
{

using gfx::Color;
using gfx::Rect;

const Color kWhite = Color::rgb(0xffffff);

constexpr float kMargin = 96.0f;
constexpr float kCard = 196.0f;
constexpr float kCardGap = 26.0f;
constexpr float kCardGrow = 1.2f;
constexpr float kShelfY = 730.0f;
constexpr float kShelfPitch = 304.0f;

constexpr const char *kTechniques[] = {
    "launcher::Store: the same key=value files as Launcher.java (instances, versions, launcher.cfg)",
    "Aurora home: backdrop eases to the focused entry's palette, hero cross-fades on focus",
    "Frosted settings sheet built from ui::Form (sliders, steppers, choices, danger action)",
    "Game Versions and Content pages with an on-screen keyboard; downloads run as background jobs with live progress",
};

constexpr app::TourStep kTour[] = {
    {0.6f, 0, Direction::none, "home"},
    {0.3f, 0, Direction::right},
    {0.3f, 0, Direction::right, "shelf"},
    {0.5f, 0, Direction::down},
    {0.6f, 0, Direction::none, "tools"},
    {0.3f, action_bit(Action::confirm)},
    {2.0f, 0, Direction::none, "install"},
    {0.3f, action_bit(Action::back)},
    {0.3f, 0, Direction::right},
    {0.3f, action_bit(Action::confirm)},
    {2.0f, 0, Direction::none, "mods"},
    {0.3f, action_bit(Action::west)},
    {1.0f, 0, Direction::none, "keyboard"},
    {0.3f, action_bit(Action::back)},
    {0.5f, 0, Direction::none},
    {0.3f, action_bit(Action::confirm)},
    {1.3f, 0, Direction::none, "progress"},
    {2.4f, 0, Direction::none, "after"},
    {0.3f, action_bit(Action::back)},
    {0.5f, 0, Direction::none},
    {0.3f, action_bit(Action::back)},
    {0.3f, 0, Direction::up},
    {0.3f, action_bit(Action::west)},
    {0.9f, 0, Direction::none, "settings"},
    {0.3f, 0, Direction::down},
    {0.3f, 0, Direction::down},
    {0.3f, 0, Direction::down},
    {0.8f, 0, Direction::none, "settings2"},
    {0.3f, 0, Direction::down},
    {0.3f, 0, Direction::down},
    {0.3f, 0, Direction::down},
    {0.3f, 0, Direction::down},
    {0.3f, 0, Direction::down},
    {0.3f, 0, Direction::down},
    {0.6f, 0, Direction::none, "settings-danger"},
    {0.3f, action_bit(Action::confirm)},
    {0.9f, 0, Direction::none, "confirm"},
    {0.3f, action_bit(Action::back)},
    {0.5f, 0, Direction::none},
    {0.3f, action_bit(Action::back)},
    {0.6f, 0, Direction::none},
    {0.3f, 0, Direction::down},
    {0.5f, 0, Direction::right},
    {0.3f, action_bit(Action::confirm)},
    {1.0f, 0, Direction::none, "installed"},
    {0.3f, 0, Direction::down},
    {0.6f, 0, Direction::none, "installed-row"},
    {0.3f, action_bit(Action::west)},
    {1.5f, 0, Direction::none, "installed-switch"},
    {0.3f, action_bit(Action::confirm)},
    {0.9f, 0, Direction::none, "installed-ask"},
    {0.3f, action_bit(Action::confirm)},
    {0.4f, 0, Direction::none, "installed-working"},
    {1.6f, 0, Direction::none, "installed-after"},
    {0.3f, action_bit(Action::back)},
};

enum class Mode
{
    settings,
    install,
    mods,
    launcher,
    account,
    installed
};

enum Field
{
    kStart = 30,
    kInfo1,
    kInfo2,
    kInfo3,
    kInfo4,
    kInfo5,
    kName = 1,
    kVersion,
    kLoader,
    kMemory,
    kCpus,
    kJit,
    kRenderDist,
    kFov,
    kPadEmu,
    kGc,
    kJdk,
    kRenderer,
    kAutoplay,
    kRefresh,
    kMerge,
    kVsync,
    kSfx,
    kCursor,
    kGlThread,
    kAccount,
    kResolution,
    kDelete,
    kSignIn,
    kOffline,
    kSignOut,
    kUse0,
    kUse1,
    kUse2,
    kUse3,
    kOnExit,
    kHud,
    kHudPos,
    kHudBg,
    kUsername,
};

enum class Kind
{
    instance,
    create,
    tool
};

struct Entry
{
    Kind kind = Kind::instance;
    int instance = -1; // index into Store::instances
    std::string title;
    std::string meta;
    std::string badge; // big label on the card
    std::string blurb;
    Color dark, mid, accent;
    int tool = 0;
};

struct Shelf
{
    const char *title;
    std::vector<Entry> items;
    int column = 0;
    ui::Scroller scroll;
};

// A rectangle that glides to its target with a little overshoot: the focus ring behaves like a drop of liquid.
struct BounceRect
{
    tween::Bounce x, y, w, h;
    void snap(const Rect &r)
    {
        x.snap(r.x);
        y.snap(r.y);
        w.snap(r.w);
        h.snap(r.h);
    }
    void target(const Rect &r)
    {
        x.target = r.x;
        y.target = r.y;
        w.target = r.w;
        h.target = r.h;
    }
    void update(float dt, float omega, float damping)
    {
        x.update(dt, omega, damping);
        y.update(dt, omega, damping);
        w.update(dt, omega, damping);
        h.update(dt, omega, damping);
    }
    Rect value() const
    {
        return {x.value, y.value, w.value, h.value};
    }
};

unsigned hash(const std::string &s)
{
    unsigned h = 2166136261u;
    for (char c : s)
        h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
    return h;
}

using launcher::loader_kind;
using launcher::game_version; // "fabric-loader-0.19.5-1.21.1" -> "1.21.1"

// One line of the Installed page: a version, a mod, a resource pack or a shader pack.
struct InstalledRow
{
    std::string title, sub;
    std::vector<std::pair<std::string, Color>> tags;
    std::string remove;  // job command that deletes it
    std::string why;     // non-empty: it cannot be deleted right now, and why
    std::string toggle;  // job command that switches a mod on or off ("" for everything else)
    bool off = false;    // the mod is switched off
    std::string detail;  // right column
    // worlds
    bool world = false;
    std::string folder;       // the world's folder name (what jobs need)
    std::string name;         // the name shown in Minecraft's world list
    std::uint32_t tex = 0;    // the world's picture (icon.png), 0 = none
    std::string when, facts;  // right column: last played, version and mode
    Color tint = Color::rgb(0x6fe6a3);
};

class Launcher final : public app::Concept
{
  public:
    explicit Launcher(app::Context &context) : context_(context)
    {
        const char *root = std::getenv("HUI_LAUNCHER_ROOT");
        store_ = (root && *root) ? launcher::Store::load(root) : launcher::Store::demo();
        jobs_ = launcher::Jobs((root && *root) ? root : "");
        if (const char *assets = std::getenv("HUI_ASSETS"); assets && *assets)
        {
            bg_.load(std::string(assets) + "/backgrounds");
            unknown_tex_ = launcher::load_texture_file(std::string(assets) + "/unknown_pack.png");
        }
        keyboard_.style.bindings = ui::KeyboardBindings::standard();
        keyboard_.style.bindings.done = Action::menu; // Options = Done, like the console's own keyboard
        keyboard_.style.bindings.shift = Action::jump_prev;   // L2 = Shift / Caps Lock (hold: capitals while held, tap: next letter, tap twice: lock), like the console's keyboard
        keyboard_.style.bindings.layout = Action::r3;         // R3 = letters / symbols
        keyboard_.style.surfaces = true;
        keyboard_.on_text = [this](std::string_view t) { kb_caret_ = std::min(kb_caret_, kb_text_.size()); kb_text_.insert(kb_caret_, std::string(t)); kb_caret_ += t.size(); };
        keyboard_.on_backspace = [this] { kb_backspace(); };
        for (const ui::Theme &t : ui::themes())
            if (std::string(t.id) == "acrylic")
                theme_ = t;
        theme_.omega = 22.0f;     // the sliding highlights of the form rows overshoot a little: liquid, not mechanical
        theme_.damping = 0.62f;
        keyboard_.style.theme = theme_;
        rebuild();
        shown_ = previous_ = focused_index();
        apply_palette(true);
        ring_.snap(card_rect(0, 0, true));
        const int cur = store_.find(store_.current);
        if (cur >= 0)
            shelves_[0].column = cur;
        announce_last_exit(root);
    }

    ~Launcher() override
    {
        release_icons();
        bg_.release();
        if (unknown_tex_ && launcher::textures().release)
            launcher::textures().release(unknown_tex_);
        if (face_tex_ && launcher::textures().release)
            launcher::textures().release(face_tex_);
    }

    // jvm_host.cpp leaves <root>/.kit/lastexit.txt ("exit 0", "exit 1", "crash 11", "abort") when the title restarted itself after Minecraft ended.
    void announce_last_exit(const char *root)
    {
        if (!root || !*root)
            return;
        const std::string path = std::string(root) + "/.kit/lastexit.txt";
        char line[96] = {0};
        if (std::FILE *f = std::fopen(path.c_str(), "r"))
        {
            if (!std::fgets(line, sizeof line, f))
                line[0] = 0;
            std::fclose(f);
            std::remove(path.c_str());
        }
        const std::string what = line;
        if (what.rfind("exit 0", 0) == 0)
            status("Minecraft closed. Welcome back.");
        else if (what.rfind("exit", 0) == 0)
            status("Minecraft ended with an error (code " + what.substr(5, what.find('\n') - 5) + "). The log has details.");
        else if (what.rfind("freeze", 0) == 0)
            status("Minecraft froze, so the app restarted itself. Try the Zink/GL option or fewer mods if it repeats.");
        else if (what.rfind("crash", 0) == 0 || what.rfind("abort", 0) == 0)
            status("Minecraft stopped unexpectedly. Check the log if it keeps happening.");
    }

    const app::ConceptInfo &info() const override
    {
        static const app::ConceptInfo kInfo{
            "launcher",
            "Minecraft Launcher",
            "Instance shelf, one-press Play, frosted instance settings",
            "src/concepts/launcher.cpp",
            audio::SoundSet::glass,
            Color::rgb(0x7ff0d8),
            kTechniques,
        };
        return kInfo;
    }

    void enter() override
    {
        age_ = 0.0f;
        sheet_open_ = false;
        cur_scale_ = static_cast<float>(2 + cursor_index(store_.config("cursor")));
        if (cur_px_.empty())
            load_pointer_art();
        hui_sfx_muted = store_.config("sfx") == "off";
    }

    void update(const InputFrame &raw_input, float dt, app::Feedback &feedback) override
    {
        age_ += dt;
        last_dt_ = dt;
        if (dt > 0.0f)
            hud_fps_ += (1.0f / dt - hud_fps_) * std::min(1.0f, dt * 2.0f);   // smoothed over about half a second
        clock_ += dt;
        feedback_ = &feedback;
        jobs_.update(dt);
        bg_.update(dt);
        if (launcher::boot().on)   // Play was pressed: the window stays up as a loading screen until the game takes it over
        {
            boot_t_ = launcher::boot().solid ? 1.0f : std::min(1.0f, boot_t_ + dt * 3.0f);
            launcher::boot().seconds += dt;
            return;
        }
        const InputFrame input = pointer_frame(raw_input, feedback);   // USB mouse: hover moves the focus, clicks press confirm / back, the wheel scrolls
        if (jobs_.take_finished())
            on_job_done();
        if (signed_in_now() && !jobs_.busy() && face_tex_ == 0 && skin_asked_ != store_.account && !store_.root.empty())
        {   // the profile picture: fetched once per account when its skin file is missing
            skin_asked_ = store_.account;
            if (std::FILE *f = std::fopen((store_.root + "/launcher/skin-" + store_.account + ".png").c_str(), "rb"))
                std::fclose(f);
            else
                jobs_.submit("skin " + store_.account);
        }
        if (!pending_cmd_.empty() && !jobs_.busy())   // a setting changed while a download ran: it goes out when the downloader is free
        {
            const std::string c = pending_cmd_;
            pending_cmd_.clear();
            jobs_.submit(c);
        }
        if (search_pending_ && sheet_open_ && mode_ == Mode::mods && !jobs_.busy())
            run_search();
        if (!info_pending_.empty() && sheet_open_ && mode_ == Mode::installed && !jobs_.busy() && jobs_.submit("info " + info_pending_))
        {
            info_loading_ = info_pending_;
            info_pending_.clear();
        }
        if (opt_pending_ && rd_form_ && sheet_open_ && mode_ == Mode::settings && !jobs_.busy() && jobs_.submit("info options"))
        {
            info_loading_ = "options";
            opt_pending_ = false;
        }
        if (error_open_)
            update_error(input, feedback);
        else if (confirm_open_)
            update_confirm(input, feedback);
        else if (kb_open_)
            update_keyboard(input, dt, feedback);
        else if (sheet_open_ && mode_ == Mode::install)
            update_install(input, feedback);
        else if (sheet_open_ && mode_ == Mode::mods)
            update_mods(input, feedback);
        else if (sheet_open_ && mode_ == Mode::installed)
            update_installed(input, feedback);
        else if (sheet_open_)
            update_sheet(input, dt, feedback);
        else
            update_shelves(input, feedback);
        kb_t_.target = kb_open_ ? 1.0f : 0.0f;
        kb_t_.update(dt, 16.0f);
        err_t_.target = error_open_ ? 1.0f : 0.0f;
        err_t_.update(dt, 16.0f);
        conf_t_.target = confirm_open_ ? 1.0f : 0.0f;
        conf_t_.update(dt, 16.0f);
        install_scroll_.update(dt, 14.0f);
        mods_scroll_.update(dt, 14.0f);
        inst_scroll_.update(dt, 14.0f);

        if (focused_index() != shown_ || focus_row_ != shown_row_)
        {
            previous_ = shown_;
            previous_row_ = shown_row_;
            shown_ = focused_index();
            shown_row_ = focus_row_;
            hero_.start(context_.settings.reduced_motion ? 0.12f : 0.42f);
            apply_palette(false);
        }
        hero_.update(dt);
        for (ui::SpringColor &c : palette_)
            c.update(dt, 4.0f);
        row_position_.target = static_cast<float>(focus_row_);
        row_position_.update(dt, 11.0f);
        for (int r = 0; r < 2; ++r)
        {
            Shelf &shelf = shelves_[static_cast<std::size_t>(r)];
            const float start = static_cast<float>(shelf.column) * (kCard + kCardGap);
            shelf.scroll.reveal(start, start + kCard * kCardGrow, gfx::kVirtualWidth - kMargin,
                                kMargin * 2.2f);
            shelf.scroll.update(dt, 12.0f);
        }
        ring_.target(card_rect(focus_row_, shelves_[static_cast<std::size_t>(focus_row_)].column, true));
        ring_.update(dt, context_.settings.reduced_motion ? 40.0f : 23.0f, context_.settings.reduced_motion ? 1.0f : 0.66f);
        nudge_.update(dt, 9.0f);
        sheet_t_.target = sheet_open_ ? 1.0f : 0.0f;
        sheet_t_.update(dt, context_.settings.reduced_motion ? 40.0f : 13.0f);
        sheet_pop_.target = sheet_open_ ? 1.0f : 0.0f;
        if (context_.settings.reduced_motion)
            sheet_pop_.snap(sheet_pop_.target);
        else
            sheet_pop_.update(dt, 17.0f, 0.55f);
        update_glass_ui(dt);
        play_.update(dt, 6.0f);
        if (finish_left_ > 0.0f && (finish_left_ -= dt) <= 0.0f)
        {
            launcher::result().action = finish_action_;
            launcher::result().done = true;
        }
        if (status_age_ < 10.0f)
            status_age_ += dt;
    }

    void draw(app::Frame &frame) const override
    {
        glass_tex_ = frame.glass_texture;
        act_hits_.clear();
        const float photo = bg_.ready() ? tween::smoothstep(bg_.appear()) : 0.0f;
        if (photo < 0.999f)   // no pictures (or still decoding the first): the animated aurora, which also fades out under a picture
        {
            frame.backdrop.mode = gfx::BackdropMode::aurora;
            for (int i = 0; i < 4; ++i)
                frame.backdrop.colors[i] = palette_[i].value();
            frame.backdrop.time = clock_;
        }
        if (photo > 0.001f)
        {
            bg_.draw(frame.scene);
            draw_scrim(frame.scene, photo);
        }
        frame.glass = true;   // everything above is blurred into the glass texture: the panels drawn next frost it

        gfx::DrawList &list = frame.overlay;
        const float back = sheet_t_.value;
        list.push_transform(1.0f - 0.035f * back, 960, 540, 0, 0);
        list.push_opacity(1.0f - 0.72f * back);   // the home screen sinks behind a sheet
        draw_top_bar(list);
        draw_hero(list);
        draw_shelves(list);
        list.pop_opacity();
        list.pop_transform();
        if (back > 0.01f)
            list.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0,
                              Color::rgb(0x05070f, 0.45f * back));
        if (back > 0.01f)
        {
            const float pop = 0.955f + 0.045f * sheet_pop_.value;   // the sheet settles into place with a small overshoot
            list.push_transform(pop, 960, 508, 0, 0);
            draw_sheet(list, glass_tex_);
            list.pop_transform();
        }
        draw_notice(list);
        if (kb_t_.value > 0.01f)
            draw_keyboard(list, glass_tex_);
        if (conf_t_.value > 0.01f)
            draw_confirm(list);
        if (err_t_.value > 0.01f)
            draw_error(list);
        if (boot_t_ < 0.5f)
            draw_hints(list);
        if (boot_t_ > 0.001f)
            draw_boot(list);
        else
            draw_pointer(list);
        draw_hud(list);
    }

    // The loading screen between Play and the game's window: what is happening (stage), the newest line of the game's log, the time so far.
    // Nothing is cut with "...": long lines wrap, a long title shrinks.
    static constexpr float kHudBgAlpha[6] = {0.0f, 0.2f, 0.4f, 0.55f, 0.72f, 0.92f};
    int hud_bg_index() const
    {
        const std::string c = store_.config("hudbg");
        return c.size() == 1 && c[0] >= '0' && c[0] <= '5' ? c[0] - '0' : 4;
    }
    int hud_pos_index() const
    {
        static const char *const pos[6] = {"tl", "tc", "tr", "bl", "bc", "br"};
        const std::string c = store_.config("hudpos");
        for (int i = 0; i < 6; ++i)
            if (c == pos[i])
                return i;
        return 0;
    }

    // The stats overlay (settings > Stats overlay): FPS with V-Sync, resolution with refresh rate, renderer. Sizes follow the text, so nothing is cut or overlaps.
    void draw_hud(gfx::DrawList &list) const
    {
        if (store_.config("hud") != "on")
            return;
        const ui::Fonts &fonts = context_.fonts;
        const int rr = ps5_running_renderer ? ps5_running_renderer() : 2;
        const std::string hzc = store_.config("hz");
        const int hz = ps5_hud_hz ? ps5_hud_hz() : (hzc == "60" ? 60 : 120);   // what the display really runs, not what the setting asks for
        char l1[48], l2[64];
        std::snprintf(l1, sizeof l1, "FPS: %d (VSync On)", static_cast<int>(hud_fps_ + 0.5f));
        std::snprintf(l2, sizeof l2, "Resolution: %dx%d %dHz", hui_fb_w, hui_fb_h, hz);
        const std::string l3 = std::string("Renderer: ") + (rr == 0 ? "Vulkan" : rr == 1 ? "OpenGL" : "Zink");
        const std::string ls[3] = {l1, l2, l3};
        const float size = 21.0f, lh = 30.0f, pad = 16.0f, bar = 5.0f;
        float tw = 0;
        for (const std::string &s : ls)
            tw = std::max(tw, fonts.semibold.font->measure(s.c_str(), size));
        const float w = tw + pad * 2 + bar, h = lh * 3 + pad * 2 - 6.0f;
        const int pos = hud_pos_index(), col = pos % 3;
        const float m = 28.0f;
        const float x = col == 0 ? m : col == 1 ? (gfx::kVirtualWidth - w) * 0.5f : gfx::kVirtualWidth - w - m;
        const float y = pos >= 3 ? gfx::kVirtualHeight - h - m : m;
        const float rel = hud_fps_ / static_cast<float>(hz);
        const Color accent = rel >= 0.9f ? Color::rgb(0x6dff7a) : rel >= 0.5f ? Color::rgb(0xffd940) : Color::rgb(0xff5959);
        list.rounded_rect({x, y, w, h}, 14, Color::rgb(0x080a10, kHudBgAlpha[hud_bg_index()]));
        list.rounded_rect({x + 8, y + 12, bar - 1.0f, h - 24}, 2, accent);
        for (int i = 0; i < 3; ++i)
            ui::text(list, fonts.semibold, ls[i], x + pad + bar, y + pad + 18.0f + lh * static_cast<float>(i) - 4.0f, size,
                     i == 0 ? accent : kWhite.with_alpha(0.92f));
    }

    void draw_boot(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const launcher::Boot &b = launcher::boot();
        list.push_opacity(tween::smoothstep(boot_t_));
        list.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0, Color::rgb(0x05070f, b.solid ? 0.97f : 0.62f));
        const Rect box{360, 390, 1200, 300};
        glass_panel(list, box, 40, palette_[3].value(), 0.8f);
        list.rounded_rect(box, 40, Color::rgb(0x05070f, 0.34f));
        const float cx = box.x + 92, cy = box.y + 104;
        list.ring(cx, cy, 36, 6, kWhite.with_alpha(0.14f));
        list.arc(cx, cy, 36, 6, clock_ * 4.0f, 1.9f, palette_[3].value());
        const float tx = box.x + 170, right = box.x + box.w - 56;
        char t[24];
        const int secs = static_cast<int>(b.seconds);
        std::snprintf(t, sizeof t, "%d:%02d", secs / 60, secs % 60);
        const float tw = fonts.regular.font->measure(t, 24.0f);
        ui::text(list, fonts.regular, t, right - tw, box.y + 76, 24, kWhite.with_alpha(0.6f));
        ui::text_shrink(list, fonts.semibold, b.title.empty() ? "Starting Minecraft" : b.title, tx, box.y + 78, 38, 38 * 0.5f, right - tw - 24 - tx, kWhite);
        ui::text_shrink(list, fonts.regular, b.stage.empty() ? "Starting the game" : b.stage, tx, box.y + 128, 26, 26 * 0.55f, right - tx, kWhite.with_alpha(0.92f));
        if (!b.detail.empty())
        {
            float size = 19.0f;
            std::vector<std::string> lines = fonts.regular.font->wrap(b.detail, size, right - tx);
            if (lines.size() > 2)
            {
                size = 15.0f;
                lines = fonts.regular.font->wrap(b.detail, size, right - tx);
            }
            const std::size_t n = std::min<std::size_t>(lines.size(), 3);
            for (std::size_t i = 0; i < n; ++i)
                ui::text_shrink(list, fonts.regular, lines[i], tx, box.y + 168 + (size + 8.0f) * static_cast<float>(i), size, size * 0.6f, right - tx, kWhite.with_alpha(0.62f));
        }
        const Rect track{box.x + 56, box.y + box.h - 44, box.w - 112, 6};
        list.rounded_rect(track, 3, kWhite.with_alpha(0.16f));
        const float p = std::fmod(clock_ * 0.55f, 1.0f), w = track.w * 0.22f;
        list.rounded_rect({track.x + (track.w - w) * (0.5f - 0.5f * std::cos(p * 6.2831853f)), track.y, w, track.h}, 3, palette_[3].value());
        list.pop_opacity();
    }

    // Darkens a photo where the text sits and tints it with the focused entry's colour.
    void draw_scrim(gfx::DrawList &list, float a) const
    {
        const Rect full{0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight};
        list.push_opacity(a);
        list.rounded_rect(full, 0, Color::rgb(0x05070f, 0.16f));
        list.gradient_rect(full, 0, Color::rgb(0x000000, 0.10f), Color::rgb(0x000000, 0.46f));
        list.gradient_rect_h(full, 0, Color::rgb(0x000000, 0.40f), Color::rgb(0x000000, 0.0f));
        list.rounded_rect(full, 0, palette_[1].value().with_alpha(0.14f));
        list.pop_opacity();
    }

    // ---- liquid glass ----
    // A pane of frosted glass over the blurred picture: a dark base so text stays readable on bright photos, a tint wash,
    // a veil that is lit from above, a thin rim and a specular streak along the top edge that slowly drifts.
    // glow > 0 adds coloured light spilling out of it (it breathes).
    void glass_panel(gfx::DrawList &list, const Rect &r, float radius, Color tint, float glow = 0.0f, float lift = 1.0f) const
    {
        radius = std::min(radius, std::min(r.w, r.h) * 0.5f);
        list.shadow({r.x, r.y + std::min(16.0f, r.h * 0.18f), r.w, r.h}, radius, 24.0f, Color::rgb(0x000000, 0.30f));
        if (glow > 0.0f)
            list.glow(r, radius, 30.0f, tint.with_alpha(glow * (0.10f + 0.06f * ui::breathe(clock_, 3.6f))));
        if (glass_tex_)
            list.glass(glass_tex_, r, radius, kWhite);
        else
            list.rounded_rect(r, radius, Color::rgb(0x0b0f1a, 0.92f));
        list.rounded_rect(r, radius, Color::rgb(0x070a14, 0.17f));
        list.rounded_rect(r, radius, tint.with_alpha(0.12f));
        list.gradient_rect(r, radius, kWhite.with_alpha(0.21f * lift), kWhite.with_alpha(0.05f * lift));
        list.bordered_rect(r, radius, Color::rgb(0x000000, 0.0f), 1.5f, kWhite.with_alpha(0.26f * lift));
        const float sx = r.x + radius * 0.75f, sw = r.w - radius * 1.5f;
        if (sw > 24.0f)
        {
            const float c = 0.5f + 0.30f * std::sin(clock_ * 0.55f + r.x * 0.004f + r.y * 0.003f);
            list.gradient_rect_h({sx, r.y + 1.5f, sw * c, 2.2f}, 1.1f, kWhite.with_alpha(0.0f), kWhite.with_alpha(0.62f * lift));
            list.gradient_rect_h({sx + sw * c, r.y + 1.5f, sw * (1.0f - c), 2.2f}, 1.1f, kWhite.with_alpha(0.62f * lift), kWhite.with_alpha(0.0f));
        }
    }

    std::span<const app::TourStep> tour() const override
    {
        return kTour;
    }

  private:
    // ---- data ----
    void rebuild()
    {
        shelves_.assign(2, Shelf{});
        shelves_[0].title = "Instances";
        shelves_[1].title = "Tools";
        for (int i = 0; i < static_cast<int>(store_.instances.size()); ++i)
        {
            const launcher::Instance &in = store_.instances[static_cast<std::size_t>(i)];
            Entry e;
            e.kind = Kind::instance;
            e.instance = i;
            e.title = in.name();
            const std::string ev = store_.effective_version(in);
            const std::string gv = ev.empty() ? std::string("No version") : game_version(ev);
            const std::string loader = loader_name(in.loader());
            char meta[160];
            const int mods = store_.mod_count(in);
            if (ev.empty())
                std::snprintf(meta, sizeof meta, "No Minecraft version  \xC2\xB7  pick one in Settings");
            else if (mods > 0)
                std::snprintf(meta, sizeof meta, "%s  \xC2\xB7  %s  \xC2\xB7  %d mods", loader.c_str(),
                              gv.c_str(), mods);
            else
                std::snprintf(meta, sizeof meta, "%s  \xC2\xB7  %s", loader.c_str(), gv.c_str());
            e.meta = meta;
            e.badge = ev.empty() ? std::string("--") : gv;
            e.blurb = in.loader().empty() ? "Plain Minecraft. Saves, options and screenshots stay in this instance."
                                          : std::string("Runs with ") + loader + ". Add mods from Content; saves and options stay in this instance.";
            palette_for(in, e);
            shelves_[0].items.push_back(std::move(e));
        }
        Entry create;
        create.kind = Kind::create;
        create.title = "New instance";
        create.meta = "Copy of the current one";
        create.badge = "+";
        create.blurb = "Makes a copy of the selected instance's settings with its own saves and mods.";
        create.dark = Color::rgb(0x10151f);
        create.mid = Color::rgb(0x2a3347);
        create.accent = Color::rgb(0xa8b4cf);
        shelves_[0].items.push_back(std::move(create));

        struct T
        {
            const char *title, *meta, *badge, *blurb;
            unsigned dark, mid, accent;
        };
        static const T tools[] = {
            {"Game Versions", "Minecraft releases, Fabric, Quilt", "VER",
             "Download a Minecraft release, snapshot or old version with its libraries and assets, and add Fabric or Quilt to it.",
             0x0f2a1d, 0x1f6b47, 0x6fe6a3},
            {"Content", "Mods, resource packs, shaders", "MOD",
             "Search Modrinth and install mods, resource packs and shaders into the selected instance.", 0x2a1a0c, 0x8a4b16, 0xffb86b},
            {"Library", "Everything installed", "LIB",
             "Lists the installed Minecraft versions, mods, resource packs and shaders. Switch mods on or off, or delete what you no longer need.",
             0x2a1015, 0x8a2f3f, 0xff8a9a},
            {"Accounts", "Offline account", "ID", "Who you play as. Sign in with a Microsoft account to play online, or play offline.", 0x1a1030,
             0x4a2a8f, 0xb79bff},
            {"Settings", "Start-up and Java", "CFG", "Choose whether this launcher opens at start-up, pick the Java version, and see where the game is installed.",
             0x101a2a, 0x2a4a7a, 0x7fb4ff},
        };
        int t = 0;
        for (const T &tool : tools)
        {
            Entry e;
            e.kind = Kind::tool;
            e.tool = t++;
            e.title = tool.title;
            e.meta = tool.meta;
            e.badge = tool.badge;
            e.blurb = tool.blurb;
            e.dark = Color::rgb(tool.dark);
            e.mid = Color::rgb(tool.mid);
            e.accent = Color::rgb(tool.accent);
            shelves_[1].items.push_back(std::move(e));
        }
    }

    static void palette_for(const launcher::Instance &in, Entry &e)
    {
        const unsigned h = hash(in.version());
        const bool mod = !in.loader().empty();
        static const unsigned vanilla[][3] = {{0x0f2a1d, 0x1f6b47, 0x6fe6a3}, {0x1a2a10, 0x4a7a1f, 0xb4f06f}, {0x102a2a, 0x1f6b6b, 0x6fe6e6}};
        static const unsigned modded[][3] = {{0x101a33, 0x2a4fa8, 0x7fb4ff}, {0x1a1030, 0x5a2fa8, 0xc09bff}, {0x2a1020, 0xa82f6b, 0xff9bc8}};
        const unsigned(*p)[3] = mod ? modded : vanilla;
        const unsigned *c = p[h % 3];
        e.dark = Color::rgb(c[0]);
        e.mid = Color::rgb(c[1]);
        e.accent = Color::rgb(c[2]);
    }

    int focused_index() const
    {
        const Shelf &s = shelves_[static_cast<std::size_t>(focus_row_)];
        return focus_row_ * 100 + s.column;
    }

    const Entry &focused() const
    {
        const Shelf &s = shelves_[static_cast<std::size_t>(focus_row_)];
        return s.items[static_cast<std::size_t>(s.column)];
    }

    const Entry &entry_at(int index) const
    {
        const Shelf &s = shelves_[static_cast<std::size_t>(index / 100)];
        return s.items[static_cast<std::size_t>(std::min<int>(index % 100, static_cast<int>(s.items.size()) - 1))];
    }

    launcher::Instance *focused_instance()
    {
        const Entry &e = focused();
        return e.kind == Kind::instance ? &store_.instances[static_cast<std::size_t>(e.instance)] : nullptr;
    }

    void apply_palette(bool snap)
    {
        const Entry &e = focused();
        const Color targets[4] = {
            gfx::mix(e.dark, Color::rgb(0x05060c), 0.35f),
            gfx::mix(e.dark, e.mid, 0.35f),
            e.mid,
            gfx::mix(e.mid, e.accent, 0.55f),
        };
        for (int i = 0; i < 4; ++i)
        {
            if (snap)
                palette_[i].snap(targets[i]);
            else
                palette_[i].target(targets[i]);
        }
    }

    Rect card_rect(int row, int column, bool grown) const
    {
        const Shelf &shelf = shelves_[static_cast<std::size_t>(row)];
        const float x = kMargin + static_cast<float>(column) * (kCard + kCardGap) - shelf.scroll.offset();
        const float y = kShelfY + (static_cast<float>(row) - row_position_.value) * kShelfPitch;
        if (!grown)
            return {x, y, kCard, kCard};
        const float g = kCard * kCardGrow;
        return {x, y - (g - kCard), g, g};
    }

    // ---- USB mouse and keyboard ----
    // The pointer sits in the 1920x1080 space of the drawing. Moving it over something focuses it (only while it moves, so the pad and the keys keep control otherwise),
    // a left click presses confirm on the focused thing, a right click presses back, the wheel scrolls. Everything is turned into the same InputFrame the pad makes,
    // so each screen keeps one code path.
    struct ActHit
    {
        Rect rect;
        ui::Button button;
    };
    mutable std::vector<ui::HintBox> hint_hits_;   // the bottom hint row, recorded while drawing
    mutable std::vector<ActHit> act_hits_;   // the action lines of the sheet pages, recorded while drawing (the right column's "Square: Rename" lines)
    static int cursor_index(const std::string &v)
    {
        const int n = v.empty() ? 3 : std::atoi(v.c_str());
        return std::clamp(n - 2, 0, 4);   // sprite pixel = n screen pixels at 1080p: 2 small .. 6 huge; 3 is the game's size
    }
    float cur_scale_ = 3.0f;
    bool kbm_ = false;                       // the keyboard / mouse spoke last (the pad did not): keys instead of pad glyphs, no on-screen keyboard
    int wheel_acc_ = 0;                      // wheel notches on the home screen not played yet (+ = along the shelf to the right)
    int drag_list_ = 0;                      // 1 while the scrollbar of a list is being dragged
    bool drag_form_ = false;                 // the scrollbar of a form (settings pages) is being dragged
    float drag_grab_ = 0.0f;                 // where inside the thumb it was grabbed
    bool ptr_vis_ = false;
    float ptr_x_ = 960.0f, ptr_y_ = 540.0f;
    std::vector<unsigned char> cur_px_;      // the Bedrock pointer of the game, 4 bytes per pixel
    int cur_w_ = 0, cur_h_ = 0;

    static bool hit(const Rect &r, float x, float y)
    {
        return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
    }

    static bool action_for(ui::Button b, Action &out)
    {
        switch (b)
        {
        case ui::Button::cross: out = Action::confirm; return true;
        case ui::Button::circle: out = Action::back; return true;
        case ui::Button::square: out = Action::west; return true;
        case ui::Button::triangle: out = Action::north; return true;
        case ui::Button::left_stick: out = Action::l3; return true;
        case ui::Button::right_stick: out = Action::r3; return true;
        case ui::Button::l1: out = Action::page_prev; return true;
        case ui::Button::r1: out = Action::page_next; return true;
        case ui::Button::l2: out = Action::jump_prev; return true;
        case ui::Button::r2: out = Action::jump_next; return true;
        case ui::Button::options: out = Action::menu; return true;
        case ui::Button::touchpad: out = Action::touch; return true;
        default: return false;
        }
    }

    // Which tab of a row of tabs (draw_tabs) is under the point, or -1.
    int tab_at(const char *const *names, int count, float px, float py) const
    {
        const Rect sheet = sheet_rect();
        float x = sheet.x + 56;
        for (int i = 0; i < count; ++i)
        {
            const float w = context_.fonts.semibold.font->measure(names[i], 22.0f) + 44.0f;
            if (hit({x, sheet.y + 140, w, 46}, px, py))
                return i;
            x += w + 12;
        }
        return -1;
    }

    // Wheel over a list: scrolls the view; the highlight follows into view like with the right stick.
    void wheel_list(int notches, int &row, int count, ui::Scroller &scroll, float row_h)
    {
        if (!notches || count <= 0)
            return;
        scroll_list_to(scroll.position.target - static_cast<float>(notches) * row_h * 1.5f, row, count, scroll, row_h);
    }

    // Moves a list's view to `target` pixels (clamped); the highlight follows into view.
    void scroll_list_to(float target, int &row, int count, ui::Scroller &scroll, float row_h)
    {
        const float limit = std::max(0.0f, static_cast<float>(count) * row_h - kListH);
        scroll.position.target = std::clamp(target, 0.0f, limit);
        const float top = scroll.position.target, bottom = top + kListH;
        if (static_cast<float>(row) * row_h < top)
            row = std::min(count - 1, static_cast<int>(std::ceil(top / row_h)));
        else if (static_cast<float>(row + 1) * row_h > bottom)
            row = std::max(0, static_cast<int>(std::floor(bottom / row_h)) - 1);
    }

    // The scrollbar of a list: a track right of the rows with a thumb (shown while the keyboard / mouse is in use; the thumb drags).
    static Rect scroll_track(const Rect &view)
    {
        return {view.x + view.w + 8.0f, view.y, 14.0f, view.h};
    }
    static Rect scroll_thumb(const Rect &view, int count, float row_h, float offset)
    {
        const float total = static_cast<float>(count) * row_h;
        const Rect tr = scroll_track(view);
        const float h = std::clamp(tr.h * tr.h / std::max(total, 1.0f), 48.0f, tr.h);
        const float limit = std::max(total - tr.h, 1.0f);
        return {tr.x, tr.y + (tr.h - h) * std::clamp(offset / limit, 0.0f, 1.0f), tr.w, h};
    }
    void draw_scrollbar(gfx::DrawList &list, const Rect &view, int count, float row_h, const ui::Scroller &scroll) const
    {
        if (!kbm_ || static_cast<float>(count) * row_h <= view.h + 1.0f)
            return;
        const Rect tr = scroll_track(view);
        const Rect th = scroll_thumb(view, count, row_h, scroll.offset());
        const bool hot = drag_list_ != 0 || (ptr_vis_ && hit(tr, ptr_x_, ptr_y_));
        list.rounded_rect({tr.x + 4, tr.y, 6, tr.h}, 3, kWhite.with_alpha(0.08f));
        list.rounded_rect({th.x + (hot ? 2.0f : 4.0f), th.y, hot ? 10.0f : 6.0f, th.h}, 5, kWhite.with_alpha(hot ? 0.75f : 0.45f));
    }

    // A hovered button of the mouse: a soft light plate and a rim.
    void draw_hover(gfx::DrawList &list, const Rect &r, float radius, bool in_sheet) const
    {
        if (!ptr_vis_ || sheet_open_ != in_sheet || kb_open_ || confirm_open_ || error_open_ || !hit(r, ptr_x_, ptr_y_))
            return;
        list.rounded_rect(r, radius, kWhite.with_alpha(0.12f));
        list.bordered_rect(r, radius, Color::rgb(0x000000, 0.0f), 2.0f, kWhite.with_alpha(0.7f));
    }

    // The row of a list under the point, or -1.
    int list_row_at(const ui::Scroller &scroll, int count, float row_h, float px, float py) const
    {
        const Rect sheet = sheet_rect();
        const Rect view{sheet.x + 56, sheet.y + 214, 1040, kListH};
        if (!hit(view, px, py))
            return -1;
        const int i = static_cast<int>(std::floor((py - view.y + scroll.offset()) / row_h));
        return i >= 0 && i < count ? i : -1;
    }

    InputFrame pointer_frame(const InputFrame &in, app::Feedback &fb)
    {
        InputFrame out = in;
        if (ps5_usb_set_text_mode)
            ps5_usb_set_text_mode(kb_open_ ? 1 : 0);
        const PointerInput &p = in.ptr;
        ptr_x_ = p.x;
        ptr_y_ = p.y;
        kbm_ = p.kbm;
        ui::keyboard_glyphs() = p.kbm;                 // hints show keys while the keyboard / mouse is in use, pad buttons once the pad speaks
        ptr_vis_ = p.kbm && p.active;                  // the pointer stays like on a desktop; a button or stick on the pad hides it at once
        const auto press = [&](Action a) { out.pressed |= action_bit(a); };
        const bool click = p.left_click, rclick = p.right_click, moved = p.moved;
        const float px = p.x, py = p.y;

        if (kb_open_ && !error_open_ && !confirm_open_)
        {
            // typing on a real keyboard goes through the on-screen one's own key logic (max length, shift, layouts)
            if (p.kbm)
                kb_edit(p);   // a text field: no on-screen keyboard needed
            else
            {
                for (int i = 0; i < p.backspaces; ++i)
                    keyboard_.tap("\b", fb);
                for (int i = 0; i < p.typed_n; ++i)
                    keyboard_.tap(std::string(1, p.typed[i]), fb);
            }
            if (p.enter)
                press(Action::menu);   // Options = Done
            keyboard_.set_length(static_cast<int>(kb_text_.size()));
        }
        if (!p.active)
            return out;

        if (click)   // the hint row at the bottom: a click presses that button
            for (const ui::HintBox &b : hint_hits_)
            {
                Action act;
                if (hit({b.x, b.y, b.w, b.h}, px, py) && action_for(b.button, act))
                {
                    press(act);
                    return out;
                }
            }
        if (rclick || p.esc)
            press(Action::back);
        if (error_open_)
        {
            if (click)
                press(Action::confirm);
            return out;
        }
        if (confirm_open_)
        {
            const Rect pn{460, 270, 1000, 500};
            const Rect del{pn.x + 60, pn.y + pn.h - 120, 250, 64};
            const Rect no{del.x + del.w + 20, del.y, 230, 64};
            if (click && hit(del, px, py))
                press(Action::confirm);
            else if (click && !hit(pn, px, py))
                press(Action::back);
            else if (click && hit(no, px, py))
                press(Action::back);
            return out;
        }
        if (kb_open_ && p.kbm)
        {
            const Rect pn = kb_panel();
            const Rect done = kb_done_rect(), cancel = kb_cancel_rect();
            if (click && hit(done, px, py))
                press(Action::menu);
            else if (click && hit(cancel, px, py))
                press(Action::back);
            else if (click && !hit(pn, px, py))
                press(Action::back);
            return out;
        }
        if (kb_open_)
        {
            const auto &layouts = keyboard_.layouts();
            const auto &rows = layouts[static_cast<std::size_t>(keyboard_.layout())].rows;
            for (int r = 0; r < static_cast<int>(rows.size()); ++r)
                for (int k = 0; k < static_cast<int>(rows[static_cast<std::size_t>(r)].size()); ++k)
                    if (hit(keyboard_.key_rect(r, k), px, py))
                    {
                        if (moved && (keyboard_.focus_row() != r || keyboard_.focus_key() != k))
                            keyboard_.set_focus(r, k, false);
                        if (click)
                        {
                            keyboard_.set_focus(r, k, false);
                            press(Action::confirm);
                        }
                    }
            if (click && !hit(kb_panel(), px, py))
                press(Action::back);
            return out;
        }
        if (sheet_open_)
        {
            const Rect sheet = sheet_rect();
            if (click && !hit(sheet, px, py))
            {
                press(Action::back);
                return out;
            }
            if (click)
                for (const ActHit &a : act_hits_)
                {
                    Action act;
                    if (hit(a.rect, px, py) && action_for(a.button, act))
                    {
                        press(act);
                        return out;
                    }
                }
            if (mode_ == Mode::install || mode_ == Mode::mods || mode_ == Mode::installed)
            {
                static const char *const t_install[] = {"Releases", "Snapshots", "Old versions"};
                static const char *const t_mods[] = {"Mods", "Resource packs", "Shaders", "Datapacks", "Modpacks"};
                static const char *const t_inst[] = {"Versions", "Mods", "Resource packs", "Shaders", "Worlds", "Datapacks"};
                int &row = mode_ == Mode::install ? install_row_ : mode_ == Mode::mods ? mods_row_ : inst_row_;
                ui::Scroller &scroll = mode_ == Mode::install ? install_scroll_ : mode_ == Mode::mods ? mods_scroll_ : inst_scroll_;
                const float row_h = mode_ == Mode::install ? kRowH : kModRowH;
                const int count = mode_ == Mode::install ? static_cast<int>(view_.size())
                                  : mode_ == Mode::mods  ? static_cast<int>(results_.size())
                                                         : static_cast<int>(inst_rows_.size());
                wheel_list(p.wheel, row, count, scroll, row_h);
                if (count > 0 && p.home)
                {
                    row = 0;
                    scroll_list_to(0.0f, row, count, scroll, row_h);
                    cue(audio::Cue::focus);
                }
                if (count > 0 && p.end)
                {
                    row = count - 1;
                    scroll_list_to(static_cast<float>(count) * row_h, row, count, scroll, row_h);
                    cue(audio::Cue::focus);
                }
                {   // the scrollbar: grab the thumb, or click the track to jump there; the thumb follows the mouse while the button is down
                    const Rect view{sheet.x + 56, sheet.y + 214, 1040, kListH};
                    const float total = static_cast<float>(count) * row_h, limit = std::max(0.0f, total - kListH);
                    if (click && limit > 0.0f && hit(scroll_track(view), px, py))
                    {
                        const Rect th = scroll_thumb(view, count, row_h, scroll.offset());
                        drag_list_ = 1;
                        drag_grab_ = hit(th, px, py) ? py - th.y : th.h * 0.5f;
                    }
                    if (!p.left_down)
                        drag_list_ = 0;
                    if (drag_list_ && limit > 0.0f)
                    {
                        const Rect tr = scroll_track(view);
                        const Rect th = scroll_thumb(view, count, row_h, 0.0f);
                        const float f = std::clamp((py - drag_grab_ - tr.y) / std::max(tr.h - th.h, 1.0f), 0.0f, 1.0f);
                        scroll_list_to(f * limit, row, count, scroll, row_h);
                        scroll.position.snap(scroll.position.target);
                        return out;
                    }
                }
                const int tab = mode_ == Mode::install ? tab_at(t_install, 3, px, py)
                                : mode_ == Mode::mods  ? tab_at(t_mods, 5, px, py)
                                                       : tab_at(t_inst, 6, px, py);
                if (click && tab >= 0)
                {
                    if (mode_ == Mode::install && tab != install_tab_)
                    {
                        install_tab_ = tab;
                        install_row_ = 0;
                        install_scroll_.position.snap(0.0f);
                        rebuild_view();
                        cue(audio::Cue::tab);
                    }
                    else if (mode_ == Mode::mods && tab != mods_tab_)
                    {
                        mods_tab_ = tab;
                        request_search();
                        cue(audio::Cue::tab);
                    }
                    else if (mode_ == Mode::installed && tab != inst_tab_)
                    {
                        inst_tab_ = tab;
                        inst_row_ = 0;
                        inst_scroll_.position.snap(0.0f);
                        rebuild_installed();
                        want_info();
                        cue(audio::Cue::tab);
                    }
                    return out;
                }
                if (mode_ == Mode::mods && click && hit({sheet.x + 56, sheet.y + 140, 1040, 46}, px, py) && tab < 0)
                {
                    press(Action::west);   // the search box right of the tabs
                    return out;
                }
                if (mode_ == Mode::install && click && hit({sheet.x + 56, sheet.y + 96, 1040, 40}, px, py))
                    press(Action::jump_prev);   // the version search
                const int at = list_row_at(scroll, count, row_h, px, py);
                if (at >= 0)
                {
                    if (moved && at != row)
                    {
                        row = at;
                        cue(audio::Cue::focus);
                    }
                    if (click)
                    {
                        row = at;
                        press(Action::confirm);
                    }
                }
            }
            else
            {
                if (p.wheel != 0)
                    form_.scroll_by(-static_cast<float>(p.wheel) * 90.0f);
                {   // the form's scrollbar: grab the thumb (or click the track to jump there) and drag; the grab area is wider than the thin bar
                    const ui::Form::ScrollGeom g = form_.scroll_geom();
                    const Rect grab{g.track.x - 10.0f, g.track.y - 8.0f, g.track.w + 28.0f, g.track.h + 16.0f};
                    if (!p.left_down)
                        drag_form_ = false;
                    form_.scroll_hot = g.overflow && (drag_form_ || (ptr_vis_ && hit(grab, px, py)));
                    if (g.overflow && click && hit(grab, px, py))
                    {
                        drag_form_ = true;
                        drag_grab_ = hit(g.thumb, px, py) || (px >= grab.x && py >= g.thumb.y && py <= g.thumb.y + g.thumb.h) ? py - g.thumb.y : g.thumb.h * 0.5f;
                    }
                    if (drag_form_ && g.overflow)
                    {
                        const float f = std::clamp((py - drag_grab_ - g.track.y) / std::max(g.track.h - g.thumb.h, 1.0f), 0.0f, 1.0f);
                        form_.scroll_to(f * g.limit, true);
                        return out;
                    }
                }
                for (int i = 0; i < form_.row_count(); ++i)
                {
                    const ui::FormRow &fr = form_.row_at(i);
                    if (fr.kind == ui::FormRowKind::header || fr.kind == ui::FormRowKind::value)
                        continue;
                    const Rect rr = form_.row_rect(i);
                    if (!hit(form_.bounds(), px, py) || !hit(rr, px, py))
                        continue;
                    if (moved && form_.focus() != i)
                    {
                        form_.set_focus(i, false);
                        cue(audio::Cue::focus);
                    }
                    const float pad = form_.style.padding;
                    const float cw = std::min(form_.style.control_width, rr.w - 2.0f * pad);
                    const Rect cr{rr.x + rr.w - pad - cw, rr.y, cw, rr.h};
                    const bool on_control = form_.style.values_right && hit(cr, px, py);
                    if (fr.kind == ui::FormRowKind::slider && on_control && p.left_down)
                    {
                        // a slider: the thumb goes where the mouse is (drag works too), on its step grid
                        const float f = std::clamp((px - cr.x - 14.0f) / std::max(cr.w - 28.0f, 1.0f), 0.0f, 1.0f);
                        const float span = fr.maximum - fr.minimum, unit = fr.step > 0.0f ? fr.step : span / 100.0f;
                        form_.set_focus(i, false);
                        form_.set_slider(fr.id, fr.minimum + std::round(f * span / unit) * unit);
                    }
                    else if (click && on_control && (fr.kind == ui::FormRowKind::choice || fr.kind == ui::FormRowKind::stepper))
                    {
                        // a choice or a number: the left part steps back, the right part steps on, the middle confirms
                        const float f = (px - cr.x) / cr.w;
                        form_.set_focus(i, false);
                        if (f < 0.4f)
                            out.nav = Direction::left;
                        else if (f > 0.6f)
                            out.nav = Direction::right;
                        else
                            press(Action::confirm);
                        out.nav_repeat = false;
                    }
                    else if (click)
                    {
                        form_.set_focus(i, false);
                        press(Action::confirm);
                    }
                    break;
                }
            }
            return out;
        }

        // the home screen: the cards of both shelves
        // The wheel runs along the shelf under the pointer, one card per notch (a burst of notches is queued and played one per frame, so a fast spin neither skips nor jams at the end of the shelf);
        // pointing at the other shelf is what switches shelves. (A wheel that moved between the two shelves only bounced back and forth: "scrolling gets stuck".)
        if (p.wheel != 0 || p.hwheel != 0)
        {
            for (int r = 0; r < 2; ++r)
            {
                const float distance = static_cast<float>(r) - row_position_.value;
                if (distance > 1.5f || distance < -1.5f)
                    continue;
                const Rect band = card_rect(r, 0, false);
                const float top = kShelfY + distance * kShelfPitch;
                if (py >= top && py < top + band.h + 24.0f && r != focus_row_)
                {
                    focus_row_ = r;
                    wheel_acc_ = 0;
                    cue(audio::Cue::tab);
                    break;
                }
            }
            wheel_acc_ = std::clamp(wheel_acc_ - p.wheel + p.hwheel, -8, 8);
        }
        if (out.nav == Direction::none && wheel_acc_ != 0)
        {
            const Shelf &cur = shelves_[static_cast<std::size_t>(focus_row_)];
            const int last = static_cast<int>(cur.items.size()) - 1;
            if ((wheel_acc_ > 0 && cur.column >= last) || (wheel_acc_ < 0 && cur.column <= 0))
                wheel_acc_ = 0;   // already at the end of the shelf: drop the rest of the burst
            else
            {
                out.nav = wheel_acc_ > 0 ? Direction::right : Direction::left;
                out.nav_repeat = false;
                wheel_acc_ += wheel_acc_ > 0 ? -1 : 1;
            }
        }
        if (click)
        {
            const Entry &fe = focused();
            const Rect play{kMargin, 556, 240, 64}, edit{kMargin + 260, 556, 220, 64};
            if (hit({1824 - 204.0f, 52, 204, 56}, px, py))
            {
                open_tool(3, fb);
                return out;
            }
            if (hit(play, px, py))
            {
                press(Action::confirm);
                return out;
            }
            if (fe.kind == Kind::instance && hit(edit, px, py))
            {
                press(Action::west);
                return out;
            }
        }
        for (int r = 0; r < 2; ++r)
        {
            Shelf &shelf = shelves_[static_cast<std::size_t>(r)];
            const float distance = static_cast<float>(r) - row_position_.value;
            if (distance > 1.5f || distance < -1.5f)
                continue;
            for (int c = 0; c < static_cast<int>(shelf.items.size()); ++c)
            {
                Rect rect = card_rect(r, c, false);
                rect.y = kShelfY + distance * kShelfPitch;
                if (r == focus_row_ && c > shelf.column)
                    rect.x += kCard * (kCardGrow - 1.0f);
                if (r == focus_row_ && c == shelf.column)
                {
                    rect = card_rect(r, c, true);
                }
                if (!hit(rect, px, py))
                    continue;
                if (moved && (r != focus_row_ || c != shelf.column))
                {
                    if (r != focus_row_)
                        cue(audio::Cue::tab);
                    else
                        cue(audio::Cue::focus);
                    focus_row_ = r;
                    shelves_[static_cast<std::size_t>(r)].column = c;
                }
                if (click && r == focus_row_ && c == shelves_[static_cast<std::size_t>(r)].column)
                    press(Action::confirm);
                else if (click)
                {
                    focus_row_ = r;
                    shelves_[static_cast<std::size_t>(r)].column = c;
                    press(Action::confirm);
                }
                return out;
            }
        }
        return out;
    }

    // The pointer: the sprite of Minecraft Bedrock's cursor (the same pixels the game draws), 3 screen pixels per sprite pixel, hotspot at the top-left.
    void draw_pointer(gfx::DrawList &list) const
    {
        if (!ptr_vis_ || cur_px_.empty())
            return;
        draw_cursor_sprite(list, ptr_x_, ptr_y_, cur_scale_);
    }

    void draw_cursor_sprite(gfx::DrawList &list, float ox, float oy, float s) const
    {
        if (cur_px_.empty())
            return;
        for (int r = 0; r < cur_h_; ++r)
        {
            int c = 0;
            while (c < cur_w_)
            {
                const unsigned char *px = &cur_px_[static_cast<std::size_t>((r * cur_w_ + c) * 4)];
                if (px[3] == 0)
                {
                    ++c;
                    continue;
                }
                int e = c + 1;
                while (e < cur_w_)
                {
                    const unsigned char *q = &cur_px_[static_cast<std::size_t>((r * cur_w_ + e) * 4)];
                    if (q[3] == 0 || q[0] != px[0] || q[1] != px[1] || q[2] != px[2])
                        break;
                    ++e;
                }
                list.rounded_rect({ox + static_cast<float>(c) * s, oy + static_cast<float>(r) * s, static_cast<float>(e - c) * s, s}, 0,
                                  Color::rgb((px[0] << 16) | (px[1] << 8) | px[2], 1.0f));
                c = e;
            }
        }
    }

    void load_pointer_art()
    {
        if (!ps5_cursor_pixels)
            return;
        const int wh = ps5_cursor_pixels(nullptr, 0);
        cur_w_ = wh >> 16;
        cur_h_ = wh & 0xffff;
        cur_px_.assign(static_cast<std::size_t>(cur_w_ * cur_h_ * 4), 0);
        ps5_cursor_pixels(cur_px_.data(), static_cast<int>(cur_px_.size()));
    }

    // ---- input ----
    // The result is published a moment later so the Play sound and press animation are seen.
    void finish(const char *action)
    {
        finish_action_ = action;
        finish_left_ = context_.settings.reduced_motion ? 0.05f : 0.35f;
    }

    void status(const std::string &text)
    {
        status_ = text;
        status_age_ = 0.0f;
    }

    void update_shelves(const InputFrame &input, app::Feedback &feedback)
    {
        Shelf &shelf = shelves_[static_cast<std::size_t>(focus_row_)];
        const int count = static_cast<int>(shelf.items.size());
        bool refused = false;
        switch (input.nav)
        {
        case Direction::left:
        case Direction::right:
        {
            const int next = shelf.column + (input.nav == Direction::right ? 1 : -1);
            if (next >= 0 && next < count)
            {
                shelf.column = next;
                feedback.play(audio::Cue::focus, 1.0f, ui::pan_for_x(card_rect(focus_row_, next, false).cx()));
            }
            else
            {
                refused = !input.nav_repeat;
                nudge_dir_ = input.nav == Direction::right ? 1.0f : -1.0f;
            }
            break;
        }
        case Direction::up:
        case Direction::down:
        {
            const int next = focus_row_ + (input.nav == Direction::down ? 1 : -1);
            if (next >= 0 && next < 2)
            {
                focus_row_ = next;
                feedback.play(audio::Cue::tab, 1.12f - 0.08f * static_cast<float>(focus_row_));
            }
            else
            {
                refused = !input.nav_repeat;
                nudge_dir_ = 0.0f;
            }
            break;
        }
        case Direction::none:
            break;
        }
        if (refused)
        {
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
            feedback.rumble(0.25f, 0.05f);
            nudge_.trigger();
        }
        if (input.is_pressed(Action::page_prev))
            bg_.step(-1);
        if (input.is_pressed(Action::page_next))
            bg_.step(1);
        if (finish_left_ > 0.0f)
            return;
        if (input.is_pressed(Action::confirm))
            activate(feedback);
        if (input.is_pressed(Action::west) && focused().kind == Kind::instance)
            open_sheet(feedback);
    }

    void activate(app::Feedback &feedback)
    {
        const Entry e = focused();
        switch (e.kind)
        {
        case Kind::instance:
        {
            const launcher::Instance &pin = store_.instances[static_cast<std::size_t>(e.instance)];
            const std::string ev = store_.effective_version(pin);
            if (jobs_.busy() && jobs_.active().rfind("info", 0) != 0)
            {
                status("A download is still running. Wait for it to finish, then press Play.");
                refuse();
                break;
            }
            if (pin.get("customJar").empty() && !store_.versions.empty() && (ev.empty() || !installed(ev)))
            {
                status(ev.empty() ? "\"" + pin.name() + "\" has no Minecraft version. Square (Settings) picks one, or install one in Game Versions."
                                  : "Minecraft " + ev + " is not installed. Square (Settings) picks another, or install it in Game Versions.");
                refuse();
                break;
            }
            store_.select(pin.id);
            feedback.play(audio::Cue::launch);
            feedback.rumble(0.7f, 0.18f);
            play_.trigger();
            status("Starting " + e.title + "  \xC2\xB7  " + e.meta);
            finish("play");
            break;
        }
        case Kind::create:
        {
            launcher::Instance base = store_.instances[static_cast<std::size_t>(std::max(0, store_.find(store_.current)))];
            char id[32];
            int n = 2;
            do
                std::snprintf(id, sizeof id, "instance%d", n++);
            while (store_.find(id) >= 0);
            base.id = id;
            base.set("name", "New instance");
            base.props.erase("gameDir");
            store_.save(base);
            store_.instances.push_back(base);
            rebuild();
            shelves_[0].column = static_cast<int>(store_.instances.size()) - 1;
            feedback.play(audio::Cue::open);
            status("Created \"New instance\". Square edits it.");
            break;
        }
        case Kind::tool:
            open_tool(e.tool, feedback);
            break;
        }
    }

    void open_sheet(app::Feedback &feedback)
    {
        launcher::Instance *in = focused_instance();
        if (!in)
            return;
        build_form(*in);
        mode_ = Mode::settings;
        sheet_open_ = true;
        feedback.play(audio::Cue::open);
    }

    void close_sheet(app::Feedback &feedback, bool save)
    {
        if (save)
        {
            if (mode_ == Mode::settings)
                commit_form();
            else if (mode_ == Mode::launcher)
                commit_launcher();
        }
        sheet_open_ = false;
        info_pending_.clear();
        feedback.play(audio::Cue::back);
    }

    // The Loader row lists only what is installed for the chosen Minecraft version (Vanilla, plus Fabric and/or Quilt), so Left / Right never lands on something
    // that is then snapped back. When the version changes the list is rebuilt and the current loader is kept if the new version has it.
    static const char *loader_label(int kind)
    {
        return kind == 1 ? "Fabric" : kind == 2 ? "Quilt" : kind == 3 ? "Forge" : kind == 4 ? "NeoForge" : "Vanilla";
    }
    static const char *loader_key(int kind)
    {
        return kind == 1 ? "fabric" : kind == 2 ? "quilt" : kind == 3 ? "forge" : kind == 4 ? "neoforge" : "";
    }
    static int loader_index(const std::string &key)
    {
        return key == "fabric" ? 1 : key == "quilt" ? 2 : key == "forge" ? 3 : key == "neoforge" ? 4 : 0;
    }
    static std::string loader_name(const std::string &key)
    {
        return key.empty() ? std::string("Vanilla") : std::string(loader_label(loader_index(key)));
    }

    void sync_loader()
    {
        if (mode_ != Mode::settings)
            return;
        ui::FormRow *row = form_.row(kLoader);
        if (!row)
            return;
        const std::size_t vi = static_cast<std::size_t>(form_.choice_index(kVersion));
        const std::string mc = vi < versions_.size() && !versions_[vi].empty() ? game_version(versions_[vi]) : std::string();
        // A change of the Loader row by the player is what the instance wants; a version without that loader only shows Vanilla meanwhile, and the wish comes
        // back when a version that has the loader is picked again (flipping through versions never loses Fabric).
        const std::size_t idx = static_cast<std::size_t>(form_.choice_index(kLoader));
        const int shown = idx < loader_kinds_.size() ? loader_kinds_[idx] : 0;
        if (!loader_kinds_.empty() && shown != loader_shown_)
            loader_want_ = loader_shown_ = shown;
        if (mc == loader_mc_ && !loader_kinds_.empty())
            return;
        const int keep = loader_want_;
        loader_kinds_.assign(1, 0);
        std::string have_list;
        for (int k = 1; k <= 4; ++k)
            if (!mc.empty() && has_loader(loader_key(k), mc))
            {
                loader_kinds_.push_back(k);
                have_list += std::string(", ") + loader_label(k);
            }
        const bool any_loader = loader_kinds_.size() > 1;
        std::vector<std::string> labels;
        int pos = 0;
        for (std::size_t i = 0; i < loader_kinds_.size(); ++i)
        {
            labels.push_back(loader_label(loader_kinds_[i]));
            if (loader_kinds_[i] == keep)
                pos = static_cast<int>(i);
        }
        const bool first = loader_mc_ == "\x01";
        loader_shown_ = loader_kinds_[static_cast<std::size_t>(pos)];
        if (keep != 0 && loader_shown_ != keep && !first && !mc.empty())
        {
            cue(audio::Cue::error);
            status(std::string(loader_label(keep)) + " is not installed for " + mc + ". Install it in Game Versions first.");
        }
        row->choice.set_options(labels);
        row->choice.set_index(pos);
        loader_mc_ = mc;
        form_.set_disabled(kLoader, loader_kinds_.size() < 2);
        row->description = mc.empty()                       ? "Pick a Minecraft version first."
                           : any_loader ? std::string("Installed for ") + mc + ": Vanilla" + have_list + "."
                                        : "No mod loader is installed for " + mc + ". Add one in Game Versions (Square: Fabric, Triangle: Quilt, L3: Forge, R3: NeoForge).";
    }

    void update_sheet(const InputFrame &input, float dt, app::Feedback &feedback)
    {
        if (mode_ == Mode::launcher && form_.row(kCursor))
            cur_scale_ = static_cast<float>(2 + std::clamp(form_.choice_index(kCursor), 0, 4));   // the pointer and the preview follow the row at once
        if (std::fabs(input.stick2_y) > 0.2f)
            form_.scroll_by(input.stick2_y * 1500.0f * dt);   // right stick scrolls long pages
        const ui::Event ev = form_.handle(input, feedback);
        sync_loader();
        form_.update(dt);
        if (ev == ui::Event::cancelled)
        {
            close_sheet(feedback, true);
        }
        else if (ev == ui::Event::activated && form_.changed_id() == kName)
        {
            if (const launcher::Instance *in = focused_instance())
                open_keyboard(0, in->name(), "Rename instance", 24);
        }
        else if (ev == ui::Event::activated && form_.changed_id() == kDelete)
        {
            if (const launcher::Instance *in = focused_instance())
                ask_delete_instance(*in);
        }
        else if (mode_ == Mode::account && ev == ui::Event::activated && form_.changed_id() == kSignIn)
        {
            if (jobs_.busy() && jobs_.active().rfind("login", 0) == 0)
            {
                // same button: while the code is waiting it cancels (the Java side polls this file)
                if (std::FILE *f = std::fopen((store_.root + "/launcher/login.cancel").c_str(), "wb"))
                {
                    std::fputs("cancel\n", f);
                    std::fclose(f);
                }
                if (ui::FormRow *r = form_.row(kSignIn))
                    r->label = "Cancelling...";
                status("Cancelling the sign-in...");
            }
            else if (submit_job("login"))
                status("Asking Microsoft for a sign-in code...");
        }
        else if (mode_ == Mode::account && ev == ui::Event::activated && form_.changed_id() == kUsername)
        {
            open_keyboard(4, store_.username, "Offline username", 16);
        }
        else if (mode_ == Mode::account && ev == ui::Event::activated && form_.changed_id() >= kUse0 && form_.changed_id() <= kUse3)
        {
            int slot = form_.changed_id() - kUse0, n = 0;
            for (const auto &acc : saved_accounts())
            {
                if (acc.first == store_.account)
                    continue;
                if (n++ == slot)
                {
                    if (submit_job("account use " + acc.first))
                        status("Playing online as " + acc.second);
                    break;
                }
            }
        }
        else if (mode_ == Mode::account && ev == ui::Event::activated && form_.changed_id() == kSignOut)
        {
            if (submit_job("signout " + store_.account))
                status("Signing out...");
        }
        else if (mode_ == Mode::account && ev == ui::Event::activated && form_.changed_id() == kOffline)
        {
            if (submit_job("account use offline"))
                status("Playing offline");
        }
        if (mode_ == Mode::account)
            poll_login_code(dt);
    }

    // ---- Microsoft sign-in (the Java side runs the device-code flow and writes <root>/launcher/login.txt: code, page, direct link) ----
    float login_poll_ = 0.0f;
    std::string login_shown_;

    static std::string read_small_file(const std::string &path)
    {
        std::string out;
        if (std::FILE *f = std::fopen(path.c_str(), "rb"))
        {
            // whole file (up to 256 KB): accounts.properties holds long tokens, so a name line can sit past the first 512 bytes
            char buf[4096];
            for (std::size_t n; out.size() < (256u << 10) && (n = std::fread(buf, 1, sizeof buf, f)) > 0;)
                out.append(buf, n);
            std::fclose(f);
        }
        return out;
    }
    static std::string nth_line(const std::string &text, int n)
    {
        std::size_t at = 0;
        for (int i = 0; i < n && at != std::string::npos; ++i)
        {
            at = text.find('\n', at);
            if (at != std::string::npos)
                ++at;
        }
        if (at == std::string::npos)
            return std::string();
        const std::size_t e = text.find('\n', at);
        std::string line = text.substr(at, e == std::string::npos ? std::string::npos : e - at);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        return line;
    }
    bool signed_in_now() const
    {
        return !(store_.account == "offline" || store_.account.empty());
    }
    // the second line of a shelf card / the page header; the Accounts tool shows who is signed in
    std::string meta_of(const Entry &e) const
    {
        if (e.kind == Kind::tool && e.tool == 3)
            return signed_in_now() ? "Microsoft: " + account_label() : std::string("Offline account");
        return e.meta;
    }
    // The Microsoft accounts saved on this console (accounts.properties: <id>.name=<name>), newest signed-in first is not tracked: file order.
    std::vector<std::pair<std::string, std::string>> saved_accounts() const
    {
        std::vector<std::pair<std::string, std::string>> out;
        const std::string text = store_.root.empty() ? std::string() : read_small_file(store_.root + "/accounts.properties");
        for (std::size_t at = 0; at < text.size();)
        {
            const std::size_t e = text.find('\n', at);
            std::string line = text.substr(at, e == std::string::npos ? std::string::npos : e - at);
            at = e == std::string::npos ? text.size() : e + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            const std::size_t dot = line.find(".name=");
            if (dot == std::string::npos || dot == 0 || line.find('.') != dot)
                continue;
            const std::string id = line.substr(0, dot), name = line.substr(dot + 6);
            if (!name.empty() && id != "offline")
                out.emplace_back(id, name);
        }
        return out;
    }
    // the signed-in account's display name: accounts.properties has <id>.name=<name>
    std::string account_label() const
    {
        if (store_.account == "offline" || store_.account.empty() || store_.root.empty())
            return "Offline";
        const std::string key = store_.account + ".name=";
        const std::string text = read_small_file(store_.root + "/accounts.properties");
        for (std::size_t at = 0; at < text.size();)
        {
            const std::size_t e = text.find('\n', at);
            const std::string line = text.substr(at, e == std::string::npos ? std::string::npos : e - at);
            if (line.compare(0, key.size(), key) == 0)
            {
                std::string name = line.substr(key.size());
                while (!name.empty() && (name.back() == '\r' || name.back() == ' '))
                    name.pop_back();
                if (!name.empty())
                    return name;
            }
            at = e == std::string::npos ? text.size() : e + 1;
        }
        return store_.account;
    }
    // while the "login" job waits for the player: show the code and the page to open, as soon as the Java side has written them
    void poll_login_code(float dt)
    {
        login_poll_ -= dt;
        if (login_poll_ > 0.0f)
            return;
        login_poll_ = 0.4f;
        const bool waiting = jobs_.busy() && jobs_.active().rfind("login", 0) == 0;
        std::string code, page;
        if (waiting && !store_.root.empty())
        {
            const std::string t = read_small_file(store_.root + "/launcher/login.txt");
            code = nth_line(t, 0);
            page = nth_line(t, 1);
            const std::size_t sl = page.find("://");
            if (sl != std::string::npos)
                page = page.substr(sl + 3);
        }
        const std::string now = waiting ? code + "|" + page : std::string("-");
        if (now == login_shown_)
            return;
        login_shown_ = now;
        if (ui::FormRow *sr = form_.row(kSignIn))
        {
            const bool signed_in = !(store_.account == "offline" || store_.account.empty());
            sr->label = waiting ? "Cancel sign-in" : signed_in ? "Sign in with another account" : "Sign in with Microsoft";
        }
        if (ui::FormRow *c = form_.row(kInfo1))
        {
            form_.set_value_text(kInfo1, !waiting ? "None" : code.empty() ? "Requesting..." : code);
            c->description = !waiting ? "Appears here while you sign in."
                             : code.empty() ? "Asking Microsoft for a code..."
                                            : "Enter this code on the page shown below. It stays valid for a few minutes.";
        }
        if (ui::FormRow *p = form_.row(kInfo2))
        {
            form_.set_value_text(kInfo2, !waiting || page.empty() ? "None" : page);
            p->description = !waiting ? "The page to open on a phone or PC." : "Open this page on a phone or PC, sign in with the Microsoft account that owns Minecraft and type the code. This screen updates by itself.";
        }
    }

    std::vector<std::string> base_versions() const
    {
        std::vector<std::string> v;
        for (const std::string &s : store_.versions)
            if (loader_kind(s).empty())
                v.push_back(s);
        return v;
    }

    // a value of the game's options as the last "info options" read it ("" = the game has not saved one)
    std::string game_option(const char *key) const
    {
        for (const launcher::InfoRow &o : jobs_.info_rows("options"))
            if (o.file == key)
                return o.loader;
        return std::string();
    }

    // options.txt keeps the field of view as (degrees - 70) / 40 in every Minecraft version; a value above 2.5 can only be degrees already
    static int fov_from_options(const std::string &raw)
    {
        if (raw.empty())
            return 70;
        const double v = std::atof(raw.c_str());
        const double deg = (v > 2.5 || v < -2.5) ? v : v * 40.0 + 70.0;
        return static_cast<int>(std::min(110.0, std::max(30.0, std::floor(deg + 0.5))));
    }

    void build_form(const launcher::Instance &in)
    {
        form_.clear();
        form_.style = ui::FormStyle{};
        form_.style.theme = theme_;
        form_.style.reduced_motion = context_.settings.reduced_motion;
        form_.style.compact = true;
        form_.style.description_inline = false; // the help text is shown in full in the right column (a one-line fold cut it to "Memory i...")
        form_.style.control_width = 360.0f;
        form_.style.row_height = 62.0f;
        form_.style.header_height = 50.0f;
        form_.style.number_width = 230.0f;   // "110° (Quake Pro)" needs this much

        // Index 0 is "None": an instance may have no Minecraft version (Play then asks for one). A version that is not installed any more stays in the list so that
        // opening this sheet and going back never rewrites it.
        const std::string cur_ver = store_.effective_version(in);
        std::vector<std::string> versions{std::string()};
        for (const std::string &b : base_versions())
            versions.push_back(b);
        int vi = 0;
        std::string loader_id = cur_ver;
        const std::string gv_cur = game_version(cur_ver);
        for (std::size_t i = 1; i < versions.size(); ++i)
            if (versions[i] == gv_cur)
                vi = static_cast<int>(i);
        if (!cur_ver.empty() && vi == 0)
        {
            versions.push_back(gv_cur);
            vi = static_cast<int>(versions.size()) - 1;
        }
        versions_ = versions;
        std::vector<std::string> labels;
        for (std::size_t i = 0; i < versions.size(); ++i)
            labels.push_back(versions[i].empty() ? std::string("None") : (!installed(versions[i]) ? versions[i] + " (not installed)" : versions[i]));
        loader_kinds_.clear();
        loader_mc_ = "\x01";
        loader_want_ = loader_shown_ = loader_index(in.loader());
        form_.add_header("Instance");
        form_.add_action(kName, "Rename: " + in.name()).description = "Change the name shown on the instance card. Opens the on-screen keyboard.";
        form_.add_choice(kVersion, "Minecraft version", labels, vi).description =
            "The Minecraft version this instance starts. Only installed versions are listed; add more under Tools, Game Versions. None leaves the instance without a version: Play asks you to pick one.";
        form_.add_choice(kLoader, "Loader", {"Vanilla"}, 0).description =
            "Vanilla, Fabric, Quilt, Forge or NeoForge. Only the loaders installed for the version above are offered (Tools, Game Versions).";
        form_.add_header("Controller");
        form_.add_toggle(kPadEmu, "Controller emulation", in.get("padEmu", "on") != "off").description =
            "On: the DualSense controls Minecraft like a keyboard and mouse (touchpad moves the cursor). Off: for mods that read the controller themselves.";
        form_.add_header("Performance");
        form_.add_stepper(kCpus, "CPU cores", in.geti("cpus", 4), 1, 8).description = "How many CPU cores Minecraft believes it has; it sizes its worker thread pools from this. Applies the next time the game starts.";
        form_.add_stepper(kJit, "JIT threads", in.geti("jitThreads", 1), 1, 4).description =
            "Threads that compile hot code in the background. More threads warm up faster after launch but take time from the game. Applies the next time the game starts.";
        {
            // the game's own setting (options.txt, the value Minecraft's Video Settings shows): read from it, written back into it; there is no second value kept here
            int rd = 12;
            for (const launcher::InfoRow &o : jobs_.info_rows("options"))
                if (o.file == "renderDistance" && !o.loader.empty() && std::atoi(o.loader.c_str()) >= 2)
                    rd = std::min(32, std::atoi(o.loader.c_str()));
            rd_loaded_ = rd;
            fov_loaded_ = fov_from_options(game_option("fov"));
            rd_form_ = true;
            opt_pending_ = true;
            form_.add_stepper(kRenderDist, "Render distance", rd, 2, 32).description =
                "The same setting as Render Distance in Minecraft's Video Settings: changing it here changes it there, and the other way round (each Minecraft version keeps its own). "
                "Chunks drawn around you. On this console 8 to 12 is smooth, above 16 can drop the frame rate a lot. Applies the next time the game starts.";
        }
        {
            ui::FormRow &r = form_.add_slider(kFov, "Field of view", static_cast<float>(fov_loaded_), 30, 110, 1);
            r.format = [](float v) {
                const int d = static_cast<int>(v + 0.5f);
                return std::to_string(d) + "\xC2\xB0" + (d == 70 ? " (Normal)" : d == 110 ? " (Quake Pro)" : "");
            };
            r.description = "The same setting as FOV in Minecraft's Video Settings, read from and written to it (each Minecraft version keeps its own). 70\xC2\xB0 is Normal, 110\xC2\xB0 is Quake Pro. "
                            "A wider view shows more of the world but stretches the edges and costs a little speed. Applies the next time the game starts.";
        }
        {
            const std::string gc = in.get("gc", "parallel");
            form_.add_choice(kGc, "Garbage collector", {"serial", "parallel"}, gc == "serial" ? 0 : 1).description =
                "How Java cleans up memory. Parallel gives smoother frames in large worlds; serial uses less memory. Applies the next time the game starts.";
        }
        form_.add_toggle(kMerge, "Chunk merging", in.get("merge", "on") != "off").description =
            "Combines Minecraft's many chunk draw calls into a few large ones. Much faster at long view distances. Turn off only if a mod draws wrongly. Applies the next time the game starts.";
        form_.add_toggle(kGlThread, "Graphics worker thread", in.get("glthread", "on") != "off").description =
            "Runs the graphics driver calls on their own thread so the game thread is not held up. Turn off only for troubleshooting. Applies the next time the game starts.";
        form_.add_header("Account and display");
        form_.add_value(kAccount, "Account", account_label())
            .description = "Who you play as. Change it in Accounts on the home screen.";
        form_.add_header("Danger zone");
        {
            ui::FormRow &r = form_.add_action(kDelete, "Delete instance");
            r.danger = true;
            r.disabled = in.id == "default";
            r.description = in.id == "default" ? "The default instance cannot be deleted."
                                               : "Permanently deletes this instance with its worlds, mods and settings. Asks to confirm first.";
        }
        form_.set_bounds({sheet_rect().x + 48, sheet_rect().y + 112, 940, sheet_rect().h - 152});
        form_.set_focus(1);
        form_.enter();
    }

    void commit_form()
    {
        launcher::Instance *in = focused_instance();
        if (!in)
            return;
        const std::size_t vi = static_cast<std::size_t>(form_.choice_index(kVersion));
        const std::string base = vi < versions_.size() ? versions_[vi] : std::string();
        const std::size_t li = static_cast<std::size_t>(form_.choice_index(kLoader));
        int loader = li < loader_kinds_.size() ? loader_kinds_[li] : 0;
        if (base.empty() || (loader > 0 && !has_loader(loader_key(loader), base)))
            loader = 0; // never keep a loader that is not installed for this version
        std::string version = base;
        if (loader > 0)
        {
            // Use an installed loader profile for this version when there is one.
            for (const std::string &s2 : store_.versions)
                if (loader_kind(s2) == loader_key(loader) && game_version(s2) == base)
                    version = s2;
            in->set("loader", loader_key(loader));
        }
        else
        {
            in->props.erase("loader");
        }
        if (version.empty())
        {
            in->props.erase("version");
            if (in->id == "default")
                store_.set_selected(""); // the default instance falls back to versions/selected.txt when it names no version
        }
        else
            in->set("version", version);
        in->seti("cpus", form_.stepper_value(kCpus));
        in->seti("jitThreads", form_.stepper_value(kJit));
        in->set("padEmu", form_.toggle_value(kPadEmu) ? "on" : "off");
        {
            const int rd = form_.stepper_value(kRenderDist), fov = static_cast<int>(form_.slider_value(kFov) + 0.5f);
            in->props.erase("options.renderDistance");   // old builds kept their own value here and forced it at every start
            in->props.erase("options.fov");
            std::string cmd;
            if (rd_form_ && rd != rd_loaded_ && rd >= 2 && rd <= 32)
                cmd += " renderDistance " + std::to_string(rd);
            if (rd_form_ && fov != fov_loaded_ && fov >= 30 && fov <= 110)
                cmd += " fov " + std::to_string(fov);
            if (!cmd.empty() && jobs_.submit("option" + cmd))
            {
                rd_loaded_ = rd;
                fov_loaded_ = fov;
            }
        }
        in->set("gc", form_.choice_index(kGc) == 0 ? "serial" : "parallel");
        in->set("merge", form_.toggle_value(kMerge) ? "on" : "off");
        in->props.erase("uncap");   // V-Sync is one launcher-wide setting now
        in->set("glthread", form_.toggle_value(kGlThread) ? "on" : "off");
        in->seti("width", 1920);
        in->seti("height", 1080);
        store_.save(*in);
        rebuild_keep();
        { const std::string rn = restart_note(); status("Saved " + in->name() + (rn.empty() ? std::string() : ". " + rn)); }
    }

    // ---- tools: jobs, pages, keyboard ----
    void rebuild_keep()
    {
        const int c0 = shelves_[0].column, c1 = shelves_[1].column;
        const ui::Scroller s0 = shelves_[0].scroll, s1 = shelves_[1].scroll;
        rebuild();
        shelves_[0].column = std::max(0, std::min(c0, static_cast<int>(shelves_[0].items.size()) - 1));
        shelves_[1].column = c1;
        shelves_[0].scroll = s0;
        shelves_[1].scroll = s1;
    }

    // Editing with a real keyboard: typed characters, Backspace / Delete, Left / Right / Home / End. No on-screen keyboard involved.
    void kb_edit(const PointerInput &p)
    {
        kb_caret_ = std::min(kb_caret_, kb_text_.size());
        for (int i = 0; i < p.caret_l; ++i)
            while (kb_caret_ > 0)
            {
                --kb_caret_;
                if ((static_cast<unsigned char>(kb_text_[kb_caret_]) & 0xC0) != 0x80)
                    break;
            }
        for (int i = 0; i < p.caret_r; ++i)
            while (kb_caret_ < kb_text_.size())
            {
                ++kb_caret_;
                if (kb_caret_ >= kb_text_.size() || (static_cast<unsigned char>(kb_text_[kb_caret_]) & 0xC0) != 0x80)
                    break;
            }
        if (p.home)
            kb_caret_ = 0;
        if (p.end)
            kb_caret_ = kb_text_.size();
        for (int i = 0; i < p.backspaces; ++i)
            kb_backspace();
        for (int i = 0; i < p.del; ++i)
        {
            if (kb_caret_ >= kb_text_.size())
                break;
            do
                kb_text_.erase(kb_caret_, 1);
            while (kb_caret_ < kb_text_.size() && (static_cast<unsigned char>(kb_text_[kb_caret_]) & 0xC0) == 0x80);
        }
        for (int i = 0; i < p.typed_n; ++i)
            if (static_cast<int>(kb_text_.size()) < kb_max_)
            {
                kb_text_.insert(kb_caret_, 1, p.typed[i]);
                ++kb_caret_;
            }
    }

    Rect kb_done_rect() const
    {
        const Rect p = kb_panel();
        return {p.x + 60, p.y + p.h - 112, 250, 64};
    }
    Rect kb_cancel_rect() const
    {
        const Rect d = kb_done_rect();
        return {d.x + d.w + 20, d.y, 230, 64};
    }

    void kb_backspace()   // removes the character left of the cursor
    {
        kb_caret_ = std::min(kb_caret_, kb_text_.size());
        while (kb_caret_ > 0)
        {
            const unsigned char c = static_cast<unsigned char>(kb_text_[kb_caret_ - 1]);
            kb_text_.erase(kb_caret_ - 1, 1);
            --kb_caret_;
            if ((c & 0xC0) != 0x80)
                break;
        }
    }

    void cue(audio::Cue c)
    {
        if (feedback_)
            feedback_->play(c);
    }

    void refuse()
    {
        if (feedback_)
        {
            feedback_->play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
            feedback_->rumble(0.25f, 0.05f);
        }
    }

    // The instance the tools work on: the one last focused on the first shelf.
    int target_index() const
    {
        const Shelf &s = shelves_[0];
        const Entry &e = s.items[static_cast<std::size_t>(std::min<int>(s.column, static_cast<int>(s.items.size()) - 1))];
        if (e.kind == Kind::instance)
            return e.instance;
        return std::max(0, store_.find(store_.current));
    }
    launcher::Instance &target()
    {
        return store_.instances[static_cast<std::size_t>(target_index())];
    }
    const launcher::Instance &target() const
    {
        return store_.instances[static_cast<std::size_t>(target_index())];
    }
    std::string target_line() const
    {
        const launcher::Instance &in = target();
        const std::string loader = loader_name(in.loader());
        const std::string ev = store_.effective_version(in);
        return "For " + in.name() + "  \xC2\xB7  " + loader + "  \xC2\xB7  " + (ev.empty() ? std::string("no version") : game_version(ev));
    }

    bool submit_job(const std::string &command)
    {
        if (jobs_.busy())
        {
            if (pending_cmd_.empty() && (jobs_.active().rfind("skin", 0) == 0 || info_busy()))
            {   // a silent background job (profile picture, folder read) is running: the press is not lost, the command goes out the moment it ends
                pending_cmd_ = command;
                return true;
            }
            status(info_busy() ? "Still reading the folder. Try again in a moment." : "A download is still running. It continues in the background.");
            refuse();
            return false;
        }
        if (!jobs_.submit(command))
        {
            status("The downloader did not take the command. Try again in a moment.");
            refuse();
            return false;
        }
        return true;
    }

    // A folder read ("info ...") is short and silent: no progress pill, and it does not count as a download.
    bool info_busy() const
    {
        return jobs_.busy() && jobs_.active().rfind("info", 0) == 0;
    }

    bool installed(const std::string &id) const
    {
        return std::find(store_.versions.begin(), store_.versions.end(), id) != store_.versions.end();
    }
    bool has_loader(const char *key, const std::string &mc) const // key: "fabric", "quilt", "forge", "neoforge"
    {
        for (const std::string &v : store_.versions)
            if (loader_kind(v) == key && game_version(v) == mc)
                return true;
        return false;
    }
    bool slug_installed(const std::string &slug) const
    {
        return std::find(installed_slugs_.begin(), installed_slugs_.end(), slug) != installed_slugs_.end();
    }

    void open_tool(int tool, app::Feedback &feedback)
    {
        feedback.play(audio::Cue::open);
        store_.select(target().id); // the Java side works on the current instance
        sheet_open_ = true;
        switch (tool)
        {
        case 0:
            mode_ = Mode::install;
            install_row_ = 0;
            install_scroll_.position.snap(0.0f);
            rebuild_view();
            if (catalog_.empty())
                submit_job("list-versions");
            break;
        case 1:
        {
            mode_ = Mode::mods;
            const launcher::Instance &in = target();
            const std::string key = in.id + "|" + store_.effective_version(in) + "|" + in.loader();
            if (key != search_key_)
            {
                search_key_ = key;
                request_search();
            }
            break;
        }
        case 2:
            mode_ = Mode::installed;
            inst_tab_ = 0;
            inst_row_ = 0;
            inst_scroll_.position.snap(0.0f);
            invalidate_info(); // the instance or its version may have changed since the last visit
            rebuild_installed();
            break;
        case 3:
            mode_ = Mode::account;
            build_account_form();
            break;
        default:
            mode_ = Mode::launcher;
            build_launcher_form();
            break;
        }
    }

    void on_job_done()
    {
        const std::string cmd = jobs_.command();
        const std::string word = cmd.substr(0, cmd.find(' '));
        const std::string detail = jobs_.message();
        if (word == "skin")   // the profile picture download: silent
        {
            face_try_ = -10.0f;
            return;
        }
        if (!jobs_.ok() && word == "info")
        {
            info_loading_.clear();
            status("Could not read the folder: " + (detail.empty() ? std::string("see the log") : detail));
            return;
        }
        if (!jobs_.ok() && word == "login" && (detail.find("cancelled") != std::string::npos || detail.find("expired") != std::string::npos))
        {
            status(detail.find("cancelled") != std::string::npos ? "Sign-in cancelled" : "Sign-in code expired: start again to get a new one");
            if (sheet_open_ && mode_ == Mode::account)
            {
                const int keep = form_.focus_id();
                build_account_form();
                if (keep >= 0)
                    form_.focus_row(keep);
            }
            return;
        }
        if (!jobs_.ok())
        {
            error_open_ = true;
            error_title_ = jobs_.status().title.empty() ? "That did not work" : jobs_.status().title + " failed";
            error_text_ = detail.empty() ? "No details were reported. The log has more." : detail;
            error_retry_ = true;
            if (word == "login")
            {
                error_title_ = "Microsoft sign-in failed";
                error_retry_ = false;
                if (detail.find("Invalid app registration") != std::string::npos)
                    error_text_ = "Microsoft accepted your sign-in, but Mojang has not approved this launcher's app yet (error 403, Invalid app registration). Nothing is wrong with your account. Online sign-in works once the app is approved; until then, play offline.";
                else if (detail.find("no Microsoft client id") != std::string::npos)
                    error_text_ = "This build has no Microsoft app ID. Build it with MSA_CLIENT_ID=<your Azure application ID>, or put msaClientId=<id> into launcher.cfg.";
            }
            cue(audio::Cue::error);
            if (feedback_)
                feedback_->rumble(0.5f, 0.2f);
            return;
        }
        if (word == "option")
        {
            status(detail.empty() ? "Render distance saved in the game's settings" : detail);
            return;
        }
        if (word == "info")
        {
            absorb_info(cmd);
            return;
        }
        if (word == "list-versions")
        {
            catalog_ = jobs_.versions();
            rebuild_view();
            status(detail.empty() ? "Version list updated" : detail);
        }
        else if (word == "search")
        {
            results_ = jobs_.search_results();
            mods_row_ = 0;
            mods_scroll_.position.snap(0.0f);
            status(detail.empty() ? "Search finished" : detail);
        }
        else
        {
            if (word == "mod" || word == "resourcepack" || word == "shader" || word == "datapack" || word == "modpack")
                installed_slugs_.push_back(cmd.substr(cmd.find(' ') + 1));
            reload_store(cmd);
            status(detail.empty() ? (word == "login" ? "Signed in as " + account_label() : word == "signout" ? std::string("Signed out") : std::string("Done")) : detail);
            cue(audio::Cue::open);
            if ((word == "login" || word == "account" || word == "signout" || word == "config") && sheet_open_ && mode_ == Mode::account)
            {
                const int keep = form_.focus_id();
                build_account_form();
                if (keep >= 0)
                    form_.focus_row(keep);
            }
        }
    }

    void reload_store(const std::string &cmd)
    {
        const std::string word = cmd.substr(0, cmd.find(' '));
        const std::string arg = cmd.find(' ') == std::string::npos ? "" : cmd.substr(cmd.find(' ') + 1);
        invalidate_info();
        if (jobs_.simulated())
            demo_worlds(word, arg);
        if (!jobs_.simulated())
        {
            const std::string root = store_.root;
            store_ = launcher::Store::load(root);
        }
        else if (word == "remove" || word == "toggle")
            store_.demo_apply(cmd);
        else if (word == "instance")
        {
            if (arg.rfind("delete ", 0) == 0)
                store_.forget(arg.substr(7));
        }
        else if (word == "fabric" || word == "quilt")
        {
            const std::string id = word + "-loader-0.16.9-" + arg;
            store_.versions.push_back(id);
            target().set("loader", word);
            target().set("version", id);
        }
        else if (word != "mod" && word != "resourcepack" && word != "shader" && word != "datapack" && word != "modpack" && word != "world" && word != "info")
        {
            const std::string id = word;
            if (!installed(id))
                store_.versions.push_back(id);
            target().set("version", id);
        }
        rebuild_keep();
        rebuild_view();
        rebuild_installed();
        want_info();
        if (word == "remove" || word == "toggle")
            installed_slugs_.clear(); // the "Installed" tags of this session no longer match the folder
        search_key_.clear(); // loader or version may have changed: search again next time
    }

    // ---- install page ----
    static constexpr float kRowH = 72.0f;
    static constexpr float kListH = 504.0f;

    void rebuild_view()
    {
        rows_ = catalog_;
        std::string q = install_query_;
        for (char &c : q) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (rows_.empty())
            for (const std::string &v : store_.versions)
                if (loader_kind(v).empty())
                    rows_.push_back({v, "release", ""});
        view_.clear();
        for (int i = 0; i < static_cast<int>(rows_.size()); ++i)
        {
            const std::string &t = rows_[static_cast<std::size_t>(i)].type;
            const int tab = t == "release" ? 0 : t == "snapshot" ? 1 : 2;
            if (tab != install_tab_)
                continue;
            if (!q.empty())
            {
                std::string id = rows_[static_cast<std::size_t>(i)].id;
                for (char &c : id) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (id.find(q) == std::string::npos)
                    continue;
            }
            view_.push_back(i);
        }
        if (!install_newest_)
            std::reverse(view_.begin(), view_.end()); // the manifest lists the newest first
        install_row_ = std::max(0, std::min(install_row_, static_cast<int>(view_.size()) - 1));
    }

    void move_row(const InputFrame &input, int &row, int count, ui::Scroller &scroll, float row_h, float view_h)
    {
        if (std::fabs(input.stick2_y) > 0.2f && count > 0)   // right stick: scroll the list freely; the highlight follows into view
        {
            const float limit = std::max(0.0f, static_cast<float>(count) * row_h - view_h);
            scroll.position.target = std::clamp(scroll.position.target + input.stick2_y * 1500.0f * last_dt_, 0.0f, limit);
            const float top = scroll.position.target, bottom = top + view_h;
            if (static_cast<float>(row) * row_h < top)
                row = std::min(count - 1, static_cast<int>(std::ceil(top / row_h)));
            else if (static_cast<float>(row + 1) * row_h > bottom)
                row = std::max(0, static_cast<int>(std::floor(bottom / row_h)) - 1);
        }
        if (input.nav != Direction::up && input.nav != Direction::down)
            return;
        const int next = row + (input.nav == Direction::down ? 1 : -1);
        if (count > 0 && next >= 0 && next < count)
        {
            row = next;
            cue(audio::Cue::focus);
        }
        else if (!input.nav_repeat)
            refuse();
        scroll.reveal(static_cast<float>(row) * row_h, static_cast<float>(row + 1) * row_h, view_h, 6.0f,
                      static_cast<float>(count) * row_h);
    }

    void update_install(const InputFrame &input, app::Feedback &feedback)
    {
        move_row(input, install_row_, static_cast<int>(view_.size()), install_scroll_, kRowH, kListH);
        if (input.is_pressed(Action::page_next) || input.is_pressed(Action::page_prev))
        {
            install_tab_ = (install_tab_ + (input.is_pressed(Action::page_next) ? 1 : 2)) % 3;
            install_row_ = 0;
            install_scroll_.position.snap(0.0f);
            rebuild_view();
            feedback.play(audio::Cue::tab);
        }
        if (input.is_pressed(Action::menu))
            submit_job("list-versions");
        if (input.is_pressed(Action::jump_prev))
        {
            open_keyboard(2, install_query_, "Search versions", 24);
            return;
        }
        if (input.is_pressed(Action::jump_next))
        {
            install_newest_ = !install_newest_;
            install_row_ = 0;
            install_scroll_.position.snap(0.0f);
            rebuild_view();
            feedback.play(audio::Cue::tab);
            status(install_newest_ ? "Newest first" : "Oldest first");
        }
        if (input.is_pressed(Action::back))
        {
            close_sheet(feedback, false);
            return;
        }
        if (view_.empty() || install_row_ >= static_cast<int>(view_.size()))
            return;
        const std::string id = rows_[static_cast<std::size_t>(view_[static_cast<std::size_t>(install_row_)])].id;
        if (input.is_pressed(Action::confirm) && submit_job(id + " select"))
        {
            feedback.play(audio::Cue::select);
            status("Installing " + id + " for " + target().name());
        }
        else if (input.is_pressed(Action::west) && submit_job("fabric " + id))
        {
            feedback.play(audio::Cue::select);
            status("Adding Fabric to " + id);
        }
        else if (input.is_pressed(Action::north) && submit_job("quilt " + id))
        {
            feedback.play(audio::Cue::select);
            status("Adding Quilt to " + id);
        }
        else if (input.is_pressed(Action::l3) && submit_job("forge " + id))
        {
            feedback.play(audio::Cue::select);
            status("Adding Forge to " + id + " (needs 1.17 or newer; this takes a few minutes)");
        }
        else if (input.is_pressed(Action::r3) && submit_job("neoforge " + id))
        {
            feedback.play(audio::Cue::select);
            status("Adding NeoForge to " + id + " (needs 1.20.2 or newer; this takes a few minutes)");
        }
    }

    // ---- mods page ----
    static const char *mod_type(int tab)
    {
        static const char *types[] = {"mod", "resourcepack", "shader", "datapack", "modpack"};
        return types[tab];
    }

    void request_search()
    {
        results_.clear();
        mods_row_ = 0;
        mods_scroll_.position.snap(0.0f);
        search_pending_ = true;
    }

    void run_search()
    {
        std::string cmd = std::string("search ") + mod_type(mods_tab_);
        if (!query_.empty())
            cmd += " " + query_;
        search_pending_ = !jobs_.submit(cmd);
    }

    void update_mods(const InputFrame &input, app::Feedback &feedback)
    {
        move_row(input, mods_row_, static_cast<int>(results_.size()), mods_scroll_, kModRowH, kListH);
        if (input.is_pressed(Action::page_next) || input.is_pressed(Action::page_prev))
        {
            mods_tab_ = (mods_tab_ + (input.is_pressed(Action::page_next) ? 1 : 4)) % 5;
            request_search();
            feedback.play(audio::Cue::tab);
        }
        if (input.is_pressed(Action::menu))
            request_search();
        if (input.is_pressed(Action::west))
        {
            open_keyboard(1, query_, "Search Modrinth", 40);
            return;
        }
        if (input.is_pressed(Action::back))
        {
            close_sheet(feedback, false);
            return;
        }
        if (input.is_pressed(Action::confirm) && mods_row_ < static_cast<int>(results_.size()))
        {
            const launcher::SearchRow &r = results_[static_cast<std::size_t>(mods_row_)];
            if (store_.effective_version(target()).empty())
            {
                status("This instance has no Minecraft version. Pick one in Settings (Square on the instance) first.");
                refuse();
            }
            else if (mods_tab_ == 0 && target().loader().empty())
            {
                status("Mods need a loader: Fabric, Quilt, Forge or NeoForge. Install one in Game Versions.");
                refuse();
            }
            else if (submit_job(std::string(mod_type(mods_tab_)) + " " + r.slug))
            {
                feedback.play(audio::Cue::select);
                status("Installing " + r.title);
            }
        }
    }

    static constexpr float kModRowH = 92.0f;

    // ---- confirm dialog (everything that deletes asks first) ----
    void ask(const std::string &title, const std::string &text, const std::string &command, const std::string &busy_text,
             bool close_sheet_after)
    {
        confirm_open_ = true;
        confirm_title_ = title;
        confirm_text_ = text;
        confirm_cmd_ = command;
        confirm_busy_ = busy_text;
        confirm_close_ = close_sheet_after;
        cue(audio::Cue::open);
    }

    void ask_delete_instance(const launcher::Instance &in)
    {
        std::string text = "This deletes the instance with its worlds, mods, resource packs and settings from this console. "
                           "It cannot be undone.";
        if (!in.get("gameDir").empty())
            text += " Its custom game folder is not touched.";
        ask("Delete \"" + in.name() + "\"?", text, "instance delete " + in.id, "Deleting " + in.name(), true);
    }

    void update_confirm(const InputFrame &input, app::Feedback &feedback)
    {
        if (input.is_pressed(Action::confirm))
        {
            confirm_open_ = false;
            if (submit_job(confirm_cmd_))
            {
                feedback.play(audio::Cue::select);
                status(confirm_busy_);
                if (confirm_close_)
                    close_sheet(feedback, false);
            }
        }
        else if (input.is_pressed(Action::back))
        {
            confirm_open_ = false;
            feedback.play(audio::Cue::back);
        }
    }

    // ---- installed page: versions, mods, resource packs, shaders ----
    static bool is_loader(const std::string &id)
    {
        return !loader_kind(id).empty();
    }

    static std::string join(const std::vector<std::string> &v)
    {
        std::string out;
        for (std::size_t i = 0; i < v.size(); ++i)
            out += (i ? ", " : "") + v[i];
        return out;
    }

    static std::string strip_suffix(const std::string &name)
    {
        for (const char *suffix : {".jar.disabled", ".jar", ".zip"})
        {
            const std::string x = suffix;
            if (name.size() > x.size() && name.compare(name.size() - x.size(), x.size(), x) == 0)
                return name.substr(0, name.size() - x.size());
        }
        return name;
    }

    // What the Java side read from the jars, packs and worlds (info jobs). The list shows its plain rows at once; the tags fill in when the answer arrives.
    static const char *info_kind(int tab)
    {
        return tab == 1 ? "mods" : tab == 2 ? "resourcepacks" : tab == 4 ? "saves" : tab == 5 ? "datapacks" : nullptr;
    }

    void invalidate_info()
    {
        for (bool &f : info_fresh_)
            f = false;
    }

    void want_info()
    {
        const char *kind = info_kind(inst_tab_);
        int fresh = inst_tab_;
        if (inst_tab_ == 5 && info_fresh_[5] && !info_fresh_[4])   // the Datapacks tab shows each world's picture, mode and last played: those come from the worlds list
        {
            kind = "saves";
            fresh = 4;
        }
        if (kind && sheet_open_ && mode_ == Mode::installed && !info_fresh_[fresh] && info_loading_.empty() && info_pending_.empty())
            info_pending_ = kind;
    }

    void release_icons()
    {
        const launcher::TextureHooks &h = launcher::textures();
        if (h.release)
            for (const std::uint32_t t : icon_tex_)
                if (t)
                    h.release(t);
        icon_tex_.clear();
    }

    // <root>/.kit/icons/<n>.rgba: width and height as little-endian ints, then the pixels (written by Launcher.info).
    void load_world_icons()
    {
        release_icons();
        icon_tex_.assign(worlds_.size(), 0);
        const launcher::TextureHooks &h = launcher::textures();
        if (!h.upload)
            return;
        for (std::size_t i = 0; i < worlds_.size(); ++i)
        {
            if (worlds_[i].icon.empty())
                continue;
            std::FILE *f = std::fopen(worlds_[i].icon.c_str(), "rb");
            if (!f)
                continue;
            std::int32_t wh[2] = {0, 0};
            bool ok = std::fread(wh, 4, 2, f) == 2 && wh[0] > 0 && wh[1] > 0 && wh[0] <= 512 && wh[1] <= 512;
            std::vector<std::uint8_t> px;
            if (ok)
            {
                px.resize(static_cast<std::size_t>(wh[0]) * static_cast<std::size_t>(wh[1]) * 4);
                ok = std::fread(px.data(), 1, px.size(), f) == px.size();
            }
            std::fclose(f);
            if (ok)
                icon_tex_[i] = h.upload(wh[0], wh[1], px.data());
        }
    }

    void absorb_info(const std::string &cmd)
    {
        const std::string kind = cmd.size() > 5 ? cmd.substr(5) : std::string();
        info_loading_.clear();
        if (kind == "options")
        {
            for (const launcher::InfoRow &o : jobs_.info_rows("options"))
                if (o.file == "renderDistance" && !o.loader.empty() && std::atoi(o.loader.c_str()) >= 2)
                {
                    const int v = std::min(32, std::atoi(o.loader.c_str()));
                    if (rd_form_ && form_.stepper_value(kRenderDist) == rd_loaded_)   // the player has not touched the row meanwhile
                        form_.set_stepper(kRenderDist, v);
                    rd_loaded_ = v;
                }
            if (rd_form_)
            {
                const int fv = fov_from_options(game_option("fov"));
                if (static_cast<int>(form_.slider_value(kFov) + 0.5f) == fov_loaded_)
                    form_.set_slider(kFov, static_cast<float>(fv));
                fov_loaded_ = fv;
            }
            return;
        }
        if (kind == "mods" || kind == "resourcepacks")
        {
            std::map<std::string, launcher::InfoRow> &m = kind == "mods" ? info_mods_ : info_packs_;
            m.clear();
            for (launcher::InfoRow &row : jobs_.info_rows(kind))
                m[row.file] = row;
            info_fresh_[kind == "mods" ? 1 : 2] = true;
        }
        else if (kind == "datapacks")
        {
            info_dp_ = jobs_.info_rows(kind);
            info_fresh_[5] = true;
        }
        else if (kind == "saves")
        {
            worlds_ = jobs_.world_rows();
            load_world_icons();
            info_fresh_[4] = true;
        }
        rebuild_installed();
        want_info(); // the tab may have changed while this was being read
    }

    // Preview only: the simulated Java side keeps no worlds, so a finished delete / rename is applied to the sample list.
    void demo_worlds(const std::string &word, const std::string &arg)
    {
        const auto word_at = [&arg](int n) {
            std::size_t at = 0;
            for (int i = 0; i < n; ++i)
            {
                at = arg.find(' ', at);
                if (at == std::string::npos)
                    return std::string();
                ++at;
            }
            const std::size_t e = arg.find(' ', at);
            return arg.substr(at, e == std::string::npos ? std::string::npos : e - at);
        };
        const auto dec = [](const std::string &e) {
            std::string out;
            for (std::size_t i = 0; i < e.size(); ++i)
                if (e[i] == '%' && i + 2 < e.size())
                {
                    out += static_cast<char>(std::strtol(e.substr(i + 1, 2).c_str(), nullptr, 16));
                    i += 2;
                }
                else
                    out += e[i];
            return out;
        };
        if (word == "remove" && word_at(0) == "saves")
        {
            const std::string folder = dec(word_at(1));
            for (std::size_t i = 0; i < worlds_.size(); ++i)
                if (worlds_[i].folder == folder)
                {
                    worlds_.erase(worlds_.begin() + static_cast<std::ptrdiff_t>(i));
                    if (i < icon_tex_.size())
                        icon_tex_.erase(icon_tex_.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            info_fresh_[4] = true;
        }
        else if (word == "world" && word_at(0) == "rename")
        {
            const std::string folder = dec(word_at(1));
            for (launcher::WorldRow &w : worlds_)
                if (w.folder == folder)
                    w.name = dec(word_at(2));
            info_fresh_[4] = true;
        }
    }

    static std::string ago_text(long long ms)
    {
        if (ms <= 0)
            return "never";
        const long long s = static_cast<long long>(std::time(nullptr)) - ms / 1000;
        char b[48];
        if (s < 90)
            return "just now";
        if (s < 3600 * 2)
            std::snprintf(b, sizeof b, "%lld min ago", s / 60);
        else if (s < 86400 * 2)
            std::snprintf(b, sizeof b, "%lld hours ago", s / 3600);
        else if (s < 86400 * 60)
            std::snprintf(b, sizeof b, "%lld days ago", s / 86400);
        else
            std::snprintf(b, sizeof b, "%lld months ago", s / (86400 * 30));
        return b;
    }

    static Color loader_color(const std::string &loader)
    {
        std::string l = loader;
        for (char &c : l)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return l == "fabric" ? Color::rgb(0x7fb4ff) : l == "quilt" ? Color::rgb(0xc09bff) : l == "forge" ? Color::rgb(0xffb070) : l == "neoforge" ? Color::rgb(0xf0a030) : Color::rgb(0xa8b4cf);
    }

    // The question before a version is deleted: what else goes with it and who is left without a version.
    std::string version_delete_text(const std::string &id) const
    {
        std::vector<std::string> gone{id};
        if (!is_loader(id))
            for (const std::string &v : store_.versions)
                if (is_loader(v) && game_version(v) == id)
                    gone.push_back(v);
        std::vector<std::string> users;
        for (const launcher::Instance &in : store_.instances)
            if (std::find(gone.begin(), gone.end(), store_.effective_version(in)) != gone.end())
                users.push_back(in.name());
        std::string t = "Removes the game files of " + id;
        if (gone.size() > 1)
            t += " and its " + std::to_string(gone.size() - 1) + (gone.size() > 2 ? " mod loader profiles" : " mod loader profile");
        t += ". Libraries, assets, worlds and mods stay.";
        if (!users.empty())
        {
            std::string names;
            for (std::size_t i = 0; i < users.size() && i < 3; ++i)
                names += (i ? ", " : "") + users[i];
            if (users.size() > 3)
                names += " and " + std::to_string(users.size() - 3) + " more";
            t += " " + names + (users.size() == 1 ? " is" : " are") + " left without a Minecraft version until you pick another in Settings.";
        }
        return t;
    }

    void rebuild_installed()
    {
        if (!sheet_open_ || mode_ != Mode::installed)
            return;
        inst_rows_.clear();
        const launcher::Instance &in = target();
        const Color amber = Color::rgb(0xffb86b), grey = Color::rgb(0xa8b4cf), green = Color::rgb(0x6fe6a3);
        if (inst_tab_ == 0)
        {
            std::vector<std::string> ids;
            for (const std::string &v : store_.versions)
                if (!is_loader(v))
                    ids.push_back(v);
            for (const std::string &v : store_.versions)
                if (is_loader(v))
                    ids.push_back(v);
            for (const std::string &id : ids)
            {
                InstalledRow r;
                r.title = id;
                const std::string lk = loader_kind(id);
                const bool is_prof = !lk.empty();
                r.tags.emplace_back(loader_name(lk), is_prof ? loader_color(lk) : Color::rgb(0x6fe6a3));
                // A version that something still runs can be deleted: the instances are left without a version (Play asks for one).
                const std::vector<std::string> users = store_.version_users(id);
                r.sub = launcher::size_text(store_.version_size(id)) +
                        (users.empty() ? "  \xC2\xB7  not used by anything" : "  \xC2\xB7  used by " + join(users));
                r.remove = "remove version " + id;
                r.detail = is_prof
                               ? loader_name(lk) + " loader profile for Minecraft " + game_version(id) +
                                     ". Deleting it keeps the game version itself."
                               : "Minecraft " + id + " game files. Libraries and assets are shared with the other versions and stay.";
                inst_rows_.push_back(std::move(r));
            }
        }
        else if (inst_tab_ == 4)
        {
            const std::string ev = store_.effective_version(in), gv = ev.empty() ? std::string() : game_version(ev);
            for (std::size_t i = 0; i < worlds_.size(); ++i)
            {
                const launcher::WorldRow &w = worlds_[i];
                InstalledRow r;
                r.world = true;
                r.folder = w.folder;
                r.name = w.name.empty() ? w.folder : w.name;
                r.title = r.name;
                r.tex = i < icon_tex_.size() ? icon_tex_[i] : 0;
                r.sub = w.folder + "  \xC2\xB7  " + launcher::size_text(w.size);
                r.when = ago_text(w.played);
                r.facts = (w.version.empty() ? std::string("Unknown version") : w.version) + (w.mode.empty() ? "" : "  \xC2\xB7  " + w.mode);
                if (!w.mode.empty())
                    r.tags.emplace_back(w.mode, w.mode == "Hardcore" ? Color::rgb(0xff8a8a) : w.mode == "Creative" ? Color::rgb(0x7fb4ff)
                                                                      : w.mode == "Survival" ? green : grey);
                if (w.version == "unreadable")
                    r.tags.emplace_back("Unreadable", amber);
                else if (!w.version.empty())
                    r.tags.emplace_back(w.version, grey);
                r.remove = "remove saves " + launcher::url_encode(w.folder);
                r.detail = "A world of " + in.name() + ". Rename changes the name Minecraft shows.";
                if (!gv.empty() && !w.version.empty() && w.version != "unreadable" && w.version != gv && w.version != "before 1.9")
                    r.detail += " Last saved in " + w.version + "; this instance runs " + gv + ".";
                inst_rows_.push_back(std::move(r));
            }
        }
        else if (inst_tab_ == 5)
        {
            // one row per pack for ONE world (L2/R2 changes the world); worlds come most recently played first and carry their real name, not the folder's
            std::vector<std::pair<std::string, std::string>> worlds;   // folder, real name
            std::vector<std::string> wvers;                           // the world's Minecraft version (level.dat), parallel to worlds
            for (const launcher::InfoRow &d : info_dp_)
            {
                if (d.loader.empty())
                    continue;
                bool seen = false;
                for (const auto &w : worlds)
                    seen = seen || w.first == d.loader;
                if (!seen)
                {
                    worlds.emplace_back(d.loader, d.mc.empty() ? d.loader : d.mc);
                    wvers.push_back(d.why);
                }
            }
            dp_world_count_ = static_cast<int>(worlds.size());
            dp_worlds_.clear();
            for (std::size_t i = 0; i < worlds.size(); ++i)
            {
                DpWorld w;
                w.folder = worlds[i].first;
                w.name = worlds[i].second;
                w.ver = wvers[i];
                for (const launcher::InfoRow &d : info_dp_)
                    if (d.loader == w.folder)
                    {
                        ++w.total;
                        w.on += d.name == "1" ? 1 : 0;
                    }
                dp_worlds_.push_back(std::move(w));
            }
            dp_world_name_.clear();
            dp_world_ver_.clear();
            dp_world_pos_ = 0;
            if (!worlds.empty())
            {
                bool known = false;
                for (const auto &w : worlds)
                    if (w.first == dp_world_)
                    {
                        known = true;
                        dp_world_name_ = w.second;
                        dp_world_pos_ = static_cast<int>(&w - worlds.data());
                        dp_world_ver_ = wvers[static_cast<std::size_t>(dp_world_pos_)];
                    }
                if (!known)
                {
                    dp_world_ = worlds[0].first;
                    dp_world_name_ = worlds[0].second;
                    dp_world_ver_ = wvers[0];
                    dp_world_pos_ = 0;
                }
            }
            for (const launcher::InfoRow &d : info_dp_)   // file = pack, loader = world folder ("" = no world yet), mc = world name, version = bytes, name = "1" when that world has it
            {
                if (!d.loader.empty() && d.loader != dp_world_)
                    continue;
                InstalledRow r;
                const bool has = d.name == "1";
                int in_worlds = 0;   // in how many worlds this pack is switched on
                for (const launcher::InfoRow &o : info_dp_)
                    in_worlds += o.file == d.file && o.name == "1" && !o.loader.empty() ? 1 : 0;
                r.title = strip_suffix(d.file);
                r.sub = launcher::size_text(std::atoll(d.version.c_str()));
                if (dp_world_count_ > 0)
                    r.sub += in_worlds == 0 ? "  \xC2\xB7  not switched on in any world"
                                            : "  \xC2\xB7  on in " + std::to_string(in_worlds) + " of " + std::to_string(dp_world_count_) + (dp_world_count_ == 1 ? " world" : " worlds");
                r.remove = "remove datapacks " + launcher::url_encode(d.file);
                if (!d.loader.empty())
                {
                    r.toggle = "toggle datapacks " + launcher::url_encode(d.file) + " " + launcher::url_encode(d.loader);
                    r.off = !has;
                    r.tags.emplace_back(has ? "On" : "Off", has ? green : grey);
                }
                r.detail = d.loader.empty() ? "Create a world first, then switch this pack on for it here. Delete removes it from the library."
                                            : "Switch on copies it into this world, switch off takes it out again. Delete only removes it from the library.";
                inst_rows_.push_back(std::move(r));
            }
        }
        else
        {
            const char *folder = inst_tab_ == 1 ? "mods" : inst_tab_ == 2 ? "resourcepacks" : "shaderpacks";
            std::vector<launcher::Item> items = store_.list_items(in, folder);
            if (inst_tab_ == 1)
            {
                const std::vector<launcher::Item> aside = store_.list_items(in, "mods-disabled");
                items.insert(items.end(), aside.begin(), aside.end());
            }
            for (const launcher::Item &it : items)
            {
                InstalledRow r;
                const std::map<std::string, launcher::InfoRow> &known = inst_tab_ == 1 ? info_mods_ : info_packs_;
                const auto found = inst_tab_ == 3 ? known.end() : known.find(it.name);
                const launcher::InfoRow *ni = found != known.end() ? &found->second : nullptr;
                const std::string file_title = strip_suffix(it.name);
                r.title = ni && !ni->name.empty() ? ni->name : file_title;
                r.sub = (ni && !ni->version.empty() ? (inst_tab_ == 1 ? "v" : "") + ni->version + "  \xC2\xB7  " : std::string()) +
                        (r.title != file_title ? file_title + "  \xC2\xB7  " : std::string()) + launcher::size_text(it.size);
                const char *dir = it.aside ? "mods-disabled" : folder;
                r.remove = std::string("remove ") + dir + " " + launcher::url_encode(it.name);
                if (inst_tab_ == 1 && !it.aside)
                {
                    r.toggle = "toggle mods " + launcher::url_encode(it.name);
                    r.off = it.disabled;
                }
                const std::string why = ni ? ni->why : std::string();
                if (ni && !ni->loader.empty())
                    r.tags.emplace_back(ni->loader, loader_color(ni->loader));
                if (ni && !ni->mc.empty())
                    r.tags.emplace_back(ni->mc, inst_tab_ == 1 ? (why.empty() ? green : amber) : grey);
                if (it.disabled)
                    r.tags.emplace_back("Switched off", amber);
                if (it.aside)
                    r.tags.emplace_back(ni && why.empty() ? "Returns at start" : "Set aside", ni && why.empty() ? green : grey);
                else if (!why.empty() && !it.disabled)
                    r.tags.emplace_back("Set aside at start", amber);
                if (inst_tab_ == 1)
                {
                    const std::string what = ni && !ni->name.empty() ? ni->name + (ni->version.empty() ? "" : " " + ni->version) : file_title;
                    if (it.aside)
                        r.detail = ni && !why.empty() ? "Set aside: " + why + ". It comes back by itself when you start a version it fits."
                                   : ni            ? what + " fits the selected version again: it is put back when the game starts."
                                                   : "Set aside because it is built for another Minecraft version. It comes back by itself when you start a version it fits.";
                    else if (it.disabled)
                        r.detail = "Switched off: the game ignores it until you switch it on again.";
                    else if (!why.empty())
                        r.detail = "Will be set aside when the game starts: " + why + ". It comes back by itself when it fits.";
                    else if (ni && !ni->mc.empty())
                        r.detail = what + (ni->loader.empty() ? "" : " for " + ni->loader) + ", Minecraft " + ni->mc + ". Changes apply the next time the game starts.";
                    else
                        r.detail = "A mod of " + in.name() + ". Changes apply the next time the game starts.";
                }
                else if (inst_tab_ == 2)
                    r.detail = std::string("A resource pack in ") + in.name() + (ni && !ni->mc.empty() ? ", made for Minecraft " + ni->mc + " (" + ni->version + ")." : ".");
                else
                    r.detail = "A shader pack in " + in.name() + ".";
                inst_rows_.push_back(std::move(r));
            }
        }
        inst_row_ = std::max(0, std::min(inst_row_, static_cast<int>(inst_rows_.size()) - 1));
    }

    void update_installed(const InputFrame &input, app::Feedback &feedback)
    {
        move_row(input, inst_row_, static_cast<int>(inst_rows_.size()), inst_scroll_, kModRowH, kListH);
        if (input.is_pressed(Action::page_next) || input.is_pressed(Action::page_prev))
        {
            inst_tab_ = (inst_tab_ + (input.is_pressed(Action::page_next) ? 1 : 5)) % 6;
            inst_row_ = 0;
            inst_scroll_.position.snap(0.0f);
            rebuild_installed();
            want_info();
            feedback.play(audio::Cue::tab);
        }
        if (inst_tab_ == 5 && dp_world_count_ > 1 && (input.is_pressed(Action::jump_prev) || input.is_pressed(Action::jump_next)))
        {
            std::vector<std::string> order;
            for (const launcher::InfoRow &d : info_dp_)
                if (!d.loader.empty() && std::find(order.begin(), order.end(), d.loader) == order.end())
                    order.push_back(d.loader);
            std::size_t at = 0;
            for (std::size_t i = 0; i < order.size(); ++i)
                if (order[i] == dp_world_)
                    at = i;
            at = (at + (input.is_pressed(Action::jump_next) ? 1 : order.size() - 1)) % order.size();
            dp_world_ = order[at];
            inst_row_ = 0;
            inst_scroll_.position.snap(0.0f);
            rebuild_installed();
            feedback.play(audio::Cue::tab);
        }
        if (input.is_pressed(Action::back))
        {
            close_sheet(feedback, false);
            return;
        }
        if (inst_rows_.empty() || inst_row_ >= static_cast<int>(inst_rows_.size()))
            return;
        const InstalledRow &r = inst_rows_[static_cast<std::size_t>(inst_row_)];
        if (input.is_pressed(Action::confirm))
        {
            if (!r.why.empty())
            {
                status("Cannot delete: " + r.why);
                refuse();
            }
            else if (r.world)
                ask("Delete \"" + r.name + "\"?",
                    "The world and everything in it (builds, inventories, maps) is deleted from " + target().name() + " and cannot be brought back. Folder: " + r.folder + ".",
                    r.remove, "Deleting " + r.name, false);
            else
                ask("Delete " + r.title + "?",
                    inst_tab_ == 0 ? version_delete_text(r.title)
                                   : "The file is removed from " + target().name() +
                                         " and cannot be brought back. You can install it again from Game Versions or Content.",
                    r.remove, "Deleting " + r.title, false);
        }
        else if (input.is_pressed(Action::west) && r.world)
        {
            kb_world_ = r.folder;
            kb_world_name_ = r.name;
            open_keyboard(3, r.name, "Rename world", 40);
        }
        else if (input.is_pressed(Action::west) && !r.toggle.empty() && submit_job(r.toggle))
        {
            feedback.play(audio::Cue::select);
            status((r.off ? "Switching on " : "Switching off ") + r.title);
        }
    }

    // ---- keyboard ----
    Rect kb_panel() const
    {
        if (kbm_)
            return {330, 300, 1260, 380};   // a plain text field with Done / Cancel
        return {330, 150, 1260, 780};
    }

    void open_keyboard(int target_kind, const std::string &text, const char *title, int max_length)
    {
        kb_target_ = target_kind;
        kb_text_ = text;
        kb_caret_ = text.size();
        kb_title_ = title;
        kb_max_ = max_length;
        kb_open_ = true;
        const Rect p{330, 150, 1260, 780};   // the on-screen keyboard's own panel (the plain text field of a real keyboard is smaller, see kb_panel)
        keyboard_.style.max_length = max_length;
        keyboard_.set_bounds({p.x + 60, p.y + 250, p.w - 120, 440});
        keyboard_.set_layout(0);
        keyboard_.set_shift(ui::KeyboardShift::off);
        keyboard_.set_length(static_cast<int>(kb_text_.size()));
        keyboard_.set_focus(2, 0);
        keyboard_.enter();
        cue(audio::Cue::open);
    }

    void update_keyboard(const InputFrame &input, float dt, app::Feedback &feedback)
    {
        keyboard_.set_length(static_cast<int>(kb_text_.size()));
        if (input.is_pressed(Action::page_prev) && kb_caret_ > 0)   // L1 / R1 move the cursor through the text
        {
            do
                --kb_caret_;
            while (kb_caret_ > 0 && (static_cast<unsigned char>(kb_text_[kb_caret_]) & 0xC0) == 0x80);
            feedback.play(audio::Cue::focus);
        }
        if (input.is_pressed(Action::page_next) && kb_caret_ < kb_text_.size())
        {
            do
                ++kb_caret_;
            while (kb_caret_ < kb_text_.size() && (static_cast<unsigned char>(kb_text_[kb_caret_]) & 0xC0) == 0x80);
            feedback.play(audio::Cue::focus);
        }
        const ui::Event ev = keyboard_.handle(input, feedback);
        keyboard_.update(dt);
        if (ev == ui::Event::activated)
            finish_keyboard(true);
        else if (ev == ui::Event::cancelled)
            finish_keyboard(false);
    }

    void finish_keyboard(bool accept)
    {
        kb_open_ = false;
        if (!accept)
        {
            cue(audio::Cue::back);
            return;
        }
        std::string text = kb_text_;
        while (!text.empty() && text.back() == ' ')
            text.pop_back();
        std::size_t lead = 0;
        while (lead < text.size() && text[lead] == ' ')
            ++lead;
        text.erase(0, lead);
        if (kb_target_ == 0)
        {
            launcher::Instance *in = focused_instance();
            if (in && !text.empty())
            {
                in->set("name", text);
                store_.save(*in);
                rebuild_keep();
                if (ui::FormRow *r = form_.row(kName))
                    r->label = "Rename: " + text;
                status("Renamed to " + text);
            }
        }
        else if (kb_target_ == 4)
        {
            bool ok = text.size() >= 3 && text.size() <= 16;
            for (const char c : text)
                ok = ok && (std::isalnum(static_cast<unsigned char>(c)) || c == '_');
            if (!ok)
            {
                status("A name is 3 to 16 letters, digits or underscores (no spaces)");
                refuse();
                return;
            }
            if (text == store_.username)
                return;
            if (jobs_.busy())   // an install is running: keep the name now, write it when the downloader is free
            {
                store_.username = text;
                pending_cmd_ = "config username " + text;
                status("Offline username: " + text + " (saved when the download ends)");
                if (mode_ == Mode::account)
                    build_account_form();
            }
            else if (submit_job("config username " + text))
                status("Offline username: " + text);
            return;
        }
        else if (kb_target_ == 3)
        {
            if (text.empty() || text == kb_world_name_)
                return;
            if (submit_job("world rename " + launcher::url_encode(kb_world_) + " " + launcher::url_encode(text)))
                status("Renaming the world to " + text);
            return;
        }
        else if (kb_target_ == 2)
        {
            install_query_ = text;
            install_row_ = 0;
            install_scroll_.position.snap(0.0f);
            rebuild_view();
            status(text.empty() ? "Search cleared" : "Showing versions matching \"" + text + "\"");
        }
        else
        {
            query_ = text;
            request_search();
        }
        cue(audio::Cue::select);
    }

    void update_error(const InputFrame &input, app::Feedback &feedback)
    {
        if (input.is_pressed(Action::confirm) || input.is_pressed(Action::back))
        {
            error_open_ = false;
            feedback.play(audio::Cue::back);
        }
    }

    // ---- launcher and account pages ----
    void style_form()
    {
        rd_form_ = false;
        form_.clear();
        form_.style = ui::FormStyle{};
        form_.style.theme = theme_;
        form_.style.reduced_motion = context_.settings.reduced_motion;
        form_.style.compact = true;
        form_.style.description_inline = false; // the help text is shown in full in the right column (a one-line fold cut it to "Memory i...")
        form_.style.control_width = 360.0f;
        form_.style.row_height = 62.0f;
        form_.style.header_height = 50.0f;
        form_.style.number_width = 130.0f;
    }

    void finish_form()
    {
        form_.set_bounds({sheet_rect().x + 48, sheet_rect().y + 112, 940, sheet_rect().h - 152});
        form_.set_focus(1);
        form_.enter();
    }

    static int ui_index(const std::string &ui)
    {
        return ui == "off" ? 1 : 0;
    }

    void build_launcher_form()
    {
        style_form();
        form_.add_header("Start-up");
        form_.add_choice(kStart, "When the game starts", {"Show launcher", "Skip launcher"}, ui_index(store_.config("ui")))
            .description = "Show launcher: this screen opens first. Skip launcher: the last instance starts right away and this screen no longer "
                           "appears. To see it again for one session, hold Square on the controller while the app starts (this setting stays on Skip launcher).";
        form_.add_choice(kOnExit, "When the game closes", {"Back to launcher", "Close the app"}, store_.config("onexit") == "app" ? 1 : 0).description =
            "Back to launcher: quitting Minecraft returns to this screen. Close the app: quitting Minecraft closes the app and returns to the PS5 home screen. "
            "A crash always returns here so its report can be shown. Applies at once.";
        form_.add_choice(kAutoplay, "Play again after a restart", {"Off", "On"}, store_.config("autoplay") == "on" ? 1 : 0).description =
            "Some games need the app to close and open again first (another Java, renderer or Forge). Off: after reopening, this screen shows and you press Play. "
            "On: the Play that needed the restart continues by itself once the app is open again (loading screen only). Applies at once.";
        form_.add_header("Memory");
        form_.add_header("Memory");
        {
            int mb = 3072;
            { const std::string m = store_.config("xmx"); const int v = std::atoi(m.c_str()); if (v > 0) mb = v; }
            mb = std::clamp(mb, 768, 8192);
            ui::FormRow &r = form_.add_slider(kMemory, "Memory", static_cast<float>(mb), 768, 8192, 256);
            r.unit = " MB";
            r.description = "Maximum Java heap for every game. 3 to 4 GB suits most modpacks. Above 4 GB the extra memory is only reserved and used when needed; "
                            "the console shares its memory with the graphics, so too much can leave Minecraft short of video memory. Applies the next time the game starts.";
        }
        form_.add_header("Stats overlay");
        form_.add_choice(kHud, "FPS and resolution", {"Off", "On"}, store_.config("hud") == "on" ? 1 : 0).description =
            "Shows the frame rate (with V-Sync), the resolution with its refresh rate and the renderer, in this launcher and in the game. The launcher shows it at once, the game the next time it starts.";
        form_.add_choice(kHudPos, "Overlay position", {"Top left", "Top center", "Top right", "Bottom left", "Bottom center", "Bottom right"}, hud_pos_index()).description =
            "Which corner or edge of the screen the overlay sits at.";
        form_.add_choice(kHudBg, "Overlay background", {"None", "Very light", "Light", "Medium", "Dark", "Solid"}, hud_bg_index()).description =
            "How see-through the panel behind the overlay text is. None leaves only the text.";
        form_.add_header("Sound");
        form_.add_choice(kSfx, "Sound effects", {"On", "Off"}, store_.config("sfx") == "off" ? 1 : 0).description =
            "The launcher's click, move and confirm sounds (Minecraft's own UI sounds). Off silences all of them. Applies at once.";
        form_.add_header("Mouse");
        form_.add_choice(kCursor, "Cursor size", {"Small", "Normal", "Large", "Larger", "Huge"}, cursor_index(store_.config("cursor"))).description =
            "The pointer of a USB mouse, in this launcher and in the game (menus, and the controller emulator's cursor). The preview shows the real size. Applies at once.";
        form_.add_header("Java");
        {
            const std::string jd = store_.config("jdk");
            const int cur = jd.empty() ? 0 : std::atoi(jd.c_str());
            jdk_opts_.assign(1, 0);
            std::vector<std::string> labels{"Automatic"};
            for (const int n : kJdks)
                if (jdk_carried(n))
                {
                    jdk_opts_.push_back(n);
                    labels.push_back("Java " + std::to_string(n));
                }
            int pos = 0;
            for (std::size_t i = 0; i < jdk_opts_.size(); ++i)
                if (jdk_opts_[i] == cur)
                    pos = static_cast<int>(i);
            if (cur && !pos) // a leftover jdk= line for a Java this build does not carry: keep it visible instead of silently rewriting it
            {
                jdk_opts_.push_back(cur);
                labels.push_back("Java " + std::to_string(cur) + " (not in this build)");
                pos = static_cast<int>(jdk_opts_.size()) - 1;
            }
            form_.add_choice(kJdk, "Java version", labels, pos).description = java_help();
        }
        if (ps5_running_renderer)   // merged title only: OpenGL, OpenGL via Zink and Vulkan in one app
        {
            render_opts_.clear();
            std::vector<std::string> labels;
            for (const int r : {1, 2, 0})
                if (!ps5_renderer_have || ps5_renderer_have(r))
                {
                    render_opts_.push_back(r);
                    labels.push_back(r == 1 ? "OpenGL" : r == 2 ? "OpenGL via Zink" : "Vulkan");
                }
            const std::string rc = store_.config("renderer");
            const int cur = rc == "vulkan" ? 0 : rc == "zink" ? 2 : rc == "gl" ? 1 : ps5_running_renderer();
            int pos = 0;
            for (std::size_t i = 0; i < render_opts_.size(); ++i)
                if (render_opts_[i] == cur)
                    pos = static_cast<int>(i);
            form_.add_header("Graphics");
            form_.add_choice(kRenderer, "Renderer", labels, pos).description =
                "OpenGL: the console's OpenGL. OpenGL via Zink: OpenGL on the Vulkan driver (Mesa), best for new versions. Vulkan: Minecraft's own Vulkan "
                "backend (26.x). Vulkan runs one game at a time: Play closes the app, open it again and the game starts on Vulkan; the launcher is back the start after. "
                "Changing between the OpenGL ones applies after the app is opened again.";
            {
                const std::string hz = store_.config("hz");
                form_.add_choice(kRefresh, "Refresh rate", {"120 Hz", "60 Hz"}, hz == "60" ? 1 : 0).description =
                    "Output refresh rate of the game. 120 Hz needs the console set to 120 Hz output; if it does not run 120 Hz the game falls back to 60 Hz by itself. "
                    "Applies at once, no restart.";
            }
            {
                const std::string vs = store_.config("vsync");
                form_.add_choice(kVsync, "V-Sync", {"Minecraft decides", "Always on"}, vs == "always" ? 1 : 0).description =
                    "Minecraft decides: the V-Sync option in Minecraft's Video Settings really works. Off there lets the game run above the display rate (tearing is possible), on keeps frames locked. "
                    "Always on: frames are always locked to the display and the in-game option does nothing. Applies the next time the game starts.";
            }
            {
                const std::string rs = store_.config("resolution");
                form_.add_choice(kResolution, "Resolution", {"1080p", "1440p (2K)", "2160p (4K)"}, rs == "2160" ? 2 : rs == "1440" ? 1 : 0).description =
                    std::string("Screen size of the game and of its mouse and keyboard overlay: both use the same size. Needs the Vulkan or Zink renderer and a console output that offers the mode "
                    "(set the console to 4K or 1440p); otherwise the game stays at 1080p. Higher is sharper but slower. Applies after the app is closed and opened again.") +
                    (ps5_running_renderer() == 1 ? " This start runs the OpenGL (SDK) renderer: it is always 1080p, whatever is chosen here." : "");
            }
        }
        form_.add_header("This install");
        form_.add_value(kInfo1, "Game folder", store_.root.empty() ? "(preview)" : store_.root);
        form_.add_value(kInfo2, "Instances", std::to_string(store_.instances.size()));
        form_.add_value(kInfo3, "Installed versions", std::to_string(store_.versions.size()));
        {
            std::string have;
            for (const int n : kJdks)
                if (jdk_carried(n))
                    have += (have.empty() ? "" : ", ") + std::to_string(n);
            form_.add_value(kInfo4, "Java in this build", have.empty() ? std::string("17") : have);
            const int running = ps5_running_jdk ? ps5_running_jdk() : 0;
            form_.add_value(kInfo5, "Java running now", running ? "Java " + std::to_string(running) : std::string("(preview)"));
        }
        finish_form();
    }

    static constexpr int kJdks[4] = {8, 17, 21, 25};

    // Does this build link that Java? (jvm_host.cpp: JDK_MASK; the PC preview pretends all four.)
    static bool jdk_carried(int n)
    {
        return ps5_java_for ? ps5_java_for(n, 0) == n : true;
    }

    // Which Java "Automatic" gives which Minecraft versions, for the Java this build carries (an older Java is never picked for a version that needs a newer one).
    std::string java_help() const
    {
        struct Bucket
        {
            int need;
            const char *from, *to;
        };
        static const Bucket buckets[] = {{8, "", "1.16.5"}, {17, "1.17", "1.20.4"}, {21, "1.20.5", "1.21.x"}, {25, "26.x", ""}};
        std::vector<int> have;
        for (const int n : kJdks)
            if (jdk_carried(n))
                have.push_back(n);
        if (have.empty())
            have.push_back(17);
        struct Group
        {
            int java;
            std::string from, to;
        };
        std::vector<Group> groups;
        for (const Bucket &b : buckets)
        {
            int pick = have.back();
            for (const int h : have)
                if (h >= b.need)
                {
                    pick = h;
                    break;
                }
            if (!groups.empty() && groups.back().java == pick)
                groups.back().to = b.to;
            else
                groups.push_back({pick, b.from, b.to});
        }
        std::string list;
        for (const Group &g : groups)
        {
            std::string range = g.from.empty() && g.to.empty() ? std::string("everything")
                                : g.from.empty()              ? "up to " + g.to
                                : g.to.empty()                ? (g.from == "26.x" ? g.from : g.from + " and newer")
                                                              : g.from + " to " + g.to;
            list += (list.empty() ? "" : "  ") + std::string("Java ") + std::to_string(g.java) + ": " + range + ".";
        }
        std::string text = "Automatic gives each version the Java it needs. " + list +
                           " A fixed Java applies to every instance but never goes below what a version needs. Applies when the app is reopened.";
        const int need = selected_need();
        if (need)
            text += " The selected version needs Java " + std::to_string(java_tier(need)) + ".";
        return text;
    }

    void commit_launcher()
    {
        const int idx = form_.choice_index(kStart);
        const bool ui_changed = (idx == 0 || idx == 1) && idx != ui_index(store_.config("ui"));   // never writes ui=off from an unreadable row
        const std::size_t ji = static_cast<std::size_t>(form_.choice_index(kJdk));
        const std::string jd_new = ji < jdk_opts_.size() ? (jdk_opts_[ji] ? std::to_string(jdk_opts_[ji]) : std::string()) : store_.config("jdk");
        const bool jdk_changed = jd_new != store_.config("jdk");
        {
            const bool off = form_.choice_index(kSfx) == 1;
            if (off != (store_.config("sfx") == "off"))
            {
                store_.set_config("sfx", off ? "off" : "on");
                hui_sfx_muted = off;
            }
        }
        {
            const std::string cs = std::to_string(2 + std::clamp(form_.choice_index(kCursor), 0, 4));
            if (cs != store_.config("cursor") && !(cs == "3" && store_.config("cursor").empty()))
                store_.set_config("cursor", cs);
            cur_scale_ = static_cast<float>(2 + std::clamp(form_.choice_index(kCursor), 0, 4));
        }
        {
            const bool app = form_.choice_index(kOnExit) == 1;
            if (app != (store_.config("onexit") == "app"))
            {
                store_.set_config("onexit", app ? "app" : "launcher");
                status(app ? "The app closes when the game closes" : "The launcher comes back when the game closes");
            }
            const bool hud = form_.choice_index(kHud) == 1;
            if (hud != (store_.config("hud") == "on"))
                store_.set_config("hud", hud ? "on" : "off");
            static const char *const pos[6] = {"tl", "tc", "tr", "bl", "bc", "br"};
            const std::string np = pos[std::clamp(form_.choice_index(kHudPos), 0, 5)];
            if (np != store_.config("hudpos") && !(np == "tl" && store_.config("hudpos").empty()))
                store_.set_config("hudpos", np);
            const std::string nb = std::to_string(std::clamp(form_.choice_index(kHudBg), 0, 5));
            if (nb != store_.config("hudbg") && !(nb == "4" && store_.config("hudbg").empty()))
                store_.set_config("hudbg", nb);
        }
        {
            const int mb = static_cast<int>(form_.slider_value(kMemory));
            if (std::to_string(mb) != store_.config("xmx") && !(mb == 3072 && store_.config("xmx").empty()))
            {
                store_.set_config("xmx", std::to_string(mb));
                status("Memory saved: " + std::to_string(mb) + " MB, applies the next time the game starts");
            }
        }
        {
            const bool on = form_.choice_index(kAutoplay) == 1;
            if (on != (store_.config("autoplay") == "on"))
                store_.set_config("autoplay", on ? "on" : "off");
        }
        if (ui_changed)
            store_.set_config("ui", idx == 0 ? "native" : "off");
        if (jdk_changed)
            store_.set_config("jdk", jd_new);
        if (ps5_running_renderer && !render_opts_.empty())
        {
            const std::size_t ri = static_cast<std::size_t>(form_.choice_index(kRenderer));
            if (ri < render_opts_.size())
            {
                const int r = render_opts_[ri];
                const std::string nv = r == 0 ? "vulkan" : r == 2 ? "zink" : "gl";
                if (nv != store_.config("renderer"))
                {
                    store_.set_config("renderer", nv);
                    status(r == 0 ? "Vulkan selected: Play closes the app, open it again to start the game on Vulkan"
                                  : r == ps5_running_renderer() ? std::string("Renderer saved")
                                                                : std::string("Renderer saved: press Play, the app closes, open it again"));
                }
            }
        }
        if (ps5_running_renderer)
        {
            const std::string nh = form_.choice_index(kRefresh) == 1 ? "60" : "120";
            const std::string oh = store_.config("hz").empty() ? "120" : store_.config("hz");
            if (nh != oh)
            {
                store_.set_config("hz", nh);
                if (ps5_set_hz)
                    ps5_set_hz(nh == "60" ? 60 : 120);
                status("Refresh rate " + nh + " Hz: applied now");
            }
        }
        if (ps5_running_renderer)
        {
            const std::string nv = form_.choice_index(kVsync) == 1 ? "always" : "minecraft";
            if (nv != (store_.config("vsync").empty() ? std::string("minecraft") : store_.config("vsync")))
            {
                store_.set_config("vsync", nv);
                status(std::string("V-Sync: ") + (nv == "always" ? "always on" : "Minecraft decides") + ". Applies the next time the game starts");
            }
        }
        if (ps5_running_renderer)
        {
            const int ri = form_.choice_index(kResolution);
            const std::string nr = ri == 2 ? "2160" : ri == 1 ? "1440" : "1080";
            const std::string old = store_.config("resolution").empty() ? std::string("1080") : store_.config("resolution");
            if (nr != old)
            {
                store_.set_config("resolution", nr);
                status(std::string("Resolution ") + (ri == 2 ? "2160p" : ri == 1 ? "1440p" : "1080p") + ": close the app and open it again to apply");
            }
        }
        const std::string rn = restart_note();
        if (jdk_changed)
            status(rn.empty() ? "Java setting saved: " + (jd_new.empty() ? std::string("automatic") : "Java " + jd_new) + " (no restart needed)" : rn);
        else if (ui_changed)
            status(idx == 0 ? "The launcher shows at start-up" : "The launcher is skipped from the next start");
    }

    void build_account_form()
    {
        style_form();
        form_.style.focus_values = true; // read-only rows take the focus and are dimmed; without a focused row the form ignored every button, Back included
        const bool signed_in = !(store_.account == "offline" || store_.account.empty());
        form_.add_header("Playing as");
        form_.add_value(kAccount, "Account", account_label()).description =
            signed_in ? "Signed in with Microsoft: online play and servers that check accounts work." : "Offline play: single player, LAN, and servers that allow offline accounts.";
        form_.add_action(kUsername, "Offline username: " + store_.username).description =
            "The name you play under offline: 3 to 16 letters, digits or underscores. A different name is a different player in worlds that already exist.";
        form_.add_header("Microsoft");
        form_.add_action(kSignIn, signed_in ? "Sign in with another account" : "Sign in with Microsoft").description =
            "Shows a code. Open the page on a phone or PC, type the code and sign in with the account that owns Minecraft: Java Edition. Takes a minute.";
        form_.add_value(kInfo1, "Sign-in code", "None").description = "Appears here while you sign in.";
        form_.add_value(kInfo2, "Open this page", "None").description = "The page to open on a phone or PC.";
        {   // saved accounts: the way back to online play after "Play offline instead" (and a switch between several accounts)
            int slot = 0;
            for (const auto &acc : saved_accounts())
            {
                if (acc.first == store_.account || slot >= 4)
                    continue;
                form_.add_action(kUse0 + slot, "Play online as " + acc.second).description = "Switch to this saved Microsoft account. No new sign-in is needed.";
                ++slot;
            }
        }
        if (signed_in)
        {
            form_.add_action(kOffline, "Play offline instead").description = "Switch back to the offline account. Your Microsoft sign-in stays saved.";
            form_.add_action(kSignOut, "Sign out of " + account_label()).description = "Forgets this Microsoft account on this console (its sign-in and saved tokens). You play offline until you sign in again.";
        }
        finish_form();
        login_shown_.clear();
        login_poll_ = 0.0f;
    }

    // ---- drawing of the tool pages ----
    static std::vector<std::string> split_csv(const std::string &s)
    {
        std::vector<std::string> out;
        for (std::size_t at = 0; at < s.size();)
        {
            const std::size_t e = s.find(',', at);
            const std::string part = s.substr(at, e == std::string::npos ? std::string::npos : e - at);
            if (!part.empty())
                out.push_back(part);
            at = e == std::string::npos ? s.size() : e + 1;
        }
        return out;
    }

    static std::string count_text(const std::string &s)
    {
        if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
            return s;
        const double n = std::atof(s.c_str());
        char b[32];
        if (n >= 1e6)
            std::snprintf(b, sizeof b, "%.1fM", n / 1e6);
        else if (n >= 1e3)
            std::snprintf(b, sizeof b, "%.0fK", n / 1e3);
        else
            std::snprintf(b, sizeof b, "%.0f", n);
        return b;
    }

    // A small rounded tag ending at right_x; returns its left edge.
    float draw_tag(gfx::DrawList &list, float right_x, float cy, const std::string &label, Color tint) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float w = fonts.semibold.font->measure(label, 18.0f) + 26.0f;
        const Rect r{right_x - w, cy - 16, w, 32};
        list.bordered_rect(r, 16, tint.with_alpha(0.18f), 1.5f, tint.with_alpha(0.6f));
        ui::text(list, fonts.semibold, label, r.cx(), cy + 6, 18, tint, gfx::Align::center);
        return r.x - 8;
    }

    float draw_tabs(gfx::DrawList &list, const Rect &sheet, const char *const *names, int count, int active,
                    const Color &accent) const
    {
        const ui::Fonts &fonts = context_.fonts;
        float x = sheet.x + 56;
        float ax = x, aw = 0.0f;
        for (int i = 0; i < count; ++i)
        {
            const float w = fonts.semibold.font->measure(names[i], 22.0f) + 44.0f;
            const Rect r{x, sheet.y + 140, w, 46};
            draw_hover(list, r, 23, true);
            if (i == active)
            {
                ax = r.x;
                aw = r.w;
            }
            else
                list.bordered_rect(r, 23, kWhite.with_alpha(0.05f), 1.0f, kWhite.with_alpha(0.14f));
            x += w + 12;
        }
        // the glass pill under the active tab glides (and overshoots a little) from the one before
        tab_goal_x_ = ax;
        tab_goal_w_ = aw;
        const Rect pill{tab_x_.value, sheet.y + 140, std::max(40.0f, tab_w_.value), 46};
        glass_panel(list, pill, 23, accent, 0.5f, 1.5f);
        list.bordered_rect(pill, 23, Color::rgb(0x000000, 0.0f), 1.5f, accent.with_alpha(0.75f));
        x = sheet.x + 56;
        for (int i = 0; i < count; ++i)
        {
            const float w = fonts.semibold.font->measure(names[i], 22.0f) + 44.0f;
            ui::text(list, fonts.semibold, names[i], x + w * 0.5f, sheet.y + 140 + 23 + 8, 22,
                     i == active ? kWhite : kWhite.with_alpha(0.72f), gfx::Align::center);
            x += w + 12;
        }
        return x;
    }

    // Steps the springs behind the sliding glass: the tab pill and the highlight under the focused list row.
    void update_glass_ui(float dt)
    {
        const bool lists = sheet_open_ && !kb_open_ && (mode_ == Mode::install || mode_ == Mode::mods || mode_ == Mode::installed);
        const int key = (static_cast<int>(mode_) << 4) | (mode_ == Mode::install ? install_tab_ : mode_ == Mode::mods ? mods_tab_ : inst_tab_);
        const bool snap = !sheet_open_ || key != glass_key_ || context_.settings.reduced_motion || sheet_t_.value < 0.5f;
        glass_key_ = key;
        tab_x_.target = tab_goal_x_;
        tab_w_.target = tab_goal_w_;
        float rel = 0.0f;
        if (lists)
            rel = mode_ == Mode::install ? static_cast<float>(install_row_) * kRowH - install_scroll_.offset()
                  : mode_ == Mode::mods  ? static_cast<float>(mods_row_) * kModRowH - mods_scroll_.offset()
                                         : static_cast<float>(inst_row_) * kModRowH - inst_scroll_.offset();
        hl_y_.target = rel;
        dp_hl_.target = static_cast<float>(dp_world_pos_ - dp_window_start()) * 84.0f;
        if (snap || (lists && inst_tab_ != 5 && mode_ == Mode::installed))
            ;
        if (snap)
        {
            dp_hl_.snap(dp_hl_.target);
            tab_x_.snap(tab_goal_x_);
            tab_w_.snap(tab_goal_w_);
            hl_y_.snap(rel);
            return;
        }
        tab_x_.update(dt, 21.0f, 0.62f);
        tab_w_.update(dt, 21.0f, 0.62f);
        hl_y_.update(dt, 26.0f, 0.72f);
        dp_hl_.update(dt, 24.0f, 0.7f);
    }

    // The glass pill behind the focused row of a list: it slides between rows instead of jumping.
    void draw_row_highlight(gfx::DrawList &list, const Rect &view, float row_h, const Color &accent, bool any) const
    {
        if (!any)
            return;
        const Rect r{view.x, view.y + hl_y_.value, view.w, row_h - 8};
        glass_panel(list, r, 18, accent, 0.0f, 1.5f);
        list.bordered_rect(r, 18, Color::rgb(0x000000, 0.0f), 2.0f, kWhite.with_alpha(0.82f));
    }

    void draw_action_line(gfx::DrawList &list, ui::Button b, const std::string &text, float x, float y,
                          const ui::GlyphStyle &style) const
    {
        const ui::Fonts &fonts = context_.fonts;
        act_hits_.push_back({{x, y - 30.0f, 460.0f, 46.0f}, b});
        draw_hover(list, {x - 6.0f, y - 30.0f, 472.0f, 46.0f}, 16, true);
        ui::draw_button(list, fonts, style, b, x + 18, y, 34);
        const float tx = 28.0f + ui::button_width(b, 34.0f);   // the glyph is a keycap ("Enter") while the keyboard is in use: the label starts after it, never under it
        ui::text_shrink(list, fonts.regular, text, x + tx, y + 8, 22, 16, 460.0f - tx, kWhite.with_alpha(0.9f));   // the right column is 460 wide: a long label shrinks, never runs out of the sheet
    }

    // Draws text word-wrapped at the largest size in [size_min, size_max] that fits in `prefer` lines; when even size_min
    // needs more, it uses size_min with up to max_lines lines. Returns the baseline of the last line drawn.
    float fitted_paragraph(gfx::DrawList &list, const ui::FontRef &font, const std::string &text, float x, float y,
                           float size_max, float size_min, float width, int prefer, int max_lines, Color color) const
    {
        float size = size_max;
        while (size > size_min && static_cast<int>(font.font->wrap(text, size, width).size()) > prefer)
            size -= 2.0f;
        const int lines = static_cast<int>(font.font->wrap(text, size, width).size());
        const float pitch = size * 1.2f;
        ui::paragraph(list, font, text, x, y, size, width, pitch, color, lines > prefer ? max_lines : prefer);
        return y + pitch * static_cast<float>(std::max(1, std::min(lines, lines > prefer ? max_lines : prefer)) - 1);
    }

    void draw_install(gfx::DrawList &list, const Rect &sheet, const Entry &e) const
    {
        const ui::Fonts &fonts = context_.fonts;
        static const char *tabs[] = {"Releases", "Snapshots", "Old versions"};
        draw_tabs(list, sheet, tabs, 3, install_tab_, e.accent);
        {
            std::string f = install_newest_ ? "Newest first" : "Oldest first";
            if (!install_query_.empty())
                f += "  \xC2\xB7  \"" + install_query_ + "\"";
            f = fonts.regular.font->fit(f, 22.0f, 520.0f);
            const float fw = fonts.regular.font->measure(f, 22.0f);
            ui::text(list, fonts.regular, f, sheet.x + sheet.w - 56, sheet.y + 118, 22, kWhite.with_alpha(0.7f), gfx::Align::right);
            ui::text_shrink(list, fonts.regular, target_line(), sheet.x + 56, sheet.y + 118, 24, 24*0.62f, sheet.w - 112 - fw - 40, kWhite.with_alpha(0.78f));
        }

        const Rect view{sheet.x + 56, sheet.y + 214, 1040, kListH};
        list.push_clip(view.inset(-4));
        draw_row_highlight(list, view, kRowH, e.accent, !view_.empty());
        if (view_.empty())
        {
            const bool wait = jobs_.busy();
            ui::text(list, fonts.regular,
                     wait ? "Loading the version list..." : !install_query_.empty() ? "No version matches your search. Press L2 to change it." : "No versions listed. Press Options to load the list.",
                     view.x + 16, view.y + 60, 26, kWhite.with_alpha(0.7f));
        }
        for (int i = 0; i < static_cast<int>(view_.size()); ++i)
        {
            const float y = view.y + static_cast<float>(i) * kRowH - install_scroll_.offset();
            if (y > view.y + view.h || y + kRowH < view.y)
                continue;
            const launcher::VersionRow &v = rows_[static_cast<std::size_t>(view_[static_cast<std::size_t>(i)])];
            const Rect row{view.x, y, view.w, kRowH - 8};
            const bool focus = i == install_row_;
            if (!focus)
                list.bordered_rect(row, 18, kWhite.with_alpha(0.05f), 1.0f, kWhite.with_alpha(0.08f));
            ui::text_shrink(list, fonts.semibold, v.id, row.x + 26, row.cy() + 11, 30, 30*0.62f, 236.0f, kWhite.with_alpha(focus ? 1.0f : 0.88f));
            ui::text_shrink(list, fonts.regular, v.date, row.x + 270, row.cy() + 8, 22, 22*0.62f, row.w - 296.0f, kWhite.with_alpha(0.6f));
            float rx = row.x + row.w - 20;
            if (installed(v.id))
                rx = draw_tag(list, rx, row.cy(), "Installed", Color::rgb(0x6fe6a3));
            if (has_loader("quilt", v.id))
                rx = draw_tag(list, rx, row.cy(), "Quilt", Color::rgb(0xc09bff));
            if (has_loader("fabric", v.id))
                rx = draw_tag(list, rx, row.cy(), "Fabric", Color::rgb(0x7fb4ff));
            if (has_loader("forge", v.id))
                rx = draw_tag(list, rx, row.cy(), "Forge", Color::rgb(0xffb070));
            if (has_loader("neoforge", v.id))
                rx = draw_tag(list, rx, row.cy(), "NeoForge", Color::rgb(0xf0a030));
        }
        list.pop_clip();
        draw_scrollbar(list, view, static_cast<int>(view_.size()), kRowH, install_scroll_);

        // Right column: what the buttons do for the focused version.
        const float x = sheet.x + sheet.w - 56 - 460;
        const ui::GlyphStyle style = ui::GlyphStyle::dark();
        std::string id = "-";
        std::string kind;
        if (!view_.empty() && install_row_ < static_cast<int>(view_.size()))
        {
            const launcher::VersionRow &v = rows_[static_cast<std::size_t>(view_[static_cast<std::size_t>(install_row_)])];
            id = v.id;
            kind = v.type == "release" ? "Release" : v.type == "snapshot" ? "Snapshot" : "Old version";
            if (!v.date.empty())
                kind += "  \xC2\xB7  " + v.date;
        }
        {
            float size = 64.0f;   // long ids ("1.RV-Pre1", "24w14potato") shrink instead of running out of the sheet
            while (size > 32.0f && fonts.display.font->measure(id, size) > 460.0f)
                size -= 4.0f;
            ui::text_shrink(list, fonts.display, id, x, sheet.y + 250, size, size*0.62f, 460.0f, kWhite);
        }
        ui::text_shrink(list, fonts.regular, kind, x, sheet.y + 294, 24, 24*0.62f, 460.0f, kWhite.with_alpha(0.7f));
        draw_action_line(list, ui::Button::cross, "Install and use for this instance", x, sheet.y + 360, style);
        draw_action_line(list, ui::Button::square, "Install, then add Fabric", x, sheet.y + 408, style);
        draw_action_line(list, ui::Button::triangle, "Install, then add Quilt", x, sheet.y + 452, style);
        draw_action_line(list, ui::Button::left_stick, "Install, then add Forge", x, sheet.y + 496, style);
        draw_action_line(list, ui::Button::right_stick, "Install, then add NeoForge", x, sheet.y + 540, style);
        ui::paragraph_box(list, fonts.regular,
                          "Game files, libraries, assets and the Java it needs are downloaded. Progress shows at the bottom; "
                          "you can keep browsing meanwhile.",
                          x, sheet.y + 602, 21, 14, 460, sheet.y + 770, kWhite.with_alpha(0.62f));
    }

    // One line for a list row: the whole text when it fits at 17 px or more, else its first sentence, else cut at a word with an ellipsis; the right column always shows the full text.
    std::string row_summary(const std::string &text, float width) const
    {
        const auto &f = *context_.fonts.regular.font;
        std::string t;
        for (const char c : text)   // line breaks and runs of spaces make one space
            if (c == '\n' || c == '\r' || c == '\t' || c == ' ')
            {
                if (!t.empty() && t.back() != ' ')
                    t += ' ';
            }
            else
                t += c;
        while (!t.empty() && t.back() == ' ')
            t.pop_back();
        if (f.measure(t, 17.0f) <= width)
            return t;
        for (std::size_t i = 0; i + 1 < t.size(); ++i)
            if ((t[i] == '.' || t[i] == '!' || t[i] == '?') && t[i + 1] == ' ' && i > 12 && f.measure(t.substr(0, i + 1), 17.0f) <= width)
            {
                std::string first = t.substr(0, i + 1);
                for (std::size_t j = i + 2; j + 1 < t.size(); ++j)   // take more whole sentences while they still fit
                    if ((t[j] == '.' || t[j] == '!' || t[j] == '?') && t[j + 1] == ' ' && f.measure(t.substr(0, j + 1), 17.0f) <= width)
                        first = t.substr(0, j + 1);
                return first;
            }
        std::string cut = t;
        while (cut.size() > 8 && f.measure(cut + "\xE2\x80\xA6", 17.0f) > width)
        {
            const std::size_t sp = cut.find_last_of(' ');
            cut.erase(sp == std::string::npos || sp < 8 ? cut.size() - 1 : sp);
        }
        return cut + "\xE2\x80\xA6";
    }

    void draw_mods(gfx::DrawList &list, const Rect &sheet, const Entry &e) const
    {
        const ui::Fonts &fonts = context_.fonts;
        ui::text_shrink(list, fonts.regular, target_line(), sheet.x + 56, sheet.y + 118, 24, 24*0.62f, sheet.w - 112, kWhite.with_alpha(0.78f));
        static const char *tabs[] = {"Mods", "Resource packs", "Shaders", "Datapacks", "Modpacks"};
        const float tabs_end = draw_tabs(list, sheet, tabs, 5, mods_tab_, e.accent);
        // The current search, right of the tabs.
        {
            const std::string q = query_.empty() ? "Top downloads" : "\"" + query_ + "\"";
            const Rect box{tabs_end + 8, sheet.y + 140, sheet.x + 56 + 1040 - (tabs_end + 8), 46};   // the rest of the row right of the tabs
            list.bordered_rect(box, 23, kWhite.with_alpha(0.06f), 1.5f, kWhite.with_alpha(0.22f));
            ui::draw_button(list, fonts, ui::GlyphStyle::dark(), ui::Button::square, box.x + 26, box.cy(), 30);
            ui::text_shrink(list, fonts.regular, q, box.x + 66, box.cy() + 8, 22, 22*0.62f, box.w - 90, kWhite.with_alpha(0.85f));
        }

        const Rect view{sheet.x + 56, sheet.y + 214, 1040, kListH};
        list.push_clip(view.inset(-4));
        draw_row_highlight(list, view, kModRowH, e.accent, !results_.empty());
        if (results_.empty())
        {
            const bool wait = jobs_.busy() || search_pending_;
            ui::text(list, fonts.regular, wait ? "Searching Modrinth..." : "No results. Press Square to search for something else.",
                     view.x + 16, view.y + 60, 26, kWhite.with_alpha(0.7f));
        }
        for (int i = 0; i < static_cast<int>(results_.size()); ++i)
        {
            const float y = view.y + static_cast<float>(i) * kModRowH - mods_scroll_.offset();
            if (y > view.y + view.h || y + kModRowH < view.y)
                continue;
            const launcher::SearchRow &r = results_[static_cast<std::size_t>(i)];
            const Rect row{view.x, y, view.w, kModRowH - 8};
            const bool focus = i == mods_row_;
            if (!focus)
                list.bordered_rect(row, 18, kWhite.with_alpha(0.05f), 1.0f, kWhite.with_alpha(0.08f));
            float rx = row.x + row.w - 20;
            const float min_title = 300.0f; // the name keeps this much room; tags that do not fit are left out
            const auto room = [&](const std::string &label) {
                return rx - (fonts.semibold.font->measure(label, 18.0f) + 26.0f) > row.x + 26 + min_title;
            };
            if (slug_installed(r.slug))
                rx = draw_tag(list, rx, row.y + 30, "Installed", Color::rgb(0x6fe6a3));
            if (!r.span.empty() && room(r.span))
                rx = draw_tag(list, rx, row.y + 30, r.span, Color::rgb(0xa8b4cf));
            if (mods_tab_ == 0)
                for (const std::vector<std::string> ls = split_csv(r.loaders); const std::string &l : std::vector<std::string>(ls.rbegin(), ls.rend()))
                {
                    const std::string label = loader_index(l) > 0 ? loader_name(l) : std::string();
                    if (!label.empty() && room(label))
                        rx = draw_tag(list, rx, row.y + 30, label, loader_color(label));
                }
            const float tw = rx - (row.x + 26) - 10;
            ui::text_shrink(list, fonts.semibold, r.title, row.x + 26, row.y + 38, 28, 28*0.62f, tw, kWhite.with_alpha(focus ? 1.0f : 0.9f));
            ui::text_shrink(list, fonts.regular, row_summary(r.description, row.w - 52), row.x + 26, row.y + 72, 20, 17, row.w - 52, kWhite.with_alpha(0.62f));
        }
        list.pop_clip();
        draw_scrollbar(list, view, static_cast<int>(results_.size()), kModRowH, mods_scroll_);

        const float x = sheet.x + sheet.w - 56 - 460;
        const ui::GlyphStyle style = ui::GlyphStyle::dark();
        if (!results_.empty() && mods_row_ < static_cast<int>(results_.size()))
        {
            const launcher::SearchRow &r = results_[static_cast<std::size_t>(mods_row_)];
            // Two lines at up to 44 pt; a long name ("Complementary Shaders - Reimagined") shrinks, then takes a third line.
            const float last = fitted_paragraph(list, fonts.display, r.title, x, sheet.y + 262, 44, 28, 460, 2, 3, kWhite);
            const float by = last + 50.0f;
            ui::text(list, fonts.regular, "by " + r.author + "  \xC2\xB7  " + count_text(r.downloads) + " downloads", x,
                     by, 22, kWhite.with_alpha(0.7f));
            std::string works;
            if (mods_tab_ == 0 && !r.loaders.empty())
                for (const std::string &l : split_csv(r.loaders))
                    if (loader_index(l) > 0)
                        works += (works.empty() ? "" : ", ") + loader_name(l);
            if (!r.span.empty())
                works += (works.empty() ? "" : "  \xC2\xB7  ") + std::string("Minecraft ") + r.span;
            if (!works.empty())
                ui::text_shrink(list, fonts.regular, works, x, by + 34, 20, 20*0.62f, 460, kWhite.with_alpha(0.6f));
            ui::paragraph_box(list, fonts.regular, r.description, x, by + (works.empty() ? 48 : 80), 21, 14, 460, sheet.y + 590, kWhite.with_alpha(0.8f));
        }
        draw_action_line(list, ui::Button::cross, (mods_tab_ == 4 ? "Install as a new instance" : mods_tab_ == 3 ? "Install to the library" : "Install (required mods come along)"), x, sheet.y + 610, style);
        if (mods_tab_ == 0 && target().loader().empty())
            ui::paragraph_box(list, fonts.regular,
                              "This instance has no mod loader yet. Install Fabric or Quilt first (Game Versions, then Square or Triangle on a version).",
                              x, sheet.y + 662, 20, 14, 460, sheet.y + 770, Color::rgb(0xffb86b));
    }

    // A world's picture (its icon.png) or, without one, a green tile with the first letter of its name.
    void draw_world_picture(gfx::DrawList &list, const InstalledRow &r, const Rect &box, float radius) const
    {
        if (r.tex)
        {
            list.image(r.tex, box, gfx::kFullUv, kWhite, radius);
            list.bordered_rect(box, radius, Color::rgb(0x000000, 0.0f), 1.5f, kWhite.with_alpha(0.25f));
            return;
        }
        if (unknown_tex_)
        {
            list.image(unknown_tex_, box, gfx::kFullUv, kWhite, radius);
            list.bordered_rect(box, radius, Color::rgb(0x000000, 0.0f), 1.5f, kWhite.with_alpha(0.25f));
            return;
        }
        list.gradient_rect(box, radius, Color::rgb(0x3f8f4e), Color::rgb(0x1a3d25));
        std::string initial = r.title.empty() ? std::string("?") : r.title.substr(0, 1);
        for (std::size_t n = 1; n < r.title.size() && (static_cast<unsigned char>(r.title[n]) & 0xC0) == 0x80; ++n)
            initial += r.title[n];
        ui::text(list, context_.fonts.display, initial, box.cx(), box.cy() + box.h * 0.17f, box.h * 0.5f, kWhite.with_alpha(0.85f), gfx::Align::center);
        list.bordered_rect(box, radius, Color::rgb(0x000000, 0.0f), 1.5f, kWhite.with_alpha(0.25f));
    }

    // Datapacks tab, right column: every world with its picture, name, version, mode and how many packs are on in it; L2 / R2 picks
    // one (the glass highlight slides to it). Returns where the list ends, relative to the sheet.
    int dp_window_start() const
    {
        const int n = static_cast<int>(dp_worlds_.size());
        const int vis = std::min(3, n);
        return std::clamp(dp_world_pos_ - 1, 0, std::max(0, n - vis));
    }

    float draw_dp_worlds(gfx::DrawList &list, const Rect &sheet, float x) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const ui::GlyphStyle style = ui::GlyphStyle::dark();
        const int n = static_cast<int>(dp_worlds_.size());
        ui::text(list, fonts.semibold, "SWITCH PACKS ON FOR", x, sheet.y + 236, 17, kWhite.with_alpha(0.62f), gfx::Align::left, 3.0f);
        if (n == 0)
        {
            const Rect box{x, sheet.y + 254, 460, 120};
            glass_panel(list, box, 20, kWhite, 0.0f, 0.8f);
            ui::paragraph_box(list, fonts.regular, "No worlds in this instance yet. Play it once and create a world, then switch packs on for it here.", box.x + 22,
                              box.y + 40, 20, 14, box.w - 44, box.y + box.h - 14, kWhite.with_alpha(0.8f));
            return 254.0f + 120.0f;
        }
        if (n > 1)
        {
            const std::string cnt = std::to_string(dp_world_pos_ + 1) + " / " + std::to_string(n);
            const float cw = fonts.semibold.font->measure(cnt, 18.0f);
            ui::text(list, fonts.semibold, cnt, x + 460, sheet.y + 236, 18, kWhite.with_alpha(0.8f), gfx::Align::right);
            ui::draw_button(list, fonts, style, ui::Button::r2, x + 460 - cw - 46, sheet.y + 230, 24);
            ui::draw_button(list, fonts, style, ui::Button::l2, x + 460 - cw - 100, sheet.y + 230, 24);
        }
        const int vis = std::min(3, n);
        const int start = dp_window_start();
        constexpr float kStep = 84.0f, kCardH = 76.0f;
        const Rect area{x, sheet.y + 254, 460, static_cast<float>(vis) * kStep - 8.0f};
        list.push_clip(area.inset(-6));
        const Rect hl{x, area.y + dp_hl_.value, 460, kCardH};
        glass_panel(list, hl, 20, Color::rgb(0x7fe0a0), 0.0f, 1.5f);
        list.bordered_rect(hl, 20, Color::rgb(0x000000, 0.0f), 2.0f, kWhite.with_alpha(0.82f));
        for (int i = start; i < start + vis; ++i)
        {
            const DpWorld &w = dp_worlds_[static_cast<std::size_t>(i)];
            const bool sel = i == dp_world_pos_;
            const Rect row{x, area.y + static_cast<float>(i - start) * kStep, 460, kCardH};
            if (!sel)
                list.bordered_rect(row, 20, kWhite.with_alpha(0.05f), 1.0f, kWhite.with_alpha(0.08f));
            const launcher::WorldRow *wr = nullptr;
            std::uint32_t tex = 0;
            for (std::size_t k = 0; k < worlds_.size(); ++k)
                if (worlds_[k].folder == w.folder)
                {
                    wr = &worlds_[k];
                    tex = k < icon_tex_.size() ? icon_tex_[k] : 0;
                }
            InstalledRow pic;
            pic.title = w.name;
            pic.tex = tex;
            draw_world_picture(list, pic, {row.x + 10, row.y + 8, 60, 60}, 10);
            // right: how many packs are on in this world
            const std::string on = std::to_string(w.on) + " of " + std::to_string(w.total) + " on";
            const float ow = fonts.semibold.font->measure(on, 17.0f);
            ui::text(list, fonts.semibold, on, row.x + row.w - 16, row.y + 31, 17, w.on > 0 ? Color::rgb(0x6fe6a3) : kWhite.with_alpha(0.55f), gfx::Align::right);
            const float nx = row.x + 86;
            ui::text_shrink(list, fonts.semibold, w.name, nx, row.y + 32, 23, 23*0.62f, row.x + row.w - 16 - ow - 14 - nx, kWhite.with_alpha(sel ? 1.0f : 0.82f));
            std::string meta = w.ver.empty() ? std::string() : w.ver;
            if (wr && !wr->mode.empty())
                meta += (meta.empty() ? "" : "  \xC2\xB7  ") + wr->mode;
            if (wr && wr->played > 0)
                meta += (meta.empty() ? "" : "  \xC2\xB7  ") + ago_text(wr->played);
            if (meta.empty())
                meta = w.folder;
            ui::text_shrink(list, fonts.regular, meta, nx, row.y + 59, 17, 17*0.62f, row.x + row.w - 16 - nx, kWhite.with_alpha(0.6f));
        }
        list.pop_clip();
        return 254.0f + static_cast<float>(vis) * kStep - 8.0f;
    }

    // Installed page.
    void draw_installed(gfx::DrawList &list, const Rect &sheet, const Entry &e) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const std::string rt = store_.runtime();
        float rw = 0.0f;
        if (inst_tab_ == 0 && !rt.empty())
        {
            const std::string jt = "Java " + rt + "  \xC2\xB7  running now";
            rw = fonts.regular.font->measure(jt, 22.0f);
            ui::text(list, fonts.regular, jt, sheet.x + sheet.w - 56, sheet.y + 118, 22, kWhite.with_alpha(0.6f), gfx::Align::right);
        }
        ui::text_shrink(list, fonts.regular, target_line(), sheet.x + 56, sheet.y + 118, 24, 24*0.62f, sheet.w - 112 - rw - (rw > 0 ? 40.0f : 0.0f), kWhite.with_alpha(0.78f));
        static const char *tabs[] = {"Versions", "Mods", "Resource packs", "Shaders", "Worlds", "Datapacks"};
        draw_tabs(list, sheet, tabs, 6, inst_tab_, e.accent);

        const Rect view{sheet.x + 56, sheet.y + 214, 1040, kListH};
        list.push_clip(view.inset(-4));
        draw_row_highlight(list, view, kModRowH, e.accent, !inst_rows_.empty());
        if (inst_rows_.empty())
        {
            const char *none[] = {"No versions installed yet. Use Install to download one.",
                                  "No mods in this instance. Content can install some (Fabric or Quilt needed).",
                                  "No resource packs in this instance.", "No shader packs in this instance.",
                                  "No worlds in this instance yet. Play it once and create one.",
                                  "No datapacks yet. Install some from Content > Datapacks, then switch them on for a world here."};
            const bool reading = inst_tab_ == 4 && !info_fresh_[4];
            ui::text_shrink(list, fonts.regular, reading ? "Reading the worlds..." : none[inst_tab_], view.x + 16, view.y + 60, 26, 26*0.62f, view.w - 32, kWhite.with_alpha(0.7f));
        }
        for (int i = 0; i < static_cast<int>(inst_rows_.size()); ++i)
        {
            const float y = view.y + static_cast<float>(i) * kModRowH - inst_scroll_.offset();
            if (y > view.y + view.h || y + kModRowH < view.y)
                continue;
            const InstalledRow &r = inst_rows_[static_cast<std::size_t>(i)];
            const Rect row{view.x, y, view.w, kModRowH - 8};
            const bool focus = i == inst_row_;
            if (!focus)
                list.bordered_rect(row, 18, kWhite.with_alpha(0.05f), 1.0f, kWhite.with_alpha(0.08f));
            const float left = row.x + (r.world ? 96.0f : 26.0f); // worlds carry their picture on the left
            if (r.world)
                draw_world_picture(list, r, {row.x + 14, row.y + 8, 62, 62}, 10);
            float rx = row.x + row.w - 20;
            for (auto it = r.tags.rbegin(); it != r.tags.rend(); ++it)
            {
                // the name keeps at least 280 px: tags that do not fit are left out (the right column repeats them)
                if (rx - (fonts.semibold.font->measure(it->first, 18.0f) + 26.0f) < left + 280.0f)
                    break;
                rx = draw_tag(list, rx, row.y + 30, it->first, it->second);
            }
            const float tw = std::max(120.0f, rx - left - 10);
            ui::text_shrink(list, fonts.semibold, r.title, left, row.y + 38, 28, 28*0.62f, tw, kWhite.with_alpha(r.off ? 0.55f : focus ? 1.0f : 0.9f));
            ui::text_shrink(list, fonts.regular, r.sub, left, row.y + 72, 20, 20*0.62f, row.x + row.w - 26 - left, kWhite.with_alpha(0.62f));
        }
        list.pop_clip();
        draw_scrollbar(list, view, static_cast<int>(inst_rows_.size()), kModRowH, inst_scroll_);

        const float x = sheet.x + sheet.w - 56 - 460;
        const ui::GlyphStyle style = ui::GlyphStyle::dark();
        float dp_bottom = 0.0f;   // Datapacks: where the world list ends (the pack's own details start below it)
        if (inst_tab_ == 5)
            dp_bottom = draw_dp_worlds(list, sheet, x);
        if (!inst_rows_.empty() && inst_row_ < static_cast<int>(inst_rows_.size()))
        {
            const InstalledRow &r = inst_rows_[static_cast<std::size_t>(inst_row_)];
            if (r.world)
            {
                // Picture, last played, version and mode; then the name and what the buttons do.
                draw_world_picture(list, r, {x, sheet.y + 214, 168, 168}, 18);
                const float fx = x + 192;
                ui::text(list, fonts.regular, "Last played", fx, sheet.y + 246, 20, kWhite.with_alpha(0.55f));
                ui::text_shrink(list, fonts.semibold, r.when, fx, sheet.y + 278, 26, 26*0.62f, 268, kWhite.with_alpha(0.95f));
                ui::text(list, fonts.regular, "Saved with", fx, sheet.y + 322, 20, kWhite.with_alpha(0.55f));
                ui::text_shrink(list, fonts.semibold, r.facts, fx, sheet.y + 354, 24, 24*0.62f, 268, kWhite.with_alpha(0.95f));
                const float last = fitted_paragraph(list, fonts.display, r.title, x, sheet.y + 440, 38, 24, 460, 2, 2, kWhite);
                ui::text_shrink(list, fonts.regular, r.sub, x, last + 38, 22, 22*0.62f, 460, kWhite.with_alpha(0.7f));
                ui::paragraph_box(list, fonts.regular, r.detail, x, last + 80, 20, 14, 460, sheet.y + 620, kWhite.with_alpha(0.75f));
                draw_action_line(list, ui::Button::cross, "Delete", x, sheet.y + 646, style);
                draw_action_line(list, ui::Button::square, "Rename", x, sheet.y + 698, style);
            }
            else
            {
                if (inst_tab_ == 5)
                {
                    // under the world list: the pack, in how many worlds it is on, and what the buttons do
                    const float top = sheet.y + dp_bottom + 56.0f;
                    const float last = fitted_paragraph(list, fonts.display, r.title, x, top, 34, 22, 460, 1, 2, kWhite);
                    ui::text_shrink(list, fonts.regular, r.sub, x, last + 32, 19, 19*0.62f, 460, kWhite.with_alpha(0.7f));
                    ui::paragraph_box(list, fonts.regular, r.detail, x, last + 62, 19, 14, 460, sheet.y + 662, kWhite.with_alpha(0.74f));
                    draw_action_line(list, ui::Button::cross, "Delete from library", x, sheet.y + 692, style);
                    if (!r.toggle.empty())
                        draw_action_line(list, ui::Button::square, r.off ? "Switch on for this world" : "Switch off for this world", x, sheet.y + 744, style);
                }
                else
                {
                // File names are long: a smaller face keeps them on two lines, then a third line.
                const float last = fitted_paragraph(list, fonts.display, r.title, x, sheet.y + 262, 44, 24, 460, 2, 3, kWhite);
                const float by = last + 50.0f;
                ui::text_shrink(list, fonts.regular, r.sub, x, by, 22, 22*0.62f, 460, kWhite.with_alpha(0.7f));
                ui::paragraph_box(list, fonts.regular, r.detail, x, by + 48, 21, 14, 460, r.why.empty() ? sheet.y + 625 : sheet.y + 515, kWhite.with_alpha(0.8f));
                if (!r.why.empty())
                    ui::paragraph_box(list, fonts.regular, r.why, x, sheet.y + 548, 20, 14, 460, sheet.y + 626, Color::rgb(0xffb86b));
                draw_action_line(list, ui::Button::cross, r.why.empty() ? "Delete" : "Delete (blocked)", x, sheet.y + 646, style);
                if (!r.toggle.empty())
                    draw_action_line(list, ui::Button::square, r.off ? "Switch on" : "Switch off", x, sheet.y + 698, style);
                }
            }
        }
    }

    // Status of the last action and the running job: one pill, top centre, between the title and the account chip,
    // above everything else (the shelves, the hero and the sheets never reach that strip).
    // The Java the selected instance asks for differs from the one running: the app has to be opened again. Stays until it matches (top right, under the account chip).
    // The Java the selected instance's Minecraft version asks for (0 = unknown) and its linked tier (17 / 21 / 25).
    int selected_need() const
    {
        const int idx = store_.find(store_.current);
        if (idx < 0 || !ps5_java_need_of)
            return 0;
        return ps5_java_need_of(store_.effective_version(store_.instances[static_cast<std::size_t>(idx)]).c_str());
    }
    static int java_tier(int need)
    {
        if (need <= 0)
            return 0;
        if (ps5_java_for)
            return ps5_java_for(0, need);   // the smallest Java this build carries that is at least `need` (8 only when a Java 8 is linked)
        return need <= 17 ? 17 : need <= 21 ? 21 : 25;
    }
    std::string selected_version() const
    {
        const int idx = store_.find(store_.current);
        return idx < 0 ? std::string() : game_version(store_.effective_version(store_.instances[static_cast<std::size_t>(idx)]));
    }

    // One line about the Java setting, shown as the amber pill (top right) and after saving it; empty when nothing needs saying.
    std::string restart_note() const
    {
        const int running = ps5_running_jdk ? ps5_running_jdk() : 0;
        if (!running)
            return {};
        const std::string jd = store_.config("jdk");   // launcher.cfg jdk=8|17|21|25, absent = automatic
        const int want = jd == "8" ? 8 : jd == "17" ? 17 : jd == "21" ? 21 : jd == "25" ? 25 : 0;
        const int need = selected_need(), tier = java_tier(need);
        const std::string ver = selected_version();
        const int expect = ps5_java_for ? ps5_java_for(want, need) : (want ? want : running);   // what the next start boots
        if (want && expect != running)
            return "Restart the app to switch from Java " + std::to_string(running) + " to Java " + std::to_string(expect);
        if (want && tier && want < tier)
            return "Java " + std::to_string(want) + " is too old for " + ver + " (needs Java " + std::to_string(tier) + "): it runs on Java " + std::to_string(expect);
        if (!want && tier > running)
            return ver + " needs Java " + std::to_string(tier) + ": the app closes on Play, open it again";
        return {};
    }

    void draw_notice(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        {
            const std::string note = restart_note();
            if (!note.empty())
            {
                // Wrapped to at most two lines, as wide as the longest line needs (the old pill fitted the text into
                // width-64 while being 56 + text wide, so the last characters were always cut off).
                constexpr float kPillMax = 980.0f, kPad = 74.0f;
                const std::vector<std::string> lines = fonts.regular.font->wrap(note, 22.0f, kPillMax - kPad);
                const std::size_t n = std::min<std::size_t>(lines.size(), 3);
                float tw = 0.0f;
                for (std::size_t i = 0; i < n; ++i)
                    tw = std::max(tw, fonts.regular.font->measure(lines[i], 22.0f));
                const float w = tw + kPad, h = 46.0f + 36.0f * static_cast<float>(n - 1);
                const Rect pill{1824 - w, 118, w, h};
                list.shadow({pill.x, pill.y + 6, pill.w, pill.h}, 23, 18, Color::rgb(0x000000, 0.4f));
                list.bordered_rect(pill, 23, Color::rgb(0x2a1a05, 0.92f), 1.5f, Color::rgb(0xffb44a, 0.8f));
                list.circle(pill.x + 24, pill.y + 23, 7, Color::rgb(0xffb44a, 1.0f));
                for (std::size_t i = 0; i < n; ++i)
                {
                    std::string line = lines[i];
                    if (i + 1 == n && lines.size() > n)
                        line = fonts.regular.font->fit(line + " \xE2\x80\xA6", 22.0f, tw);
                    ui::text(list, fonts.regular, line, pill.x + 42, pill.y + 30 + 30.0f * static_cast<float>(i), 22, kWhite.with_alpha(0.95f));
                }
            }
        }
        const bool busy = jobs_.busy() && !info_busy();
        const bool toast = !status_.empty() && status_age_ < 6.0f;
        if (!busy && !toast)
            return;
        constexpr float kCenter = 1120.0f, kWide = 940.0f;
        if (busy)
        {
            const launcher::JobStatus &s = jobs_.status();
            const Rect box{kCenter - 400, 16, 800, 72};
            glass_panel(list, box, 36, palette_[3].value(), 0.7f);
            list.rounded_rect(box, 36, Color::rgb(0x05070f, 0.34f));
            ui::text_shrink(list, fonts.semibold, s.title, box.x + 40, box.y + 28, 22, 22*0.62f, box.w - 80, kWhite);
            ui::text_shrink(list, fonts.regular, s.detail.empty() ? "Working..." : s.detail, box.x + 40, box.y + 50, 17, 17*0.62f, box.w - 80, kWhite.with_alpha(0.68f));
            const Rect track{box.x + 40, box.y + box.h - 11, box.w - 80, 4};
            list.rounded_rect(track, 2, kWhite.with_alpha(0.16f));
            if (s.progress >= 0.0f)
                list.rounded_rect({track.x, track.y, std::max(4.0f, track.w * std::min(1.0f, s.progress)), track.h}, 2,
                                  palette_[3].value());
            else
            {
                const float p = std::fmod(clock_ * 0.7f, 1.0f);
                const float w = track.w * 0.25f;
                list.rounded_rect({track.x + (track.w - w) * p, track.y, w, track.h}, 2, palette_[3].value());
            }
            return;
        }
        const float a = tween::clamp01(status_age_ * 6.0f) * tween::clamp01((6.0f - status_age_) * 2.0f);
        const std::vector<std::string> lines = fonts.regular.font->wrap(status_, 24.0f, kWide - 60.0f);
        const std::size_t n = std::min<std::size_t>(lines.size(), 3);
        float tw = 0.0f;
        for (std::size_t i = 0; i < n; ++i)
            tw = std::max(tw, fonts.regular.font->measure(lines[i], 24.0f));
        const float w = tw + 64.0f, h = 54.0f + 30.0f * static_cast<float>(n - 1);
        const Rect pill{kCenter - w * 0.5f, 28, w, h};
        list.push_opacity(a);
        glass_panel(list, pill, 27, palette_[3].value(), 0.5f);
        list.rounded_rect(pill, 27, Color::rgb(0x05070f, 0.34f));
        for (std::size_t i = 0; i < n; ++i)
        {
            std::string line = lines[i];
            if (i + 1 == n && lines.size() > n)
                line = fonts.regular.font->fit(line + " \xE2\x80\xA6", 24.0f, tw);
            ui::text(list, fonts.regular, line, pill.x + 32, pill.y + 35 + 30.0f * static_cast<float>(i), 24, kWhite.with_alpha(0.95f));
        }
        list.pop_opacity();
    }

    void draw_confirm(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float t = conf_t_.value;
        list.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0, Color::rgb(0x05070f, 0.6f * t));
        Rect p{460, 270 + 40.0f * (1.0f - t), 1000, 500};
        list.push_opacity(tween::clamp01(t * 1.5f));
        glass_panel(list, p, 40, Color::rgb(0xff6a6a), 1.0f);
        list.rounded_rect(p, 40, Color::rgb(0x1a0a10, 0.52f));
        list.bordered_rect(p, 40, Color::rgb(0x000000, 0.0f), 2.0f, Color::rgb(0xff7a7a, 0.6f));
        ui::text(list, fonts.semibold, "PLEASE CONFIRM", p.x + 60, p.y + 84, 20, Color::rgb(0xff9a9a), gfx::Align::left, 4.0f);
        ui::paragraph_box(list, fonts.display, confirm_title_, p.x + 60, p.y + 150, 40, 24, p.w - 120, p.y + 200, kWhite, 1.2f);
        ui::paragraph_box(list, fonts.regular, confirm_text_, p.x + 60, p.y + 250, 24, 15, p.w - 120, p.y + p.h - 150, kWhite.with_alpha(0.82f));
        const Rect del{p.x + 60, p.y + p.h - 120, 250, 64};
        list.rounded_rect(del, 32, Color::rgb(0xff8a8a));
        ui::draw_button(list, fonts, ui::GlyphStyle::light(), ui::Button::cross, del.x + 22, del.cy(), 34);
        ui::text(list, fonts.semibold, "Delete", del.x + 38.0f + ui::button_width(ui::Button::cross, 34.0f), del.cy() + 10, 28, Color::rgb(0x2a0c0c));
        const Rect no{del.x + del.w + 20, del.y, 230, 64};
        glass_panel(list, no, 32, kWhite, 0.0f, 1.0f);
        ui::draw_button(list, fonts, ui::GlyphStyle::dark(), ui::Button::circle, no.x + 22, no.cy(), 34);
        ui::text(list, fonts.semibold, "Cancel", no.x + 38.0f + ui::button_width(ui::Button::circle, 34.0f), no.cy() + 10, 28, kWhite.with_alpha(0.95f));
        list.pop_opacity();
    }

    void draw_keyboard(gfx::DrawList &list, std::uint32_t glass) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float t = kb_t_.value;
        list.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0, Color::rgb(0x05070f, 0.55f * t));
        Rect p = kb_panel();
        p.y += 40.0f * (1.0f - t);
        list.push_opacity(tween::clamp01(t * 1.5f));
        glass_panel(list, p, 40, palette_[3].value(), 1.0f);
        list.rounded_rect(p, 40, Color::rgb(0x0a0e1a, 0.50f));
        ui::text(list, fonts.semibold, ui::upper(kb_title_), p.x + 60, p.y + 84, 20, palette_[3].value(), gfx::Align::left, 4.0f);
        const Rect box{p.x + 60, p.y + 112, p.w - 120, 96};
        glass_panel(list, box, 20, kWhite, 0.0f, 0.9f);
        list.bordered_rect(box, 20, Color::rgb(0x000000, 0.0f), 2.0f, kWhite.with_alpha(0.45f));
        {
            const float room = box.w - 64.0f;   // a long text shrinks to stay inside the box
            float ts = 48.0f;
            const float full = fonts.display.font->measure(kb_text_, ts);
            if (full > room && full > 0.0f)
                ts = std::max(22.0f, std::floor(48.0f * room / full));
            ui::text_shrink(list, fonts.display, kb_text_, box.x + 28, box.cy() + 16, ts, ts, room, kWhite);
            const std::size_t at = std::min(kb_caret_, kb_text_.size());
            const float cx = fonts.display.font->measure(kb_text_.substr(0, at), ts);
            if (std::fmod(clock_, 1.0f) < 0.6f)
                list.rounded_rect({box.x + 32 + cx, box.cy() - 28, 3, 56}, 1.5f, kWhite.with_alpha(0.9f));
        }
        if (kb_text_.empty())
            ui::text(list, fonts.regular, kb_target_ == 0   ? "Instance name"
                                         : kb_target_ == 2 ? "Version, e.g. 1.12 or 24w14 (empty: all)"
                                         : kb_target_ == 3 ? "World name"
                                         : kb_target_ == 4 ? "Offline username (3 to 16 letters, digits, _)"
                                                           : "Mod, pack or shader name (empty: top downloads)",
                     box.x + 28, box.cy() + 10, 28, kWhite.with_alpha(0.35f));
        ui::Canvas canvas{list, fonts, glass, clock_};
        if (kbm_)
        {
            const ui::GlyphStyle gs = ui::GlyphStyle::dark();
            ui::text(list, fonts.regular, "Type with the keyboard. Left, Right, Home, End and Delete edit the text.", p.x + 60, p.y + 246, 22, kWhite.with_alpha(0.65f));
            const Rect done = kb_done_rect(), cancel = kb_cancel_rect();
            list.rounded_rect(done, 32, kWhite.with_alpha(0.94f));
            ui::draw_button(list, fonts, ui::GlyphStyle::light(), ui::Button::cross, done.x + 20, done.cy(), 34);
            ui::text(list, fonts.semibold, "Done", done.x + 40 + ui::button_width(ui::Button::cross, 34), done.cy() + 10, 28, Color::rgb(0x0b0d16));
            glass_panel(list, cancel, 32, kWhite, 0.0f, 1.0f);
            ui::draw_button(list, fonts, gs, ui::Button::circle, cancel.x + 20, cancel.cy(), 34);
            ui::text(list, fonts.semibold, "Cancel", cancel.x + 40 + ui::button_width(ui::Button::circle, 34), cancel.cy() + 10, 28, kWhite.with_alpha(0.95f));
        }
        else
            keyboard_.draw(canvas);
        list.pop_opacity();
    }

    void draw_error(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float t = err_t_.value;
        list.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0, Color::rgb(0x05070f, 0.6f * t));
        Rect p{460, 300 + 40.0f * (1.0f - t), 1000, 440};
        list.push_opacity(tween::clamp01(t * 1.5f));
        glass_panel(list, p, 40, Color::rgb(0xff6a6a), 1.0f);
        list.rounded_rect(p, 40, Color::rgb(0x1a0a10, 0.52f));
        list.bordered_rect(p, 40, Color::rgb(0x000000, 0.0f), 2.0f, Color::rgb(0xff7a7a, 0.6f));
        ui::text(list, fonts.semibold, "DID NOT WORK", p.x + 60, p.y + 84, 20, Color::rgb(0xff9a9a), gfx::Align::left, 4.0f);
        ui::paragraph_box(list, fonts.display, error_title_, p.x + 60, p.y + 150, 40, 24, p.w - 120, p.y + 200, kWhite, 1.2f);
        ui::paragraph_box(list, fonts.regular, error_text_, p.x + 60, p.y + 236, 24, 15, p.w - 120, p.y + p.h - 80, kWhite.with_alpha(0.82f));
        ui::text(list, fonts.regular, error_retry_ ? "Run it again to retry. Files that finished downloading are kept." : "Press Close, then try again whenever you like.", p.x + 60, p.y + p.h - 44, 20,
                 kWhite.with_alpha(0.55f));
        list.pop_opacity();
    }

    // ---- drawing ----
    Rect sheet_rect() const
    {
        return {120, 96, 1680, 820};
    }

    // The signed-in player's skin face (the profile picture): loaded once the skin file exists, dropped on sign-out / another account.
    void ensure_face() const
    {
        if (!signed_in_now() || face_for_ != store_.account)
        {
            if (face_tex_ && launcher::textures().release)
                launcher::textures().release(face_tex_);
            face_tex_ = 0;
            face_for_.clear();
        }
        if (signed_in_now() && face_tex_ == 0 && clock_ - face_try_ > 3.0f)
        {
            face_try_ = clock_;
            face_tex_ = launcher::load_skin_face(store_.root + "/launcher/skin-" + store_.account + ".png", 128);
            if (face_tex_)
                face_for_ = store_.account;
        }
    }

    void draw_top_bar(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float in = tween::stagger(age_, 0, 0.05f, 0.5f) * (1.0f - sheet_t_.value);
        list.push_opacity(in);
        float x = kMargin;
        x += ui::text(list, fonts.display, "Minecraft", x, 96 - 10 * (1.0f - in), 40, kWhite) + 18;
        ui::text(list, fonts.semibold, "JAVA EDITION  \xC2\xB7  PS5", x, 94, 18, kWhite.with_alpha(0.55f),
                 gfx::Align::left, 3.0f);
        const std::string acc = signed_in_now() ? account_label() : "Offline";
        ensure_face();
        const float w = 60.0f + std::max(110.0f, fonts.semibold.font->measure(acc.c_str(), 24.0f)) + 26.0f;   // face (60) + the name + a margin: the name never runs over the chip edge
        const Rect chip{1824 - w, 52, w, 56};
        glass_panel(list, chip, 28, palette_[3].value(), 0.6f);
        draw_hover(list, chip, 28, false);
        list.circle(chip.x + 28, chip.cy(), 20, palette_[3].value());
        if (face_tex_)
            list.image(face_tex_, {chip.x + 8, chip.cy() - 20, 40, 40}, gfx::kFullUv, kWhite, 8.0f);   // the skin face as the profile picture
        else
            ui::text(list, fonts.semibold, acc.substr(0, 1), chip.x + 28, chip.cy() + 8, 22, Color::rgb(0x0b0d16),
                     gfx::Align::center);
        ui::text(list, fonts.semibold, acc, chip.x + 60, chip.cy() + 9, 24, kWhite.with_alpha(0.92f));
        list.pop_opacity();
    }

    // The main action: a bright pill of milky glass with the accent colour glowing up from below.
    void draw_primary_pill(gfx::DrawList &list, const Rect &r, Color accent) const
    {
        const float radius = r.h * 0.5f;
        list.rounded_rect(r, radius, kWhite.with_alpha(0.94f));
        list.gradient_rect(r, radius, kWhite.with_alpha(0.0f), gfx::mix(accent, kWhite, 0.35f).with_alpha(0.55f));
        list.bordered_rect(r, radius, Color::rgb(0x000000, 0.0f), 1.5f, kWhite);
        const float c = 0.5f + 0.3f * std::sin(clock_ * 0.8f);
        const float sx = r.x + radius * 0.7f, sw = r.w - radius * 1.4f;
        list.gradient_rect_h({sx, r.y + 3.0f, sw * c, 2.4f}, 1.2f, kWhite.with_alpha(0.0f), kWhite);
        list.gradient_rect_h({sx + sw * c, r.y + 3.0f, sw * (1.0f - c), 2.4f}, 1.2f, kWhite, kWhite.with_alpha(0.0f));
    }

    void draw_badge_art(gfx::DrawList &list, const Entry &e, const Rect &r, float radius, float text_size,
                        bool big = true) const
    {
        const ui::Fonts &fonts = context_.fonts;
        glass_panel(list, r, radius, e.accent, big ? 1.0f : 0.0f);
        list.gradient_rect(r.inset(1.5f), radius, gfx::mix(e.mid, e.accent, 0.25f).with_alpha(0.62f), e.dark.with_alpha(0.74f));
        // Isometric-ish block stack: three tiles hint at Minecraft without using its art.
        const float u = r.w * 0.16f;
        const float cx = r.cx();
        const float cy = r.y + r.h * 0.42f;
        const Color top = gfx::mix(e.accent, kWhite, 0.15f);
        list.rounded_rect({cx - u * 1.1f, cy - u * 0.2f, u * 1.0f, u * 1.0f}, u * 0.12f, e.accent.with_alpha(0.55f));
        list.rounded_rect({cx - u * 0.1f, cy - u * 0.9f, u * 1.0f, u * 1.0f}, u * 0.12f, top.with_alpha(0.85f));
        list.rounded_rect({cx + u * 0.0f, cy + u * 0.1f, u * 1.0f, u * 1.0f}, u * 0.12f, e.accent.with_alpha(0.35f));
        if (big)
            ui::text(list, fonts.display, e.badge, cx, r.y + r.h - r.h * 0.12f, text_size, kWhite, gfx::Align::center);
        else
            ui::text(list, fonts.semibold, e.badge, r.x + 16, r.y + 34, r.w * 0.125f, kWhite.with_alpha(0.8f));
    }

    void draw_hero_entry(gfx::DrawList &list, const Entry &e, int row, float alpha, float slide) const
    {
        if (alpha <= 0.01f)
            return;
        const ui::Fonts &fonts = context_.fonts;
        list.push_opacity(alpha);

        const float bob = context_.settings.reduced_motion ? 0.0f : std::sin(clock_ * 0.8f) * 6.0f;
        const Rect art{1316 + slide * 1.6f, 132 + bob, 440, 440};
        list.glow(art.inset(30), 60, 90, e.accent.with_alpha(0.22f + 0.08f * ui::breathe(clock_, 4.2f)));
        draw_badge_art(list, e, art, 36, 92);

        const float x = kMargin + slide;
        ui::text(list, fonts.semibold, ui::upper(shelves_[static_cast<std::size_t>(row)].title), x, 212, 20,
                 e.accent, gfx::Align::left, 4.0f);
        ui::text_shrink(list, fonts.display, e.title, x - 4, 304, 88, 88*0.62f, 1130.0f, kWhite);
        ui::text_shrink(list, fonts.regular, meta_of(e), x, 358, 26, 26*0.62f, 1130.0f, kWhite.with_alpha(0.78f));
        // Two lines on instance pages (the chips sit below); tools have nothing under the text, so they may use three.
        ui::paragraph_box(list, fonts.regular, e.blurb, x, 414, 28, 16, 860, e.kind == Kind::tool ? 494.0f : 456.0f, kWhite.with_alpha(0.86f), 1.43f);

        if (e.kind == Kind::instance)
        {
            const launcher::Instance &in = store_.instances[static_cast<std::size_t>(e.instance)];
            char t[64];
            float cx = x;
            const auto chip = [&](const char *label) {
                const float w = fonts.semibold.font->measure(label, 22.0f);
                glass_panel(list, {cx, 488, w + 40, 40}, 20, kWhite, 0.0f, 0.8f);
                ui::text(list, fonts.semibold, label, cx + 20, 515, 22, kWhite.with_alpha(0.94f));
                cx += w + 40 + 12;
            };
            { int gmb = 3072; { const std::string m = store_.config("xmx"); const int v = std::atoi(m.c_str()); if (v > 0) gmb = v; } std::snprintf(t, sizeof t, "%d MB", gmb); }
            chip(t);
            std::snprintf(t, sizeof t, "%d cores", in.geti("cpus", 4));
            chip(t);
            chip(in.get("gc", "parallel") == "serial" ? "Serial GC" : "Parallel GC");
        }

        if (e.kind != Kind::tool)
        {
            const bool is_new = e.kind == Kind::create;
            const Rect play{x, 556, 240, 64};
            const float pulse = 1.0f + 0.04f * play_.value;
            list.glow(play, 32, 20, e.accent.with_alpha(0.30f + 0.12f * ui::breathe(clock_, 3.2f)));
            draw_primary_pill(list, play.inset(-play.w * (pulse - 1.0f) * 0.5f), e.accent);
            draw_hover(list, play, 32, false);
            ui::draw_button(list, fonts, ui::GlyphStyle::light(), ui::Button::cross, play.x + 22, play.cy(), 34);
            ui::text(list, fonts.semibold, is_new ? "Create" : "Play", play.x + 38.0f + ui::button_width(ui::Button::cross, 34.0f), play.cy() + 10, 28,
                     Color::rgb(0x0b0d16));
            if (!is_new)
            {
                const Rect edit{x + 260, 556, 220, 64};
                glass_panel(list, edit, 32, e.accent, 0.0f, 1.0f);
                draw_hover(list, edit, 32, false);
                ui::draw_button(list, fonts, ui::GlyphStyle::dark(), ui::Button::square, edit.x + 22, edit.cy(), 34);
                ui::text(list, fonts.semibold, "Settings", edit.x + 38.0f + ui::button_width(ui::Button::square, 34.0f), edit.cy() + 10, 28, kWhite.with_alpha(0.95f));
            }
        }
        else
        {
            const Rect open{x, 556, 240, 64};
            list.glow(open, 32, 20, e.accent.with_alpha(0.30f + 0.12f * ui::breathe(clock_, 3.2f)));
            draw_primary_pill(list, open, e.accent);
            draw_hover(list, open, 32, false);
            ui::draw_button(list, fonts, ui::GlyphStyle::light(), ui::Button::cross, open.x + 22, open.cy(), 34);
            ui::text(list, fonts.semibold, "Open", open.x + 38.0f + ui::button_width(ui::Button::cross, 34.0f), open.cy() + 10, 28, Color::rgb(0x0b0d16));
        }
        list.pop_opacity();
    }

    void draw_hero(gfx::DrawList &list) const
    {
        const float in = tween::stagger(age_, 1, 0.08f, 0.6f);
        list.push_opacity(in);
        if (hero_.running)
        {
            const float t = hero_.progress();
            draw_hero_entry(list, entry_at(previous_), previous_row_, 1.0f - tween::smoothstep(t * 2.2f),
                            -36.0f * tween::cubic_in(tween::clamp01(t * 2.2f)));
            const float arrive = tween::clamp01((t - 0.25f) / 0.75f);
            draw_hero_entry(list, entry_at(shown_), shown_row_, tween::smoothstep(arrive),
                            44.0f * (1.0f - tween::quint_out(arrive)));
        }
        else
        {
            draw_hero_entry(list, entry_at(shown_), shown_row_, 1.0f, 30.0f * (1.0f - in));
        }
        list.pop_opacity();
    }

    void draw_card(gfx::DrawList &list, const Entry &e, const Rect &r, float alpha, bool current) const
    {
        const ui::Fonts &fonts = context_.fonts;
        list.push_opacity(alpha);
        draw_badge_art(list, e, r, 22, r.w * 0.2f, false);
        ui::text_shrink(list, fonts.semibold, e.title, r.x + 16, r.y + r.h - 18.0f, r.w * 0.1f, r.w * 0.1f*0.62f, r.w - 32.0f, kWhite.with_alpha(0.95f));
        if (current)
        {
            list.circle(r.x + r.w - 22, r.y + 22, 8, kWhite);
        }
        list.pop_opacity();
    }

    void draw_shelves(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float nudge = ui::shake(nudge_.value, clock_, 16.0f, 8.0f) * nudge_dir_;
        for (int r = 0; r < 2; ++r)
        {
            const Shelf &shelf = shelves_[static_cast<std::size_t>(r)];
            const float distance = static_cast<float>(r) - row_position_.value;
            const float visible = tween::clamp01(1.0f + distance * 2.5f) *
                                  (distance > 0.0f ? 1.0f - 0.45f * tween::clamp01(distance) : 1.0f);
            if (visible <= 0.01f)
                continue;
            const float in = tween::stagger(age_, 3 + r, 0.09f, 0.6f);
            list.push_opacity(visible * in);
            const float y = kShelfY + distance * kShelfPitch + 40.0f * (1.0f - in);
            ui::text(list, fonts.semibold, shelf.title, kMargin, y - 62, 24, kWhite.with_alpha(r == focus_row_ ? 0.95f : 0.6f));
            for (int c = 0; c < static_cast<int>(shelf.items.size()); ++c)
            {
                Rect rect = card_rect(r, c, false);
                rect.y = y;
                if (rect.x > gfx::kVirtualWidth || rect.x + rect.w < -80.0f)
                    continue;
                if (r == focus_row_ && c == shelf.column)
                    continue;
                if (r == focus_row_ && c > shelf.column)
                    rect.x += kCard * (kCardGrow - 1.0f);
                const Entry &e = shelf.items[static_cast<std::size_t>(c)];
                const bool current = e.kind == Kind::instance &&
                                     store_.instances[static_cast<std::size_t>(e.instance)].id == store_.current;
                draw_card(list, e, rect, 0.86f, current);
            }
            list.pop_opacity();
        }

        const Entry &e = focused();
        Rect ring = ring_.value();
        ring.x += nudge;
        const float in = tween::stagger(age_, 3 + focus_row_, 0.09f, 0.6f);
        list.push_opacity(in);
        list.glow(ring, 26, 30, e.accent.with_alpha(0.36f + 0.16f * ui::breathe(clock_, 3.0f)));
        const bool current = e.kind == Kind::instance &&
                             store_.instances[static_cast<std::size_t>(e.instance)].id == store_.current;
        draw_card(list, e, ring, 1.0f, current);
        list.bordered_rect(ring.inset(-4), 28, Color::rgb(0x000000, 0.0f), 2.5f, kWhite.with_alpha(0.9f));
        list.bordered_rect(ring.inset(-9), 33, Color::rgb(0x000000, 0.0f), 1.5f, kWhite.with_alpha(0.22f));
        list.pop_opacity();
    }

    void draw_sheet(gfx::DrawList &list, std::uint32_t glass) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const Entry &e = focused();
        const float t = sheet_t_.value;
        Rect sheet = sheet_rect();
        sheet.y += 60.0f * (1.0f - t);
        list.push_opacity(tween::clamp01(t * 1.4f));
        (void)glass;
        glass_panel(list, sheet, 44, e.accent, 1.0f);
        list.rounded_rect(sheet, 44, gfx::mix(e.dark, Color::rgb(0x0b0d16), 0.5f).with_alpha(0.36f));   // text-heavy: a little more body than a card

        static const char *titles[] = {"INSTANCE SETTINGS", "GAME VERSIONS", "CONTENT", "SETTINGS", "ACCOUNTS", "LIBRARY"};
        ui::text(list, fonts.semibold, titles[static_cast<int>(mode_)], sheet.x + 56, sheet.y + 76, 20, e.accent,
                 gfx::Align::left, 4.0f);
        if (mode_ == Mode::install || mode_ == Mode::mods || mode_ == Mode::installed)
        {
            if (mode_ == Mode::install)
                draw_install(list, sheet, e);
            else if (mode_ == Mode::mods)
                draw_mods(list, sheet, e);
            else
                draw_installed(list, sheet, e);
            list.pop_opacity();
            return;
        }

        ui::Canvas canvas{list, fonts, glass, clock_};
        form_.draw(canvas);

        // Right column: live summary card and the focused row's help.
        const Rect art{sheet.x + sheet.w - 56 - 420, sheet.y + 56, 330, 330};   // smaller than before: the help text below needs room for up to 8 lines
        if (mode_ == Mode::launcher && form_.focus_id() == kCursor)
        {
            // Cursor size: the real pointer, at the real size, over a card and a button.
            glass_panel(list, art, 32, e.accent, 1.0f);
            list.rounded_rect(art, 32, Color::rgb(0x0b0d16, 0.35f));
            const Rect card{art.x + 36, art.y + 44, 150, 150};
            draw_badge_art(list, e, card, 22, 30, false);
            const Rect pill{art.x + 36, art.y + 226, 220, 56};
            list.rounded_rect(pill, 28, kWhite.with_alpha(0.9f));
            ui::text(list, fonts.semibold, "Play", pill.cx(), pill.cy() + 9, 26, Color::rgb(0x0b0d16), gfx::Align::center);
            draw_cursor_sprite(list, card.x + card.w * 0.55f, card.y + card.h * 0.5f, cur_scale_);
            ui::text(list, fonts.semibold, "PREVIEW", art.x + art.w - 30, art.y + 40, 18, kWhite.with_alpha(0.5f), gfx::Align::right, 3.0f);
        }
        else if (mode_ == Mode::account && form_.focus_id() == kAccount && signed_in_now() && (ensure_face(), face_tex_ != 0))
        {
            // the Account row: the player's skin face instead of the ID icon
            glass_panel(list, art, 32, e.accent, 1.0f);
            list.gradient_rect(art.inset(1.5f), 32, gfx::mix(e.mid, e.accent, 0.25f).with_alpha(0.62f), e.dark.with_alpha(0.74f));
            list.image(face_tex_, art.inset(34), gfx::kFullUv, kWhite, 20.0f);
        }
        else
            draw_badge_art(list, e, art, 32, 84);
        ui::text_shrink(list, fonts.display, e.title, art.x, art.y + art.h + 64, 44, 44*0.62f, 420.0f, kWhite);
        ui::text_shrink(list, fonts.regular, meta_of(e), art.x, art.y + art.h + 104, 24, 24*0.62f, 420.0f, kWhite.with_alpha(0.75f));
        const std::string &help = form_.help_text();
        if (!help.empty())
            ui::paragraph_box(list, fonts.regular, help, art.x, art.y + art.h + 150, 21, 13, 420, sheet.y + sheet.h - 40, kWhite.with_alpha(0.78f));
        list.pop_opacity();
    }

    // Draws a row of hints at the bottom right and records where each button is, so the pointer can press them.
    void put_hints(gfx::DrawList &list, const ui::Hint *hints, int count) const
    {
        const ui::GlyphStyle style = ui::GlyphStyle::dark();
        ui::HintBox boxes[16];
        const int n = ui::hint_boxes(context_.fonts, hints, std::min(count, 8), 1824, true, boxes);
        hint_hits_.clear();
        for (int i = 0; i < n; ++i)
            hint_hits_.push_back(boxes[i]);
        ui::draw_hints(list, context_.fonts, style, hints, count, 1824, true);
    }

    void draw_hints(gfx::DrawList &list) const
    {
        hint_hits_.clear();
        const ui::Fonts &fonts = context_.fonts;
        const ui::GlyphStyle style = ui::GlyphStyle::dark();
        if (error_open_)
        {
            const ui::Hint hints[] = {{ui::Button::cross, "Close"}};
            put_hints(list, hints, 1);
        }
        else if (confirm_open_)
        {
            const ui::Hint hints[] = {{ui::Button::cross, "Delete"}, {ui::Button::circle, "Cancel"}};
            put_hints(list, hints, 2);
        }
        else if (kb_open_ && kbm_)
        {
            const ui::Hint hints[] = {{ui::Button::cross, "Done"}, {ui::Button::circle, "Cancel"}};
            put_hints(list, hints, 2);
        }
        else if (kb_open_)
        {
            const ui::Hint hints[] = {{ui::Button::square, "Delete"}, {ui::Button::triangle, "Space"},
                                      {ui::Button::l2, "Shift"}, {ui::Button::right_stick, "Symbols"},
                                      {ui::Button::l1, "Cursor", ui::Button::r1}, {ui::Button::options, "Done"}, {ui::Button::circle, "Cancel"}};
            put_hints(list, hints, 7);
        }
        else if (sheet_open_ && mode_ == Mode::install)
        {
            const ui::Hint hints[] = {{ui::Button::cross, "Install"}, {ui::Button::square, "+ Fabric"},
                                      {ui::Button::triangle, "+ Quilt"}, {ui::Button::l1, "Tab", ui::Button::r1},
                                      {ui::Button::l2, "Search"}, {ui::Button::r2, "Sort"},
                                      {ui::Button::options, "Refresh"}, {ui::Button::circle, "Back"}};
            put_hints(list, hints, 8);
        }
        else if (sheet_open_ && mode_ == Mode::mods)
        {
            const ui::Hint hints[] = {{ui::Button::cross, "Install"}, {ui::Button::square, "Search"},
                                      {ui::Button::l1, "Type", ui::Button::r1}, {ui::Button::options, "Refresh"},
                                      {ui::Button::circle, "Back"}};
            put_hints(list, hints, 5);
        }
        else if (sheet_open_ && mode_ == Mode::installed)
        {
            const bool can_toggle = !inst_rows_.empty() && inst_row_ < static_cast<int>(inst_rows_.size()) &&
                                    !inst_rows_[static_cast<std::size_t>(inst_row_)].toggle.empty();
            const bool off = can_toggle && inst_rows_[static_cast<std::size_t>(inst_row_)].off;
            const bool world = !inst_rows_.empty() && inst_row_ < static_cast<int>(inst_rows_.size()) &&
                               inst_rows_[static_cast<std::size_t>(inst_row_)].world;
            const ui::Hint hints[] = {{ui::Button::cross, "Delete"}, {ui::Button::square, world ? "Rename" : off ? "Switch on" : "Switch off"},
                                      {ui::Button::l1, "Tab", ui::Button::r1}, {ui::Button::circle, "Back"}};
            if (inst_tab_ == 5 && dp_world_count_ > 1)
            {
                const ui::Hint dp[] = {hints[0], hints[1], {ui::Button::l2, "World", ui::Button::r2}, hints[2], hints[3]};
                put_hints(list, dp, 5);
            }
            else if (can_toggle || world)
                put_hints(list, hints, 4);
            else
            {
                const ui::Hint plain[] = {hints[0], hints[2], hints[3]};
                put_hints(list, plain, 3);
            }
        }
        else if (sheet_open_ && mode_ == Mode::account)
        {
            const ui::Hint hints[] = {{ui::Button::dpad, "Move"}, {ui::Button::circle, "Back"}};
            put_hints(list, hints, 2);
        }
        else if (sheet_open_)
        {
            const ui::Hint hints[] = {{ui::Button::dpad, "Adjust"}, {ui::Button::circle, "Save and back"}};
            put_hints(list, hints, 2);
        }
        else
        {
            list.push_opacity(tween::stagger(age_, 8, 0.08f, 0.5f) * (1.0f - sheet_t_.value));
            const ui::Hint hints[] = {{ui::Button::cross, focused().kind == Kind::tool ? "Open" : "Play"},
                                      {ui::Button::square, "Settings"}};
            put_hints(list, hints, focused().kind == Kind::instance ? 2 : 1);
            list.pop_opacity();
        }
    }

    app::Context &context_;
    launcher::Slideshow bg_;               // the background pictures (L1 / R1 on the home screen)
    mutable std::uint32_t face_tex_ = 0;     // the signed-in player's skin face (profile picture)
    std::string skin_asked_;                 // the account whose skin download was requested
    mutable std::string face_for_;           // the account id face_tex_ belongs to
    mutable float face_try_ = -10.0f;        // clock_ of the last attempt to load it (the skin arrives a moment after the sign-in)
    std::uint32_t unknown_tex_ = 0;        // unknown_pack.png: the picture of a world without an icon
    mutable std::uint32_t glass_tex_ = 0;  // this frame's blurred background (set at the start of draw)
    launcher::Store store_;
    ui::Theme theme_ = ui::default_theme();
    std::vector<Shelf> shelves_;
    std::vector<std::string> versions_;   // instance settings: index 0 is "" (None), then the installed Minecraft versions
    std::string pending_cmd_;             // one command waiting for the downloader to be free (the offline username typed during an install)
    std::vector<int> loader_kinds_;       // the Loader row's options: 0 Vanilla, 1 Fabric, 2 Quilt
    std::string loader_mc_ = "\x01";      // the Minecraft version loader_kinds_ was built for
    int loader_want_ = 0, loader_shown_ = 0;
    int focus_row_ = 0;
    int shown_row_ = 0, previous_row_ = 0;
    int shown_ = 0, previous_ = 0;
    float age_ = 0.0f, clock_ = 0.0f, boot_t_ = 0.0f;
    tween::Timer hero_;
    ui::SpringColor palette_[4];
    tween::Spring row_position_;
    BounceRect ring_;
    ui::Pulse nudge_;
    float nudge_dir_ = 0.0f;
    ui::Pulse play_;
    bool sheet_open_ = false;
    tween::Spring sheet_t_;
    tween::Bounce sheet_pop_;
    tween::Bounce tab_x_, tab_w_, hl_y_;   // the sliding glass behind the active tab / the focused list row
    mutable float tab_goal_x_ = 0.0f, tab_goal_w_ = 0.0f;   // where the tab pill should be (set while drawing)
    int glass_key_ = -1;
    ui::Form form_;
    std::string status_;
    float status_age_ = 10.0f;
    std::string finish_action_;
    float finish_left_ = 0.0f;

    // ---- tools ----
    Mode mode_ = Mode::settings;
    app::Feedback *feedback_ = nullptr;
    launcher::Jobs jobs_;
    // install page
    std::vector<launcher::VersionRow> catalog_, rows_;
    std::vector<int> view_;
    int install_tab_ = 0, install_row_ = 0;
    ui::Scroller install_scroll_;
    // mods page
    std::vector<launcher::SearchRow> results_;
    std::vector<std::string> installed_slugs_;
    int mods_tab_ = 0, mods_row_ = 0;
    std::string query_, search_key_;
    bool search_pending_ = false;
    ui::Scroller mods_scroll_;
    // installed page
    std::vector<InstalledRow> inst_rows_;
    int inst_tab_ = 0, inst_row_ = 0;
    ui::Scroller inst_scroll_;
    // library: what the jars, packs and worlds say about themselves (info jobs)
    std::map<std::string, launcher::InfoRow> info_mods_, info_packs_;
    std::vector<launcher::WorldRow> worlds_;
    std::vector<std::uint32_t> icon_tex_;   // one per world, 0 = none
    bool info_fresh_[6] = {false, false, false, false, false, false};
    float last_dt_ = 1.0f / 60.0f;
    float hud_fps_ = 60.0f;
    int fov_loaded_ = 70;
    int rd_loaded_ = 12;      // render distance as last read from the game's options
    bool rd_form_ = false, opt_pending_ = false;   // the form on screen is the instance form / the options file has not been (re)read for it yet
    std::string dp_world_, dp_world_name_, dp_world_ver_;   int dp_world_pos_ = 0;   // the world the Datapacks tab shows (folder, real name)
    int dp_world_count_ = 0;
    struct DpWorld
    {
        std::string folder, name, ver;
        int on = 0, total = 0;   // packs switched on in this world / packs in the library
    };
    std::vector<DpWorld> dp_worlds_;   // the Datapacks tab's world list, most recently played first
    tween::Bounce dp_hl_;              // the glass highlight on the chosen world
    std::vector<launcher::InfoRow> info_dp_;   // datapacks: file "world/pack" (what remove takes), loader = world folder, mc = pack name, version = bytes
    std::string info_pending_, info_loading_;   // the kind waiting for a free job slot / being read now
    std::string kb_world_, kb_world_name_;      // the world a rename is for
    std::vector<int> render_opts_;              // the Renderer row's options: 1 OpenGL, 2 OpenGL via Zink, 0 Vulkan
    std::vector<int> jdk_opts_;                 // the Java row's options: 0 automatic, else the Java version
    // confirm dialog
    bool confirm_open_ = false, confirm_close_ = false;
    std::string confirm_title_, confirm_text_, confirm_cmd_, confirm_busy_;
    tween::Spring conf_t_;
    // keyboard
    ui::Keyboard keyboard_;
    bool kb_open_ = false;
    int kb_target_ = 0; // 0 rename, 1 content search, 2 version search, 3 world rename, 4 offline username
    std::string install_query_;
    bool install_newest_ = true;
    std::string kb_text_, kb_title_;
    std::size_t kb_caret_ = 0;   // byte position of the cursor in kb_text_
    int kb_max_ = 24;
    tween::Spring kb_t_;
    // error dialog
    bool error_open_ = false;
    std::string error_title_, error_text_;
    bool error_retry_ = true;
    tween::Spring err_t_;
};

} // namespace

std::unique_ptr<app::Concept> make_launcher(app::Context &context)
{
    return std::make_unique<Launcher>(context);
}

} // namespace hui::concepts
