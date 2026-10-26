// ps5sdl.inc: the SDL3 subset Minecraft 26.x uses (LWJGL 3.4 org.lwjgl.sdl), on top of the GLFW window / EGL / DualSense code in ps5lwjgl.c.
// LWJGL's SDL binding has no JNI library: it dlopen()s "SDL3" and calls function pointers it finds with dlsym(). dlopen/dlsym below hand out h_sdl and
// sdl_syms[]. The window, GL context, swap and pad handling are the GLFW functions above; their callbacks (which Java never sets in SDL mode) are pointed at
// the pushers here, which turn pad-generated key / mouse / scroll / text input into SDL_Event structs.
// Struct offsets (SDL_Event is a 128-byte union) were checked against the LWJGL 3.4.3 generated Layout code, see tests/sdl_test.c.
// ps5sdl_keys.inc (GLFW key -> SDL scancode / keycode) is generated from the same sources.
#include "ps5sdl_keys.inc"

enum { SDLE_QUIT = 0x100, SDLE_WIN_PIXSIZE = 0x207, SDLE_WIN_FOCUS_GAINED = 0x20E, SDLE_WIN_CLOSE = 0x210, SDLE_KEY_DOWN = 0x300, SDLE_KEY_UP = 0x301, SDLE_TEXT_INPUT = 0x303,
       SDLE_MOUSE_MOTION = 0x400, SDLE_MOUSE_DOWN = 0x401, SDLE_MOUSE_UP = 0x402, SDLE_MOUSE_WHEEL = 0x403 };
#define SDL_EV_SIZE 128
#define SDL_Q 256
static int h_sdl;
static uint8_t sdl_q[SDL_Q][SDL_EV_SIZE]; static char sdl_qtext[SDL_Q][8];
static unsigned sdl_qh, sdl_qt;              // head (read) / tail (write); the queue is full at SDL_Q entries (the oldest is dropped)
static uint8_t sdl_kb[512]; static uint16_t sdl_mod; static uint32_t sdl_mbtn;
static short sdl_g2s[KEY_MAX + 1]; static int sdl_g2k[KEY_MAX + 1]; static int sdl_map_ready, sdl_pumped;
static float sdl_lastx = -1, sdl_lasty = -1;
#ifndef PS5_VK   // ps5vk.inc (included first) defines it
static char sdl_err[128] = "";
#endif
static struct { uint32_t displayID, format; int32_t w, h; float density, refresh; int32_t num, den; void *internal; } sdl_dm;   // SDL_DisplayMode: 40 bytes
static struct { uint32_t format; uint8_t bpp, bytes, pad[2]; uint32_t rm, gm, bm, am; uint8_t rb, gb, bb, ab, rs, gs, bs, as; } sdl_pfd = { 0x16161804, 24, 4, {0, 0}, 0xFF0000, 0xFF00, 0xFF, 0, 8, 8, 8, 0, 16, 8, 0, 0 };   // XRGB8888
_Static_assert(sizeof sdl_dm == 40, "SDL_DisplayMode layout"); _Static_assert(sizeof sdl_pfd == 32, "SDL_PixelFormatDetails layout");

static void sdl_map_init(void) {
    if (sdl_map_ready) return; sdl_map_ready = 1;
    for (int i = 0; sdl_keymap[i].glfw; i++) if (sdl_keymap[i].glfw <= KEY_MAX) { sdl_g2s[sdl_keymap[i].glfw] = sdl_keymap[i].sc; sdl_g2k[sdl_keymap[i].glfw] = sdl_keymap[i].key; }
}
static uint8_t *sdl_push(uint32_t type) {
    if (sdl_qt - sdl_qh >= SDL_Q) sdl_qh++;
    unsigned i = sdl_qt++ % SDL_Q; uint8_t *e = sdl_q[i];
    memset(e, 0, SDL_EV_SIZE); memcpy(e, &type, 4);
    uint64_t ts = (uint64_t)(now_s() * 1e9); memcpy(e + 8, &ts, 8);
    return e;
}
#define EV32(e, off, v) do { uint32_t t_ = (uint32_t)(v); memcpy((e) + (off), &t_, 4); } while (0)
#define EVF(e, off, v) do { float t_ = (float)(v); memcpy((e) + (off), &t_, 4); } while (0)
static unsigned sdl_cur_slot(void) { return (sdl_qt - 1) % SDL_Q; }

static uint16_t sdl_modbit(int sc) {
    switch (sc) { case 225: return 0x1; case 229: return 0x2; case 224: return 0x40; case 228: return 0x80; case 226: return 0x100; case 230: return 0x200; case 227: return 0x400; case 231: return 0x800; }
    return 0;
}
static int sdl_in_burst;
static int sdl_nb;   // synthetic bursts logged for this game
static void sdl_on_key(void *w, int k, int s, int act, int mods) {
    (void)w; (void)s; (void)mods; sdl_map_init();
    if (k < 0 || k > KEY_MAX || !sdl_g2s[k]) return;
    int sc = sdl_g2s[k], down = act != GLFW_RELEASE;
    uint16_t mb = sdl_modbit(sc); if (mb) { if (down) sdl_mod |= mb; else sdl_mod &= (uint16_t)~mb; }
    sdl_kb[sc] = (uint8_t)down;
    uint8_t *e = sdl_push(down ? SDLE_KEY_DOWN : SDLE_KEY_UP);
    EV32(e, 16, 1); EV32(e, 20, 1); EV32(e, 24, sc); EV32(e, 28, sdl_g2k[k]); memcpy(e + 32, &sdl_mod, 2); e[36] = (uint8_t)down; e[37] = 0;
}
static void sdl_on_char(void *w, unsigned c) {
    (void)w; if (!c) return;
    uint8_t *e = sdl_push(SDLE_TEXT_INPUT); char *t = sdl_qtext[sdl_cur_slot()];
    if (c < 0x80) { t[0] = (char)c; t[1] = 0; }
    else if (c < 0x800) { t[0] = (char)(0xC0 | (c >> 6)); t[1] = (char)(0x80 | (c & 0x3F)); t[2] = 0; }
    else if (c < 0x10000) { t[0] = (char)(0xE0 | (c >> 12)); t[1] = (char)(0x80 | ((c >> 6) & 0x3F)); t[2] = (char)(0x80 | (c & 0x3F)); t[3] = 0; }
    else { t[0] = (char)(0xF0 | (c >> 18)); t[1] = (char)(0x80 | ((c >> 12) & 0x3F)); t[2] = (char)(0x80 | ((c >> 6) & 0x3F)); t[3] = (char)(0x80 | (c & 0x3F)); t[4] = 0; }
    EV32(e, 16, 1); void *p = t; memcpy(e + 24, &p, sizeof p);
}
static unsigned long sdl_st_btn, sdl_st_btn_polled;   // mouse-button events pushed by the pad code / pulled by the game
static void sdl_on_mouse(void *w, int b, int act, int mods) {
    (void)w; (void)mods; int sb = b == 0 ? 1 : b == 1 ? 3 : 2, down = act != GLFW_RELEASE;
    if (down) sdl_mbtn |= 1u << (sb - 1); else sdl_mbtn &= ~(1u << (sb - 1));
    sdl_st_btn++; { static int nlog; if (nlog < 16) { nlog++; TR("sdl: pad -> mouse button %d %s at (%.0f, %.0f)", sb, down ? "down" : "up", g_cx, g_cy); } }
    uint8_t *e = sdl_push(down ? SDLE_MOUSE_DOWN : SDLE_MOUSE_UP);
    EV32(e, 16, 1); EV32(e, 20, 1); e[24] = (uint8_t)sb; e[25] = (uint8_t)down; e[26] = 1; EVF(e, 28, g_cx); EVF(e, 32, g_cy);
}
static unsigned long sdl_st_motion, sdl_st_polled, sdl_st_warp; static double sdl_st_sumx, sdl_st_sumy; static int sdl_grab;   // input diagnostics ("sdl: input ..." every 5 s)
static unsigned long sdl_st_motion_polled, sdl_st_merged;
static void sdl_on_pos(void *w, double x, double y) {
    (void)w; if (sdl_lastx < 0) { sdl_lastx = (float)x; sdl_lasty = (float)y; }
    int rel = g_cursor_mode == GLFW_CURSOR_DISABLED;
    // Relative (gameplay) mode: real SDL pins the pointer; only xrel/yrel carry the camera. The movement comes straight from this poll's stick delta (g_rel_dx/dy), not from differences of the absolute
    // position (which accumulates without bounds and used to depend on warps / mode switches), and the event reports the window centre.
    double dx = rel ? g_rel_dx : x - sdl_lastx, dy = rel ? g_rel_dy : y - sdl_lasty, px = rel ? g_w / 2.0 : x, py = rel ? g_h / 2.0 : y;
    sdl_lastx = (float)x; sdl_lasty = (float)y;
    if (rel && dx == 0 && dy == 0) return;
   
    sdl_st_motion++; sdl_st_sumx += dx < 0 ? -dx : dx; sdl_st_sumy += dy < 0 ? -dy : dy;
    // Motion events not yet read by the game are merged (the queue only holds 256 and drops the oldest: a render stall must not eat camera movement, or worse a key/button release)
    if (sdl_qt != sdl_qh) { uint8_t *l = sdl_q[(sdl_qt - 1) % SDL_Q]; uint32_t ty; memcpy(&ty, l, 4);
        if (ty == SDLE_MOUSE_MOTION) { float ox, oy; memcpy(&ox, l + 36, 4); memcpy(&oy, l + 40, 4); EVF(l, 28, px); EVF(l, 32, py); EVF(l, 36, ox + dx); EVF(l, 40, oy + dy); sdl_st_merged++; return; } }
    uint8_t *e = sdl_push(SDLE_MOUSE_MOTION);
    EV32(e, 16, 1); EV32(e, 20, 1); EV32(e, 24, sdl_mbtn); EVF(e, 28, px); EVF(e, 32, py); EVF(e, 36, dx); EVF(e, 40, dy);
}
static void sdl_on_scroll(void *w, double dx, double dy) {
    (void)w; uint8_t *e = sdl_push(SDLE_MOUSE_WHEEL);
    EV32(e, 16, 1); EV32(e, 20, 1); EVF(e, 24, dx); EVF(e, 28, dy); EVF(e, 36, g_cx); EVF(e, 40, g_cy); EV32(e, 44, (int)dx); EV32(e, 48, (int)dy);
}
static void sdl_size_events(void);
static void sdl_on_close(void *w) { (void)w; uint8_t *e = sdl_push(SDLE_WIN_CLOSE); EV32(e, 16, 1); sdl_push(SDLE_QUIT); }   // the pad quit combo (ps5lwjgl.c): the game gets the window-close request and quits through its own path
static void sdl_pump(void) { glfwPollEvents(); sdl_pumped = 1; }

// ---- init / events ----
int SDL_Init(unsigned flags) { (void)flags; glfwInit(); TR("sdl: SDL_Init(0x%x)", flags); return 1; }
void SDL_Quit(void) {}
int SDL_SetHint(const char *n, const char *v) { TR("sdl: hint %s=%s", n ? n : "?", v ? v : "?"); return 1; }
int SDL_SetAppMetadataProperty(const char *n, const char *v) { (void)n; (void)v; return 1; }
const char *SDL_GetError(void) { return sdl_err; }
const char *SDL_GetPlatform(void) { return "Linux"; }   // LWJGL's Platform says LINUX too
void SDL_PumpEvents(void) { sdl_pump(); }
int SDL_PollEvent(void *out) {
    // Minecraft 26's FramerateLimitTracker caps the frame rate at 10 fps while it believes nobody has touched an input device (its "latest input" time starts at zero; measured: flat 100 ms frames at the title
    // screen until the first pad input, then 60 fps). Until the first real input, a harmless burst (zero-delta mouse move, Left Shift tap) is queued every 2 s, once the main loop is polling.
    if (!sdl_real_in && g_created) { static double t_burst; double tb = now_s(); if (tb - t_burst > 2.0) { t_burst = tb; if (sdl_nb < 3) { sdl_nb++; TR("sdl: no input yet: queued a synthetic input burst (#%d) so the game does not treat the console as idle", sdl_nb); }
        sdl_in_burst = 1;
        { static const unsigned wev[] = { 0x202 /*SHOWN*/, 0x20C /*MOUSE_ENTER*/, SDLE_WIN_FOCUS_GAINED }; for (int q = 0; q < 3; q++) { uint8_t *we = sdl_push(wev[q]); EV32(we, 16, 1); } sdl_size_events(); }   // the window-state events too: the game may be waiting for "focus gained" (the ones sent at window creation can arrive before it is listening)
        sdl_on_pos(WIN, g_cx, g_cy); sdl_on_key(WIN, 340, 0, GLFW_PRESS, 0); sdl_on_key(WIN, 340, 0, GLFW_RELEASE, 0); sdl_in_burst = 0; } }
    { static double t_log; double t = now_s(); sdl_st_polled++;
      if (t - t_log > 5.0) { if (t_log > 0) TR("sdl: in A motion=%lu polls=%lu warps=%lu mode=0x%x grab=%d", sdl_st_motion, sdl_st_polled, sdl_st_warp, g_cursor_mode, sdl_grab);
        if (t_log > 0) TR("sdl: in B pulled=%lu merged=%lu gap=%.0fms capped=%lu lost=%.0fpx btn=%lu/%lu", sdl_st_motion_polled, sdl_st_merged, g_in_maxgap * 1e3, g_in_capped, g_in_lostpx, sdl_st_btn, sdl_st_btn_polled);
        g_in_maxgap = g_in_lostpx = 0; g_in_capped = 0;
        t_log = t; sdl_st_btn = sdl_st_btn_polled = 0; sdl_st_motion_polled = sdl_st_merged = 0; sdl_st_motion = sdl_st_polled = sdl_st_warp = 0; sdl_st_sumx = sdl_st_sumy = 0; } }
    if (sdl_qh == sdl_qt) { if (!sdl_pumped) { sdl_pump(); } if (sdl_qh == sdl_qt) { sdl_pumped = 0; return 0; } }
    unsigned i = sdl_qh++ % SDL_Q; if (out) memcpy(out, sdl_q[i], SDL_EV_SIZE);
    { uint32_t ty; memcpy(&ty, sdl_q[i], 4); if (ty == SDLE_MOUSE_DOWN || ty == SDLE_MOUSE_UP) sdl_st_btn_polled++; else if (ty == SDLE_MOUSE_MOTION) sdl_st_motion_polled++; }
    return 1;
}
void SDL_FlushEvents(unsigned lo, unsigned hi) {
    unsigned n = 0, t = sdl_qt;
    for (unsigned j = sdl_qh; j != t; j++) { uint32_t ty; memcpy(&ty, sdl_q[j % SDL_Q], 4); if (ty >= lo && ty <= hi) continue; if (n != j - sdl_qh) { memcpy(sdl_q[(sdl_qh + n) % SDL_Q], sdl_q[j % SDL_Q], SDL_EV_SIZE); memcpy(sdl_qtext[(sdl_qh + n) % SDL_Q], sdl_qtext[j % SDL_Q], 8); } n++; }
    sdl_qt = sdl_qh + n;
}
void *SDL_GetWindowFromEvent(const void *e) { (void)e; return WIN; }
void SDL_free(void *p) { free(p); }
uint64_t SDL_GetTicksNS(void) { return (uint64_t)(now_s() * 1e9); }

// ---- window / display / GL ----
static void sdl_dm_fill(void) { sdl_dm.displayID = 1; sdl_dm.format = 0x16161804; sdl_dm.w = g_w; sdl_dm.h = g_h; sdl_dm.density = 1.0f; sdl_dm.refresh = 60.0f; sdl_dm.num = 60; sdl_dm.den = 1; }
// The game asks for a small window (26.3: 720x408) but the console output is g_w x g_h. SDL reports mouse positions in WINDOW units and Minecraft scales them by framebuffer/window size, so the window size
// must be the real one (measured on the console: the cursor was drawn at the pad position but the game saw it 1920/720 = 2.67x further out, so only the top-left of the screen was clickable).
static void sdl_size_events(void) {
    uint8_t *e = sdl_push(0x206 /*WINDOW_RESIZED*/); EV32(e, 16, 1); EV32(e, 20, g_w); EV32(e, 24, g_h);
    e = sdl_push(SDLE_WIN_PIXSIZE); EV32(e, 16, 1); EV32(e, 20, g_w); EV32(e, 24, g_h);
}
int SDL_GetWindowSize(void *w, int *a, int *b) { (void)w; if (a) *a = g_w; if (b) *b = g_h; return 1; }
int SDL_GetWindowSafeArea(void *w, int *r) { (void)w; if (r) { r[0] = 0; r[1] = 0; r[2] = g_w; r[3] = g_h; } return 1; }
float SDL_GetDisplayContentScale(unsigned id) { (void)id; return 1.0f; }
float SDL_GetWindowDisplayScale(void *w) { (void)w; return 1.0f; }
void *SDL_CreateWindow(const char *title, int w, int h, uint64_t flags) {
    TR("sdl: SDL_CreateWindow(\"%s\", %d, %d, flags 0x%llx)", title ? title : "", w, h, (unsigned long long)flags);
    sdl_real_in = 0; sdl_pad_armed = 0; sdl_nb = 0;   // a new game: MC 26 idles at 10 fps until it sees input, so the synthetic bursts restart. Input only counts once the pad was seen at rest (see the pad poll): a button still held from the launcher menu produced no event the game keeps and, counted as input, switched the bursts off (round 2+ and a first round with a late finger stayed at 10 fps until the cursor moved)
    void *win = glfwCreateWindow(w, h, title, 0, 0);
    if (!win) { snprintf(sdl_err, sizeof sdl_err, "EGL window creation failed"); return 0; }
    cb_key = sdl_on_key; cb_char = sdl_on_char; cb_mouse = sdl_on_mouse; cb_pos = sdl_on_pos; cb_scroll = sdl_on_scroll; cb_close = (void *)sdl_on_close;   // the pad now produces SDL events
    uint8_t *e = sdl_push(SDLE_WIN_FOCUS_GAINED); EV32(e, 16, 1);
    sdl_size_events();
    return win;
}
void SDL_DestroyWindow(void *w) { (void)w; }
unsigned long long SDL_GetWindowFlags(void *w) {
    (void)w;
#ifdef PS5_VK
    unsigned long long f = 0x1 | (g_use_gl ? 0x2 : 0x10000000) | 0x200 | 0x400 | 0x2000;   // FULLSCREEN | OPENGL or VULKAN (AIO: chosen at start-up) | INPUT_FOCUS | MOUSE_FOCUS | HIGH_PIXEL_DENSITY
#else
    unsigned long long f = 0x1 | 0x2 | 0x200 | 0x400 | 0x2000;   // FULLSCREEN | OPENGL | INPUT_FOCUS | MOUSE_FOCUS | HIGH_PIXEL_DENSITY
#endif
    if (g_cursor_mode == GLFW_CURSOR_DISABLED) f |= 0x100 | 0x8000;     // MOUSE_GRABBED | MOUSE_RELATIVE_MODE
    return f;
}
int SDL_GetWindowSizeInPixels(void *w, int *a, int *b) { (void)w; if (a) *a = g_w; if (b) *b = g_h; return 1; }
int SDL_GetWindowPosition(void *w, int *a, int *b) { (void)w; if (a) *a = 0; if (b) *b = 0; return 1; }
float SDL_GetWindowPixelDensity(void *w) { (void)w; return 1.0f; }
int SDL_SetWindowTitle(void *w, const char *t) { (void)w; (void)t; return 1; }
int SDL_SetWindowIcon(void *w, void *s) { (void)w; (void)s; return 1; }
int SDL_SetWindowSize(void *w, int a, int b) { (void)w; (void)a; (void)b; return 1; }
int SDL_SetWindowPosition(void *w, int a, int b) { (void)w; (void)a; (void)b; return 1; }
int SDL_SetWindowMinimumSize(void *w, int a, int b) { (void)w; (void)a; (void)b; return 1; }
int SDL_SetWindowMaximumSize(void *w, int a, int b) { (void)w; (void)a; (void)b; return 1; }
int SDL_SetWindowBordered(void *w, int b) { (void)w; (void)b; return 1; }
int SDL_SetWindowFullscreen(void *w, int f) { (void)w; (void)f; return 1; }   // the console output is always full screen
int SDL_SetWindowFullscreenMode(void *w, const void *m) { (void)w; (void)m; return 1; }
int SDL_RestoreWindow(void *w) { (void)w; return 1; }
int SDL_SyncWindow(void *w) { (void)w; return 1; }
int SDL_SetWindowMouseGrab(void *w, int g) { (void)w; sdl_grab = g; TR("sdl: mouse grab %d", g); return 1; }
const void *SDL_GetWindowFullscreenMode(void *w) { (void)w; sdl_dm_fill(); return &sdl_dm; }
const void *SDL_GetCurrentDisplayMode(unsigned id) { (void)id; sdl_dm_fill(); return &sdl_dm; }
const void *SDL_GetDesktopDisplayMode(unsigned id) { (void)id; sdl_dm_fill(); return &sdl_dm; }
int SDL_GetClosestFullscreenDisplayMode(unsigned id, int w, int h, float r, int hd, void *out) { (void)id; (void)w; (void)h; (void)r; (void)hd; sdl_dm_fill(); if (out) memcpy(out, &sdl_dm, sizeof sdl_dm); return 1; }
void *SDL_GetFullscreenDisplayModes(unsigned id, int *count) {
    (void)id; sdl_dm_fill(); void **a = (void **)malloc(2 * sizeof(void *)); if (!a) { if (count) *count = 0; return 0; }
    a[0] = &sdl_dm; a[1] = 0; if (count) *count = 1; return a;
}
unsigned *SDL_GetDisplays(int *count) { unsigned *a = (unsigned *)malloc(2 * sizeof(unsigned)); if (!a) { if (count) *count = 0; return 0; } a[0] = 1; a[1] = 0; if (count) *count = 1; return a; }
unsigned SDL_GetPrimaryDisplay(void) { return 1; }
unsigned SDL_GetDisplayForWindow(void *w) { (void)w; return 1; }
const char *SDL_GetDisplayName(unsigned id) { (void)id; return "PS5 display"; }
const char *SDL_GetCurrentVideoDriver(void) { return "ps5"; }
int SDL_GetDisplayBounds(unsigned id, int *r) { (void)id; if (r) { r[0] = 0; r[1] = 0; r[2] = g_w; r[3] = g_h; } return 1; }
const void *SDL_GetPixelFormatDetails(unsigned f) { (void)f; return &sdl_pfd; }

int SDL_GL_LoadLibrary(const char *p) { (void)p; return 1; }
void SDL_GL_UnloadLibrary(void) {}
int SDL_GL_SetAttribute(int a, int v) { TR("sdl: GL attribute %d = %d", a, v); return 1; }
int SDL_GL_GetAttribute(int a, int *v) {
    static const int val[] = { 8, 8, 8, 8, 32, 1, 24, 8 };   // RED GREEN BLUE ALPHA BUFFER DOUBLEBUFFER DEPTH STENCIL
    int r = a >= 0 && a < 8 ? val[a] : a == 17 ? 4 : a == 18 ? 6 : a == 20 ? 1 : a == 15 ? 1 : 0;   // major 4, minor 6, profile CORE, accelerated
    if (v) *v = r; return 1;
}
void *SDL_GL_CreateContext(void *w) { (void)w; TR("sdl: GL_CreateContext"); return g_created ? (void *)g_ctx : 0; }
int SDL_GL_DestroyContext(void *c) { (void)c; return 1; }
int SDL_GL_MakeCurrent(void *w, void *c) { (void)c; glfwMakeContextCurrent(w ? w : WIN); return 1; }
int SDL_GL_SetSwapInterval(int i) { glfwSwapInterval(i); return 1; }
int SDL_GL_SwapWindow(void *w) { glfwSwapBuffers(w ? w : WIN); return 1; }
void *SDL_GL_GetProcAddress(const char *n) { return n ? gl_proc(n) : 0; }

// ---- input ----
int SDL_SetWindowRelativeMouseMode(void *w, int on) {
    (void)w; TR("sdl: relative mouse mode %d (pointer was at %.0f, %.0f; stick right %.2f)", on, g_cx, g_cy, g_rel_dx);
    g_cursor_mode = on ? GLFW_CURSOR_DISABLED : 0x34001; sdl_lastx = -1;
    g_cx = g_w / 2.0; g_cy = g_h / 2.0; if (!on) g_cur_vis = 0;   // both ways start from the centre: the camera drifted g_cx/g_cy far off screen, and a menu must not open with the pointer in a corner (GLFW path: glfwSetInputMode does the same)
    return 1;
}
void SDL_WarpMouseInWindow(void *w, float x, float y) { (void)w; sdl_st_warp++; g_cx = x; g_cy = y; sdl_lastx = (float)x; sdl_lasty = (float)y; }
unsigned SDL_GetMouseState(float *x, float *y) { int rel = g_cursor_mode == GLFW_CURSOR_DISABLED; if (x) *x = rel ? g_w / 2.0f : (float)g_cx; if (y) *y = rel ? g_h / 2.0f : (float)g_cy; return sdl_mbtn; }   // relative mode: pinned at the centre, like the events
unsigned SDL_GetGlobalMouseState(float *x, float *y) { return SDL_GetMouseState(x, y); }
void *SDL_CreateSystemCursor(int id) { return (void *)(intptr_t)(0x100 + id); }
void *SDL_GetDefaultCursor(void) { return (void *)(intptr_t)0x100; }
int SDL_SetCursor(void *c) { static int n; if (n++ < 6) TR("sdl: SetCursor(%p)", c); return 1; }
const uint8_t *SDL_GetKeyboardState(int *n) { if (n) *n = 512; return sdl_kb; }
unsigned SDL_GetModState(void) { return sdl_mod; }
int SDL_GetKeyFromScancode(int sc, unsigned mod, int ev) {
    (void)ev; sdl_map_init();
    for (int i = 0; sdl_keymap[i].glfw; i++) if (sdl_keymap[i].sc == sc) { int k = sdl_keymap[i].key; if ((mod & 3) && k >= 'a' && k <= 'z') k -= 32; return k; }
    return 0;
}
const char *SDL_GetKeyName(int key) {
    static char nm[8];
    if (key >= 'a' && key <= 'z') key -= 32;
    if (key >= 0x20 && key < 0x7F) { nm[0] = (char)key; nm[1] = 0; return nm; }
    switch (key) { case 0x0D: return "Return"; case 0x1B: return "Escape"; case 0x08: return "Backspace"; case 0x09: return "Tab"; case 0x40000052: return "Up"; case 0x40000051: return "Down";
                   case 0x40000050: return "Left"; case 0x4000004F: return "Right"; case 0x400000E1: return "Left Shift"; case 0x400000E0: return "Left Ctrl"; case 0x400000E2: return "Left Alt"; }
    if (key >= 0x4000003A && key <= 0x40000045) { snprintf(nm, sizeof nm, "F%d", key - 0x4000003A + 1); return nm; }
    return "";
}
int SDL_StartTextInput(void *w) { (void)w; return 1; }
int SDL_StopTextInput(void *w) { (void)w; return 1; }
int SDL_ClearComposition(void *w) { (void)w; return 1; }
int SDL_SetTextInputArea(void *w, const void *r, int c) { (void)w; (void)r; (void)c; return 1; }

// ---- clipboard / message boxes / misc ----
static char *sdl_clip;
int SDL_SetClipboardText(const char *s) { free(sdl_clip); sdl_clip = s ? strdup(s) : 0; return 1; }
char *SDL_GetClipboardText(void) { return strdup(sdl_clip ? sdl_clip : ""); }
int SDL_ShowSimpleMessageBox(unsigned fl, const char *t, const char *m, void *w) { (void)fl; (void)w; TR("sdl: message box \"%s\": %s", t ? t : "", m ? m : ""); return 1; }
int SDL_ShowMessageBox(const uint8_t *d, int *btn) {
    const char *t = 0, *m = 0; void *bp = 0; int nb = 0;
    if (d) { memcpy(&t, d + 16, sizeof t); memcpy(&m, d + 24, sizeof m); nb = *(const int *)(d + 32); memcpy(&bp, d + 40, sizeof bp); }
    TR("sdl: message box \"%s\": %s", t ? t : "", m ? m : "");
    if (btn) *btn = nb > 0 && bp ? ((const int *)bp)[1] : -1;   // the first button's buttonID (SDL_MessageBoxButtonData: flags, buttonID, text)
    return 1;
}
int SDL_OpenURL(const char *u) { TR("sdl: OpenURL %s ignored", u ? u : ""); return 0; }
void SDL_SetLogOutputFunction(void *f, void *u) { (void)f; (void)u; }
void SDL_SetLogPriorities(int p) { (void)p; }
void *SDL_CreateSurfaceFrom(int w, int h, unsigned fmt, void *px, int pitch) {
    uint8_t *s = (uint8_t *)calloc(1, 48); if (!s) return 0;
    memcpy(s + 4, &fmt, 4); memcpy(s + 8, &w, 4); memcpy(s + 12, &h, 4); memcpy(s + 16, &pitch, 4); memcpy(s + 24, &px, sizeof px); *(int *)(s + 32) = 1;
    return s;
}
void SDL_DestroySurface(void *s) { free(s); }
int SDL_AddSurfaceAlternateImage(void *a, void *b) { (void)a; (void)b; return 1; }
// Vulkan: not available (the OpenGL backend is used)
#ifndef PS5_VK   // the Vulkan build has the real ones (ps5vk.inc)
int SDL_Vulkan_LoadLibrary(const char *p) { (void)p; snprintf(sdl_err, sizeof sdl_err, "Vulkan is not available on this build"); return 0; }
void SDL_Vulkan_UnloadLibrary(void) {}
void *SDL_Vulkan_GetVkGetInstanceProcAddr(void) { return 0; }
const char *const *SDL_Vulkan_GetInstanceExtensions(unsigned *n) { if (n) *n = 0; return 0; }
int SDL_Vulkan_GetPresentationSupport(void *i, void *d, unsigned q) { (void)i; (void)d; (void)q; return 0; }
int SDL_Vulkan_CreateSurface(void *w, void *i, const void *a, void *s) { (void)w; (void)i; (void)a; (void)s; return 0; }
#endif

#define SDLSYM(n) { #n, (void *)n }
static const Sym sdl_syms[] = {
    SDLSYM(SDL_Init), SDLSYM(SDL_Quit), SDLSYM(SDL_SetHint), SDLSYM(SDL_SetAppMetadataProperty), SDLSYM(SDL_GetError), SDLSYM(SDL_GetPlatform), SDLSYM(SDL_PumpEvents), SDLSYM(SDL_PollEvent),
    SDLSYM(SDL_FlushEvents), SDLSYM(SDL_GetWindowFromEvent), SDLSYM(SDL_free), SDLSYM(SDL_GetTicksNS), SDLSYM(SDL_CreateWindow), SDLSYM(SDL_DestroyWindow), SDLSYM(SDL_GetWindowFlags),
    SDLSYM(SDL_GetWindowSizeInPixels), SDLSYM(SDL_GetWindowSize), SDLSYM(SDL_GetWindowSafeArea), SDLSYM(SDL_GetDisplayContentScale), SDLSYM(SDL_GetWindowDisplayScale), SDLSYM(SDL_GetWindowPosition), SDLSYM(SDL_GetWindowPixelDensity), SDLSYM(SDL_SetWindowTitle), SDLSYM(SDL_SetWindowIcon), SDLSYM(SDL_SetWindowSize),
    SDLSYM(SDL_SetWindowPosition), SDLSYM(SDL_SetWindowMinimumSize), SDLSYM(SDL_SetWindowMaximumSize), SDLSYM(SDL_SetWindowBordered), SDLSYM(SDL_SetWindowFullscreen),
    SDLSYM(SDL_SetWindowFullscreenMode), SDLSYM(SDL_RestoreWindow), SDLSYM(SDL_SyncWindow), SDLSYM(SDL_SetWindowMouseGrab), SDLSYM(SDL_GetWindowFullscreenMode),
    SDLSYM(SDL_GetCurrentDisplayMode), SDLSYM(SDL_GetDesktopDisplayMode), SDLSYM(SDL_GetClosestFullscreenDisplayMode), SDLSYM(SDL_GetFullscreenDisplayModes), SDLSYM(SDL_GetDisplays),
    SDLSYM(SDL_GetPrimaryDisplay), SDLSYM(SDL_GetDisplayForWindow), SDLSYM(SDL_GetDisplayName), SDLSYM(SDL_GetCurrentVideoDriver), SDLSYM(SDL_GetDisplayBounds),
    SDLSYM(SDL_GetPixelFormatDetails), SDLSYM(SDL_GL_LoadLibrary), SDLSYM(SDL_GL_UnloadLibrary), SDLSYM(SDL_GL_SetAttribute), SDLSYM(SDL_GL_GetAttribute), SDLSYM(SDL_GL_CreateContext),
    SDLSYM(SDL_GL_DestroyContext), SDLSYM(SDL_GL_MakeCurrent), SDLSYM(SDL_GL_SetSwapInterval), SDLSYM(SDL_GL_SwapWindow), SDLSYM(SDL_GL_GetProcAddress),
    SDLSYM(SDL_SetWindowRelativeMouseMode), SDLSYM(SDL_WarpMouseInWindow), SDLSYM(SDL_GetMouseState), SDLSYM(SDL_GetGlobalMouseState), SDLSYM(SDL_CreateSystemCursor),
    SDLSYM(SDL_GetDefaultCursor), SDLSYM(SDL_SetCursor), SDLSYM(SDL_GetKeyboardState), SDLSYM(SDL_GetModState), SDLSYM(SDL_GetKeyFromScancode), SDLSYM(SDL_GetKeyName),
    SDLSYM(SDL_StartTextInput), SDLSYM(SDL_StopTextInput), SDLSYM(SDL_ClearComposition), SDLSYM(SDL_SetTextInputArea), SDLSYM(SDL_SetClipboardText), SDLSYM(SDL_GetClipboardText),
    SDLSYM(SDL_ShowSimpleMessageBox), SDLSYM(SDL_ShowMessageBox), SDLSYM(SDL_OpenURL), SDLSYM(SDL_SetLogOutputFunction), SDLSYM(SDL_SetLogPriorities), SDLSYM(SDL_CreateSurfaceFrom),
    SDLSYM(SDL_DestroySurface), SDLSYM(SDL_AddSurfaceAlternateImage), SDLSYM(SDL_Vulkan_LoadLibrary), SDLSYM(SDL_Vulkan_UnloadLibrary), SDLSYM(SDL_Vulkan_GetVkGetInstanceProcAddr),
    SDLSYM(SDL_Vulkan_GetInstanceExtensions), SDLSYM(SDL_Vulkan_GetPresentationSupport), SDLSYM(SDL_Vulkan_CreateSurface),
    { 0, 0 } };
