// sandbox test of ps5sdl.inc: drives the scripted pad through the SDL entry points exactly as LWJGL would reach them (dlopen "libSDL3.so" -> dlsym -> call)
// and checks the SDL_Event bytes at the offsets of the LWJGL 3.4.3 Layout code (see sdl_layout.py).
#include "../ps5lwjgl.c"
#include <unistd.h>
void ps5_trace(const char *m) { (void)m; }
static PadState g_script;
int scePadReadState(int h, PadState *s) { (void)h; *s = g_script; s->connected = 1; return 0; }
int scePadInit(void) { return 0; } int scePadOpen(int a, int b, int c, const void *d) { (void)a; (void)b; (void)c; (void)d; return 1; }
int sceUserServiceInitialize(void *p) { (void)p; return 0; } int sceUserServiceGetInitialUser(int *u) { *u = 1; return 0; }
size_t ps5_opengl_heap_live_bytes(void) { return 0; } void ps5_opengl_heap_stats_print(unsigned x) { (void)x; }
static void poll(unsigned b, int lx, int ly, int rx, int ry, int r2) { memset(&g_script, 0, sizeof g_script); g_script.buttons = b; g_script.lx = lx; g_script.ly = ly; g_script.rx = rx; g_script.ry = ry; g_script.r2 = r2; }
static void idle(void) { poll(0, 128, 128, 128, 128, 0); }
static int fails;
static void feed(unsigned b, int lx, int ly, int rx, int ry, int r2);
#define CHECK(c, ...) do { if (!(c)) { dprintf(1, "FAIL line %d: ", __LINE__); dprintf(1, __VA_ARGS__); dprintf(1, "\n"); fails++; } } while (0)
static void *S(const char *n) { void *f = dlsym(dlopen("libSDL3.so.0", 0), n); if (!f) { dprintf(1, "FAIL missing %s\n", n); fails++; } return f; }
#define CALL(ret, name, args, ...) ((ret (*) args)S(name))(__VA_ARGS__)
static uint32_t u32(const uint8_t *e, int o) { uint32_t v; memcpy(&v, e + o, 4); return v; }
static float f32(const uint8_t *e, int o) { float v; memcpy(&v, e + o, 4); return v; }
static int next(uint8_t *e) { return CALL(int, "SDL_PollEvent", (void *), e); }
static int drain(void) { uint8_t e[128]; int n = 0; while (next(e)) n++; return n; }
static void feed(unsigned b, int lx, int ly, int rx, int ry, int r2) { poll(b, lx, ly, rx, ry, r2); ((void (*)(void))S("SDL_PumpEvents"))(); }
static void ifeed(void) { feed(0, 128, 128, 128, 128, 0); }
int main(void) {
    idle();
    g_pad = 1; g_w = 1920; g_h = 1080; g_created = 1; g_cursor_mode = 0; g_inited = 1;
    uint8_t e[128];
    CHECK(CALL(int, "SDL_Init", (unsigned), 0x20), "init");
    void *win = CALL(void *, "SDL_CreateWindow", (const char *, int, int, uint64_t), "Minecraft", 1280, 720, 2);
    CHECK(win == WIN, "window handle");
    CHECK(next(e) && u32(e, 0) == 0x20E && u32(e, 16) == 1, "focus-gained event queued at creation");
    drain(); ifeed(); drain(); ifeed(); drain();
    // menu: Cross = left click (SDL_MouseButtonEvent: windowID 16, which 20, button 24, down 25, clicks 26, x 28, y 32)
    feed(0x4000, 128, 128, 128, 128, 0);
    CHECK(next(e) && u32(e, 0) == 0x401 && e[24] == 1 && e[25] == 1 && e[26] == 1 && u32(e, 16) == 1, "left click down: type %x button %d down %d", u32(e, 0), e[24], e[25]);
    CHECK(f32(e, 28) == 960.0f && f32(e, 32) == 540.0f, "click position %f %f", f32(e, 28), f32(e, 32));
    CHECK(CALL(unsigned, "SDL_GetMouseState", (float *, float *), 0, 0) == 1, "button mask while held");
    CHECK(!next(e), "no extra events");
    ifeed(); CHECK(next(e) && u32(e, 0) == 0x402 && e[24] == 1 && e[25] == 0, "left click up"); drain();
    // Square = right click -> SDL button 3
    feed(0x8000, 128, 128, 128, 128, 0); next(e); CHECK(u32(e, 0) == 0x401 && e[24] == 3, "right click is SDL button 3 (got %d)", e[24]); drain(); ifeed(); drain();
    // right stick in a menu moves the cursor: motion (windowID 16, which 20, state 24, x 28, y 32, xrel 36, yrel 40)
    usleep(20000); feed(0, 128, 128, 255, 128, 0); int sawm = 0;
    while (next(e)) if (u32(e, 0) == 0x400) { sawm = 1; CHECK(f32(e, 36) > 0 && f32(e, 40) == 0, "motion xrel %f yrel %f", f32(e, 36), f32(e, 40)); CHECK(f32(e, 28) > 960.0f, "motion x %f", f32(e, 28)); }
    CHECK(sawm, "motion event"); ifeed(); drain();
    // Options = Escape: key event (windowID 16, which 20, scancode 24, key 28, mod 32, down 36, repeat 37)
    feed(0x8, 128, 128, 128, 128, 0); next(e);
    CHECK(u32(e, 0) == 0x300 && u32(e, 24) == 41 && u32(e, 28) == 0x1B && e[36] == 1 && e[37] == 0, "Escape key down: type %x scancode %u key %x", u32(e, 0), u32(e, 24), u32(e, 28));
    const uint8_t *ks = CALL(const uint8_t *, "SDL_GetKeyboardState", (int *), 0); int n = 0; CALL(const uint8_t *, "SDL_GetKeyboardState", (int *), &n);
    CHECK(n == 512 && ks[41] == 1, "keyboard state: numkeys %d esc %d", n, ks[41]);
    drain(); ifeed(); next(e); CHECK(u32(e, 0) == 0x301 && e[36] == 0 && ks[41] == 0, "Escape key up");
    drain();
    // on-screen keyboard: R3 opens it, Cross types 'q' -> SDL_TextInputEvent (windowID 16, text ptr 24)
    feed(0x4, 128, 128, 128, 128, 0); drain(); ifeed(); drain();
    feed(0x4000, 128, 128, 128, 128, 0); int sawt = 0;
    while (next(e)) if (u32(e, 0) == 0x303) { const char *t; memcpy(&t, e + 24, sizeof t); sawt = 1; CHECK(t && !strcmp(t, "q"), "text input \"%s\"", t ? t : "(null)"); }
    CHECK(sawt, "text input event"); drain(); ifeed(); drain();
    feed(0x2000, 128, 128, 128, 128, 0); drain(); ifeed(); drain();   // Circle closes it
    // gameplay: relative mouse mode, L1 = wheel up (SDL_MouseWheelEvent: x 24, y 28, direction 32)
    CALL(int, "SDL_SetWindowRelativeMouseMode", (void *, int), win, 1); CHECK(g_cursor_mode == GLFW_CURSOR_DISABLED, "relative mode on");
    CHECK((CALL(unsigned long long, "SDL_GetWindowFlags", (void *), win) & 0x8000) != 0, "window flags show relative mode");
    // relative mode: g_cx starts at the centre (mode switch), many right-stick frames push it far outside the window; events stay pinned at the centre and the xrel total equals the stick movement,
    // also when the game does not read for a while (motion events are merged, nothing is lost)
    { CHECK(g_cx == 960.0 && g_cy == 540.0, "relative mode starts from the centre (%.0f %.0f)", g_cx, g_cy);
      int nm = 0, bad = 0; float sx = 0; double cx0 = g_cx;
      for (int k = 0; k < 40; k++) { usleep(20000); feed(0, 128, 128, 255, 128, 0);
        if (k >= 20) while (next(e)) if (u32(e, 0) == 0x400) { nm++; sx += f32(e, 36); if (f32(e, 28) != 960.0f || f32(e, 32) != 540.0f) bad++; } }   // frames 0..19 are not read at all
      while (next(e)) if (u32(e, 0) == 0x400) { nm++; sx += f32(e, 36); if (f32(e, 28) != 960.0f || f32(e, 32) != 540.0f) bad++; }
      float mx = 0, my = 0; CALL(unsigned, "SDL_GetMouseState", (float *, float *), &mx, &my);
      CHECK(nm >= 1 && !bad && g_cx > 1920 && fabs(sx - (g_cx - cx0)) < 2.0, "relative motion pinned at the centre: %d events, %d off-centre, sum xrel %.1f vs stick movement %.1f, g_cx %.0f", nm, bad, sx, g_cx - cx0, g_cx);
      CHECK(mx == 960.0f && my == 540.0f, "GetMouseState pinned in relative mode (%f %f)", mx, my); }
    CALL(int, "SDL_SetWindowRelativeMouseMode", (void *, int), win, 0); CHECK(g_cx == 960.0 && g_cy == 540.0 && g_cursor_mode != GLFW_CURSOR_DISABLED, "leaving relative mode re-centres the pointer (%.0f %.0f)", g_cx, g_cy);
    CALL(int, "SDL_SetWindowRelativeMouseMode", (void *, int), win, 1);
    ifeed(); drain(); feed(0x400, 128, 128, 128, 128, 0); int sawh = 0;
    while (next(e)) if (u32(e, 0) == 0x403) { sawh = 1; CHECK(f32(e, 24) == 0 && f32(e, 28) == 1.0f, "wheel x %f y %f", f32(e, 24), f32(e, 28)); }
    CHECK(sawh, "wheel event"); drain(); ifeed(); drain();
    // gameplay keys: left stick up = W (scancode 26, key 'w' = 0x77)
    feed(0, 128, 0, 128, 128, 0); next(e); CHECK(u32(e, 0) == 0x300 && u32(e, 24) == 26 && u32(e, 28) == 0x77, "W: scancode %u key %x", u32(e, 24), u32(e, 28)); drain(); ifeed(); drain();
    // flush
    feed(0x4000, 128, 128, 128, 128, 0); CALL(void, "SDL_FlushEvents", (unsigned, unsigned), 0x300, 0x3FF); CHECK(drain() == 0, "key events flushed"); ifeed(); drain();
    // displays
    int cnt = -1; unsigned *d = CALL(unsigned *, "SDL_GetDisplays", (int *), &cnt); CHECK(cnt == 1 && d[0] == 1 && d[1] == 0, "displays"); CALL(void, "SDL_free", (void *), d);
    void **m = CALL(void **, "SDL_GetFullscreenDisplayModes", (unsigned, int *), 1, &cnt); CHECK(cnt == 1 && m[0] && !m[1], "modes");
    uint8_t *dm = m[0]; CHECK(u32(dm, 0) == 1 && u32(dm, 4) == 0x16161804 && (int)u32(dm, 8) == 1920 && (int)u32(dm, 12) == 1080 && f32(dm, 20) == 60.0f, "display mode layout"); CALL(void, "SDL_free", (void *), m);
    uint8_t *pf = CALL(uint8_t *, "SDL_GetPixelFormatDetails", (unsigned), 0x16161804); CHECK(pf[24] == 8 && pf[25] == 8 && pf[26] == 8, "pixel format bits %d %d %d", pf[24], pf[25], pf[26]);
    int rect[4]; CALL(int, "SDL_GetDisplayBounds", (unsigned, int *), 1, rect); CHECK(rect[2] == 1920 && rect[3] == 1080, "bounds");
    int w = 0, h = 0; CALL(int, "SDL_GetWindowSizeInPixels", (void *, int *, int *), win, &w, &h); CHECK(w == 1920 && h == 1080, "size in pixels");
    w = h = 0; CALL(int, "SDL_GetWindowSize", (void *, int *, int *), win, &w, &h); CHECK(w == 1920 && h == 1080, "window size equals the framebuffer (mouse units): %dx%d", w, h);
    CHECK(CALL(float, "SDL_GetDisplayContentScale", (unsigned), 1) == 1.0f && CALL(float, "SDL_GetWindowDisplayScale", (void *), win) == 1.0f, "content scale 1.0");
    { int sz = 0; drain(); void *w2 = CALL(void *, "SDL_CreateWindow", (const char *, int, int, uint64_t), "Minecraft 26.3", 720, 408, 0x2022ULL); (void)w2;
      while (next(e)) { if (u32(e, 0) == 0x206 && (int)u32(e, 20) == 1920 && (int)u32(e, 24) == 1080) sz |= 1; if (u32(e, 0) == 0x207 && (int)u32(e, 20) == 1920) sz |= 2; }
      CHECK(sz == 3, "creating a 720x408 window announces resize + pixel-size 1920x1080 (%d)", sz); drain(); ifeed(); drain(); }
    // message box: buttons array of SDL_MessageBoxButtonData{flags, buttonID, text}
    struct { int fl, id; const char *tx; } bt[2] = { { 0, 7, "OK" }, { 0, 8, "No" } }; uint8_t mb[56]; memset(mb, 0, sizeof mb); const char *t = "t", *msg = "m"; void *bp = bt;
    memcpy(mb + 16, &t, 8); memcpy(mb + 24, &msg, 8); *(int *)(mb + 32) = 2; memcpy(mb + 40, &bp, 8); int pressed = 0;
    CHECK(CALL(int, "SDL_ShowMessageBox", (const void *, int *), mb, &pressed) && pressed == 7, "message box answers with the first button (%d)", pressed);
    // surface
    uint8_t px[16]; uint8_t *sf = CALL(uint8_t *, "SDL_CreateSurfaceFrom", (int, int, unsigned, void *, int), 2, 2, 0x16362004, px, 8); CHECK((int)u32(sf, 8) == 2 && (int)u32(sf, 12) == 2, "surface w/h"); CALL(void, "SDL_DestroySurface", (void *), sf);
    // clipboard round trip (malloc'd: LWJGL may SDL_free it)
    CALL(int, "SDL_SetClipboardText", (const char *), "hello"); char *cl = CALL(char *, "SDL_GetClipboardText", (void)); CHECK(cl && !strcmp(cl, "hello"), "clipboard"); CALL(void, "SDL_free", (void *), cl);
    // keymap sanity: SDL_GetKeyFromScancode(A) = 'a', with shift 'A'; key names
    CHECK(CALL(int, "SDL_GetKeyFromScancode", (int, unsigned, int), 4, 0, 0) == 'a' && CALL(int, "SDL_GetKeyFromScancode", (int, unsigned, int), 4, 1, 0) == 'A', "keycode of scancode A");
    CHECK(!strcmp(CALL(const char *, "SDL_GetKeyName", (int), 'w'), "W"), "key name");
    CHECK(CALL(int, "SDL_Vulkan_LoadLibrary", (const char *), 0) == 0, "vulkan reports unavailable");
    { void *u = dlsym(dlopen("libSDL3.so.0", 0), "SDL_DoesNotExist"); CHECK(u != 0 && ((long (*)(void))u)() == 0, "unknown SDL function gets a stub returning 0"); }
    // pad quit combo: L1 + R1 + L3 + R3 held 1.5 s asks the game to close (window-close + quit events, g_should_close); a shorter hold does nothing
    { drain(); g_should_close = 0; feed(0x400 | 0x800 | 0x2 | 0x4, 128, 128, 128, 128, 0); usleep(600000); feed(0x400 | 0x800 | 0x2 | 0x4, 128, 128, 128, 128, 0); ifeed();
      CHECK(!g_should_close, "a short hold of the quit combo must not close");
      feed(0x400 | 0x800 | 0x2 | 0x4, 128, 128, 128, 128, 0); usleep(1700000); feed(0x400 | 0x800 | 0x2 | 0x4, 128, 128, 128, 128, 0);
      int seen_close = 0, seen_quit = 0; while (next(e)) { uint32_t t = u32(e, 0); if (t == 0x210) seen_close = 1; if (t == 0x100) seen_quit = 1; }
      CHECK(g_should_close && seen_close && seen_quit, "quit combo held 1.5 s: should_close=%d close=%d quit=%d", g_should_close, seen_close, seen_quit); ifeed(); }
    // idle keep-alive (MC 26 idles at 10 fps until it sees input): every NEW game window starts from "no input yet" - a pad button still held from the launcher must not count - and the burst stops after a real event
    { feed(0x4000, 128, 128, 128, 128, 0); drain();   // cross pressed in the launcher (its press event went to the previous owner) ...
      CALL(void *, "SDL_CreateWindow", (const char *, int, int, uint64_t), "Minecraft", 1920, 1080, 2); drain();
      feed(0x4000, 128, 128, 128, 128, 0); drain();   // ... and still held when the new game starts: no new edge, no event for this game
      usleep(2200000); int shift = 0; while (next(e)) if (u32(e, 0) == 0x300 && u32(e, 24) == 225) shift = 1;
      CHECK(shift, "a new window with only a held button gets the synthetic input burst (Left Shift %d)", shift);
      feed(0, 128, 128, 128, 128, 0); drain(); feed(0, 255, 128, 128, 128, 0); usleep(50000); feed(0, 255, 128, 128, 128, 0); drain();   // a real cursor move
      usleep(2200000); shift = 0; while (next(e)) if (u32(e, 0) == 0x300 && u32(e, 24) == 225) shift = 1;
      CHECK(!shift, "no more synthetic bursts once the game has had real input (%d)", shift); ifeed(); drain(); }
    dprintf(1, "fails=%d\n", fails); return fails;
}
