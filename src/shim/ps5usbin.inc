// ps5usbin.inc - USB keyboard and mouse for the title (included by ps5lwjgl.c right before glfwPollEvents).
//
// The console reads USB keyboards and mice through its own libSceKeyboard / libSceMouse. They are not linked into the title: they are loaded with sceSysmoduleLoadModule (ids 0x0106 and 0x00a9),
// the module handle comes from sceSysmoduleGetModuleHandleInternal and the functions from sceKernelDlsym on that handle (in_sym() below tries the plain name, the NID and the stored full name).
// Record layouts (the ones the public PS4/PS5 reimplementations agree on; the first records are logged in hex as "usb: ..." lines so a layout that differs on this firmware shows in the first run):
//   keyboard state, 96 bytes: u64 timestamp; [16] u8 connected; [20] s32 key count; [24] u32 locks; [28] u32 modifier bits (USB boot report: 0 LCtrl 1 LShift 2 LAlt 3 LGui 4 RCtrl 5 RShift 6 RAlt 7 RGui);
//                             [32] u16 USB HID usages[16]
//   mouse record, 40 bytes:   u64 timestamp; [8] u8 connected; [12] u32 buttons (bit0 left 1 right 2 middle 3 back 4 forward); [16] s32 x, [20] s32 y (motion since the last record), [24] s32 wheel, [28] s32 tilt
// Everything here feeds the same callbacks the pad does (cb_key / cb_char / cb_mouse / cb_pos / cb_scroll), so GLFW, SDL and LWJGL 2 games all get it, whatever the Minecraft version.
// The launcher uses ps5_usb_pad_bits(): the keyboard and mouse as virtual pad buttons, so every screen of it is operable without a pad.
#include <stdint.h>
#include <string.h>

// ---- pure part (no console calls): USB HID usage -> GLFW key, characters, diffing. Tested by tests/usb_test.c ----
#define USB_MODBASE 0xE0   // the HID modifier usages 0xE0..0xE7 are used as pseudo-keys in the snapshot
static int usb_glfw_key(int u) {   // GLFW key code for a keyboard-page usage; -1 when there is none
    if (u >= 4 && u <= 29) return 65 + (u - 4);            // A-Z
    if (u >= 30 && u <= 38) return 49 + (u - 30);          // 1-9
    if (u == 39) return 48;                                // 0
    if (u >= 58 && u <= 69) return 290 + (u - 58);         // F1-F12
    if (u >= 104 && u <= 112) return 302 + (u - 104);      // F13-F21
    if (u >= 89 && u <= 97) return 321 + (u - 89);         // keypad 1-9
    switch (u) {
    case 40: return 257; case 41: return 256; case 42: return 259; case 43: return 258; case 44: return 32;   // Enter Esc Backspace Tab Space
    case 45: return 45; case 46: return 61; case 47: return 91; case 48: return 93; case 49: return 92;       // - = [ ] \.
    case 50: return 92; case 51: return 59; case 52: return 39; case 53: return 96; case 54: return 44; case 55: return 46; case 56: return 47;   // non-US # ; ' ` , . /
    case 57: return 280; case 70: return 283; case 71: return 281; case 72: return 284;                       // CapsLock PrintScreen ScrollLock Pause
    case 73: return 260; case 74: return 268; case 75: return 266; case 76: return 261; case 77: return 269; case 78: return 267;   // Insert Home PgUp Delete End PgDn
    case 79: return 262; case 80: return 263; case 81: return 264; case 82: return 265;                       // Right Left Down Up
    case 83: return 282; case 84: return 331; case 85: return 332; case 86: return 333; case 87: return 334; case 88: return 335;   // NumLock KP/ KP* KP- KP+ KPEnter
    case 98: return 320; case 99: return 330; case 100: return 161; case 101: return 348; case 103: return 336;   // KP0 KP. non-US\ Menu KP=
    case 0xE0: return 341; case 0xE1: return 340; case 0xE2: return 342; case 0xE3: return 343;               // LCtrl LShift LAlt LGui
    case 0xE4: return 345; case 0xE5: return 344; case 0xE6: return 346; case 0xE7: return 347;               // RCtrl RShift RAlt RGui
    }
    return -1;
}
static int usb_is_mod(int u) { return u >= USB_MODBASE && u <= USB_MODBASE + 7; }
// The character a key types on a US layout (0 = none). shift / caps follow a PC keyboard; numlock is assumed on for the keypad.
static unsigned usb_char(int u, int shift, int caps) {
    if (u >= 4 && u <= 29) { int up = shift ^ caps; return (unsigned)((up ? 'A' : 'a') + (u - 4)); }
    if (u >= 30 && u <= 39) { static const char un[] = "1234567890", sh[] = ")!@#$%^&*("; int i = u - 30; return (unsigned char)(shift ? (u == 39 ? sh[0] : sh[i + 1]) : un[i]); }
    if (u == 44) return ' ';
    if (u >= 45 && u <= 49) { static const char un[] = "-=[]\\", sh[] = "_+{}|"; return (unsigned char)(shift ? sh[u - 45] : un[u - 45]); }
    if (u == 50) return shift ? '~' : '#';
    if (u >= 51 && u <= 56) { static const char un[] = ";'`,./", sh[] = ":\"~<>?"; return (unsigned char)(shift ? sh[u - 51] : un[u - 51]); }
    if (u >= 89 && u <= 97) return (unsigned)('1' + (u - 89));
    if (u == 98) return '0'; if (u == 99) return '.'; if (u == 84) return '/'; if (u == 85) return '*'; if (u == 86) return '-'; if (u == 87) return '+'; if (u == 103) return '=';
    return 0;
}
typedef struct { uint8_t down[256]; } UsbKeys;   // snapshot: down[usage] for usages 0..0xE7
// Decodes one keyboard state record into out (OR-ed in); returns 1 when the keyboard is connected.
static int usb_decode_keyboard(const uint8_t st[96], UsbKeys *out) {
    if (!st[16]) return 0;
    int32_t n; uint32_t mods; memcpy(&n, st + 20, 4); memcpy(&mods, st + 28, 4);
    if (n < 0) n = 0; if (n > 16) n = 16;
    for (int i = 0; i < n; i++) { uint16_t u; memcpy(&u, st + 32 + 2 * i, 2); if (u >= 4 && u < 0xE0) out->down[u] = 1; }
    for (int b = 0; b < 8; b++) if (mods & (1u << b)) out->down[USB_MODBASE + b] = 1;
    return 1;
}
typedef struct { int connected; uint32_t buttons; int dx, dy, wheel, tilt; } UsbMouse;
// Adds count mouse records into m (motion and wheels summed, buttons of the last connected one); returns how many were connected.
static int usb_decode_mouse(const uint8_t *rec, int count, UsbMouse *m) {
    int conn = 0;
    for (int i = 0; i < count; i++) {
        const uint8_t *r = rec + 40 * i; int32_t x, y, w, t; uint32_t b;
        if (!r[8]) continue;
        memcpy(&b, r + 12, 4); memcpy(&x, r + 16, 4); memcpy(&y, r + 20, 4); memcpy(&w, r + 24, 4); memcpy(&t, r + 28, 4);
        m->buttons = b; m->dx += x; m->dy += y; m->wheel += w; m->tilt += t; conn++;
    }
    if (conn) m->connected = 1;
    return conn;
}
// Key events from the previous snapshot to the current: releases first, then presses. mod keys come out as ordinary keys. Returns the count (<= cap).
typedef struct { int usage, down; } UsbEv;
static int usb_diff(const UsbKeys *prev, const UsbKeys *cur, UsbEv *ev, int cap) {
    int n = 0;
    for (int u = 0; u < 256 && n < cap; u++) if (prev->down[u] && !cur->down[u]) { ev[n].usage = u; ev[n].down = 0; n++; }
    for (int u = 0; u < 256 && n < cap; u++) if (!prev->down[u] && cur->down[u]) { ev[n].usage = u; ev[n].down = 1; n++; }
    return n;
}
// GLFW modifier bits of a snapshot (1 shift, 2 ctrl, 4 alt, 8 super)
static int usb_glfw_mods(const UsbKeys *k) {
    return ((k->down[0xE1] || k->down[0xE5]) ? 1 : 0) | ((k->down[0xE0] || k->down[0xE4]) ? 2 : 0) | ((k->down[0xE2] || k->down[0xE6]) ? 4 : 0) | ((k->down[0xE3] || k->down[0xE7]) ? 8 : 0);
}
// The launcher's virtual pad buttons (scePad bit layout, see kit core/input.hpp) for a snapshot.
static uint32_t usb_pad_bits(const UsbKeys *k) {
    uint32_t b = 0; int shift = (k->down[0xE1] || k->down[0xE5]);
    if (k->down[82]) b |= 0x10;                              // Up
    if (k->down[79]) b |= 0x20;                              // Right
    if (k->down[81]) b |= 0x40;                              // Down
    if (k->down[80]) b |= 0x80;                              // Left
    if (k->down[40] || k->down[88] || k->down[44]) b |= 0x4000;           // Enter / keypad Enter / Space: confirm (Cross)
    if (k->down[41] || k->down[42]) b |= 0x2000;             // Esc / Backspace: back (Circle)
    if (k->down[75] || (k->down[43] && shift)) b |= 0x400;    // PgUp / Shift+Tab: previous page (L1)
    if (k->down[78] || (k->down[43] && !shift)) b |= 0x800;   // PgDn / Tab: next page (R1)
    if (k->down[20]) b |= 0x100;                             // Q: L2 (jump back)
    if (k->down[8]) b |= 0x200;                              // E: R2 (jump forward)
    if (k->down[28]) b |= 0x1000;                           // Y: Triangle (Home / End are the launcher's jump-to-first / last)
    if (k->down[76] || k->down[27]) b |= 0x8000;             // Delete / X: Square
    if (k->down[58] || k->down[101] || k->down[16]) b |= 0x8;   // F1 / Menu / M: Options
    if (k->down[59]) b |= 0x2;                               // F2: L3
    if (k->down[60]) b |= 0x4;                               // F3: R3
    return b;
}

#ifndef PS5_USB_PURE_ONLY
// ---- console part ----
// The keyboard / mouse / sysmodule imports are declared in ps5lwjgl.c (USB_IMPORT): the keyboard and mouse functions are IMPORTS of the title, resolved by the runtime linker from the SDK stub libSceKeyboard.so and
// libSceMouse.so (built by vk_relink.sh: the SDK has no mouse stub). Looking them up with sceKernelDlsym on the module handle found nothing in this sandbox (log babd4fce: plain, NID and NID#x#y names all failed).
static int (*volatile usb_sm_load)(unsigned short) = sceSysmoduleLoadModule;   // read through pointers so the null check is real in both variants
static int (*volatile usb_sm_handle)(unsigned, int *) = sceSysmoduleGetModuleHandleInternal;
typedef int (*UsbInitFn)(void);
typedef int (*UsbOpenFn)(int, int, int, const void *);
typedef int (*UsbKbReadFn)(int, void *);
typedef int (*UsbMsReadFn)(int, void *, int);
#define USB_KBS 4
static struct {
    int tried, kb_n, ms_h, uid; double next_try; int kb_h[USB_KBS];
    UsbKbReadFn kb_read; UsbMsReadFn ms_read;
    UsbKeys keys;            // newest snapshot of every keyboard together
    UsbMouse ms;             // motion / wheel accumulated since the last consume
    uint32_t ms_buttons;     // buttons of the newest record
    int kb_dumps, ms_dumps, kb_ok, ms_ok;
    int wheel_sign;          // +1: a positive wheel value is "up" (to be confirmed by the first wheel record in the log)
} g_usb = { .ms_h = -1, .uid = -1, .wheel_sign = 1 };
static double g_usb_act;     // time of the last mouse movement / click / key press: the pointer stays visible while a mouse is in use (0 = never)
static int usb_mouse_present(void) { return g_usb.ms_h >= 0; }
static int usb_hex(const char *what, const uint8_t *p, int n) { char b[240]; int l = snprintf(b, sizeof b, "usb: %s", what); for (int i = 0; i < n && l < 230; i++) l += snprintf(b + l, sizeof b - l, "%02x", p[i]); TR("%s", b); return 0; }
static int usb_module(unsigned short id, int *status) {
    int32_t h = -1; *status = usb_sm_load ? usb_sm_load(id) : -1;
    if (usb_sm_handle) {
        if (usb_sm_handle(id, &h) == 0 && h >= 0) return h;
        h = -1; if (usb_sm_handle(0x80000000u | id, &h) == 0 && h >= 0) return h;
    }
    return -1;
}
static void usb_open(double t) {
    if (t < g_usb.next_try) return;
    g_usb.next_try = t + 3.0;   // devices plugged in later are found within 3 s
    int first = !g_usb.tried; g_usb.tried = 1;
    if (!usb_sm_load || !usb_sm_handle) { if (first) TR("usb: libSceSysmodule is not linked: no USB keyboard / mouse"); return; }
    if (g_usb.uid < 0) { int u = -1; sceUserServiceGetInitialUser(&u); g_usb.uid = u; }
    int st = 0;
    if (g_usb.kb_n == 0) {
        static int loaded; if (!loaded) { loaded = 1; int m = usb_module(0x0106, &st); if (first) TR("usb: keyboard module load=0x%x handle=%d", st, m); }
        UsbInitFn init = (UsbInitFn)sceKeyboardInit; UsbOpenFn open = (UsbOpenFn)sceKeyboardOpen; void *rd = (void *)sceKeyboardReadState;
        if (first) TR("usb: keyboard functions init=%p open=%p readState=%p user=%d", (void *)init, (void *)open, rd, g_usb.uid);
        if (init && open && rd) {
            static int inited; int ir = 0; if (!inited) { inited = 1; ir = init(); }
            uint8_t prm[8] = {0}; g_usb.kb_read = (UsbKbReadFn)rd;
            for (int i = 0; i < USB_KBS; i++) { int h = open(g_usb.uid, 0, i, prm); g_usb.kb_h[i] = h; if (h >= 0) g_usb.kb_n++; }
            if (first || g_usb.kb_n) TR("usb: keyboard init=0x%x opened %d handle(s): 0x%x 0x%x 0x%x 0x%x", ir, g_usb.kb_n, g_usb.kb_h[0], g_usb.kb_h[1], g_usb.kb_h[2], g_usb.kb_h[3]);
        }
    }
    if (g_usb.ms_h < 0) {
        static int loaded; if (!loaded) { loaded = 1; int m = usb_module(0x00a9, &st); if (first) TR("usb: mouse module load=0x%x handle=%d", st, m); }
        UsbInitFn init = (UsbInitFn)sceMouseInit; UsbOpenFn open = (UsbOpenFn)sceMouseOpen; void *rd = (void *)sceMouseRead;
        if (first) TR("usb: mouse functions init=%p open=%p read=%p", (void *)init, (void *)open, rd);
        if (init && open && rd) {
            static int inited; int ir = 0; if (!inited) { inited = 1; ir = init(); }
            uint8_t prm[8] = {1}; int h = open(g_usb.uid, 0, 0, prm);
            if (h < 0) { prm[0] = 0; h = open(g_usb.uid, 0, 0, prm); }
            g_usb.ms_read = (UsbMsReadFn)rd; if (h >= 0) g_usb.ms_h = h;
            if (first || h >= 0) TR("usb: mouse init=0x%x open=0x%x", ir, h);
        }
    }
}
// ---- the reader ----
// A thread reads both devices every ~2 ms and queues what happened, so nothing is lost when the game (or the launcher) runs at a low frame rate:
// the mouse queue is drained completely (sceMouseRead returns at most the requested count; 8 per frame lost motion / wheel notches at low fps),
// motion / wheel are summed atomically, and every key / button edge goes into a ring in order (a tap shorter than a frame is still delivered).
typedef struct { uint8_t kind, down; uint16_t code; } UsbQE;   // kind 0: key (code = usage), 1: mouse button (code = bit)
#define USB_QN 512
static UsbQE g_uq[USB_QN]; static volatile unsigned g_uq_h, g_uq_t;
static volatile int g_ms_dx, g_ms_dy, g_ms_wh, g_ms_tilt, g_usb_thread, g_usb_thread_fail;
static void usb_push(int kind, int code, int down) {
    unsigned h = g_uq_h; if (h - __atomic_load_n(&g_uq_t, __ATOMIC_ACQUIRE) >= USB_QN) return;
    g_uq[h % USB_QN].kind = (uint8_t)kind; g_uq[h % USB_QN].code = (uint16_t)code; g_uq[h % USB_QN].down = (uint8_t)down;
    __atomic_store_n(&g_uq_h, h + 1, __ATOMIC_RELEASE);
}
// One pass over both devices (the reader thread's body; the main thread runs it itself when no thread could be made).
static void usb_pump(double t) {
    static UsbKeys tk; static uint32_t tb;
    usb_open(t);
    if (g_usb.kb_n > 0 && g_usb.kb_read) {
        UsbKeys now; memset(&now, 0, sizeof now); int any = 0;
        for (int i = 0; i < USB_KBS; i++) {
            if (g_usb.kb_h[i] < 0) continue;
            uint8_t st[96]; memset(st, 0, sizeof st); int rc = g_usb.kb_read(g_usb.kb_h[i], st);
            if (rc != 0) continue;
            if (g_usb.kb_dumps < 6 && st[16] && (st[20] || st[28])) { g_usb.kb_dumps++; usb_hex("keyboard state ", st, 96); }
            if (usb_decode_keyboard(st, &now)) any = 1;
        }
        if (any && !g_usb.kb_ok) { g_usb.kb_ok = 1; TR("usb: a keyboard is connected"); }
        UsbEv ev[64]; int n = usb_diff(&tk, &now, ev, 64);
        for (int i = 0; i < n; i++) usb_push(0, ev[i].usage, ev[i].down);
        tk = now;
    }
    if (g_usb.ms_h >= 0 && g_usb.ms_read) {
        for (int round = 0; round < 16; round++) {   // until the queue is empty
            uint8_t rec[40 * 32]; memset(rec, 0, sizeof rec); int got = g_usb.ms_read(g_usb.ms_h, rec, 32);
            if (got <= 0) break; if (got > 32) got = 32;
            for (int i = 0; i < got && g_usb.ms_dumps < 8; i++) { const uint8_t *r = rec + 40 * i; int32_t x, y, w; memcpy(&x, r + 16, 4); memcpy(&y, r + 20, 4); memcpy(&w, r + 24, 4); uint32_t bt; memcpy(&bt, r + 12, 4); g_usb.ms_dumps++; TR("usb: mouse record buttons=0x%x x=%d y=%d wheel=%d", bt, x, y, w); }
            for (int i = 0; i < got; i++) {
                UsbMouse m; memset(&m, 0, sizeof m); m.buttons = tb;
                if (!usb_decode_mouse(rec + 40 * i, 1, &m)) continue;
                if (m.dx) __atomic_fetch_add(&g_ms_dx, m.dx, __ATOMIC_RELAXED);
                if (m.dy) __atomic_fetch_add(&g_ms_dy, m.dy, __ATOMIC_RELAXED);
                if (m.wheel) __atomic_fetch_add(&g_ms_wh, m.wheel, __ATOMIC_RELAXED);
                if (m.tilt) __atomic_fetch_add(&g_ms_tilt, m.tilt, __ATOMIC_RELAXED);
                for (int b = 0; b < 5; b++) if (((m.buttons ^ tb) >> b) & 1) usb_push(1, b, (m.buttons >> b) & 1);
                tb = m.buttons;
                if (!g_usb.ms_ok) { g_usb.ms_ok = 1; TR("usb: a mouse is connected"); }
            }
            if (got < 32) break;
        }
    }
}
static void *usb_thread_fn(void *a) { (void)a; for (;;) { usb_pump(now_s()); usleep(2000); } return 0; }
// Main-thread side: start the reader once, then take what it queued. Key / button edges are applied to g_usb.keys / ms_buttons in order and
// returned in g_usb.ev (kind, code, down); `pulse` marks keys / buttons that went down at any time since the previous call (for the launcher's edge detection).
static struct { UsbQE ev[USB_QN]; int n; UsbKeys pulse; uint32_t pulse_b; } g_usbc;
static void usb_read(double t) {
    if (!g_usb_thread && !g_usb_thread_fail) {
        pthread_t th; int rc = pthread_create(&th, 0, usb_thread_fn, 0);
        if (rc == 0) { pthread_detach(th); g_usb_thread = 1; TR("usb: reader thread started"); } else { g_usb_thread_fail = 1; TR("usb: reader thread failed (%d): reading inline", rc); }
    }
    if (!g_usb_thread) usb_pump(t);
    g_usbc.n = 0; memset(&g_usbc.pulse, 0, sizeof g_usbc.pulse); g_usbc.pulse_b = 0;
    unsigned h = __atomic_load_n(&g_uq_h, __ATOMIC_ACQUIRE), tl = g_uq_t;
    while (tl != h) {
        UsbQE e = g_uq[tl % USB_QN]; tl++;
        if (g_usbc.n < USB_QN) g_usbc.ev[g_usbc.n++] = e;
    }
    __atomic_store_n(&g_uq_t, tl, __ATOMIC_RELEASE);
    g_usb.ms.dx += __atomic_exchange_n(&g_ms_dx, 0, __ATOMIC_RELAXED); g_usb.ms.dy += __atomic_exchange_n(&g_ms_dy, 0, __ATOMIC_RELAXED);
    g_usb.ms.wheel += __atomic_exchange_n(&g_ms_wh, 0, __ATOMIC_RELAXED); g_usb.ms.tilt += __atomic_exchange_n(&g_ms_tilt, 0, __ATOMIC_RELAXED);
}
// Applies one queued event to the current state.
static void usb_apply(const UsbQE *e) {
    if (e->kind == 0) { if (e->code < 256) { g_usb.keys.down[e->code] = e->down; if (e->down) g_usbc.pulse.down[e->code] = 1; } }
    else { if (e->down) { g_usb.ms_buttons |= 1u << e->code; g_usbc.pulse_b |= 1u << e->code; } else g_usb.ms_buttons &= ~(1u << e->code); }
}

// ---- the launcher ----
// Everything the launcher needs from a USB keyboard / mouse, once per frame: virtual pad buttons (arrows = D-pad, Enter = confirm, Esc = back, ...),
// the pointer (in the launcher's 1920x1080 space), clicks, wheel notches and typed text. Mouse clicks are NOT turned into pad buttons here: the launcher hit-tests them itself.
typedef struct {
    float x, y;                  // pointer position, 0..1920 x 0..1080
    int active;                  // a mouse has moved / clicked / scrolled
    int moved;                   // the pointer moved this frame
    int left_down, left_click, right_click, mid_click;   // *_click: pressed during this frame (taps are kept)
    int wheel;                   // notches this frame, positive = up
    char typed[24]; int typed_n; // printable characters typed this frame (US layout), key repeat included
    int backspaces, enter, esc;  // presses this frame (repeat included)
    int kbd;                     // a key went down this frame (the launcher switches its glyphs to keyboard keys)
    int hwheel;                  // sideways scroll (tilt wheel, or Shift + wheel), positive = right
    int caret_l, caret_r, home, end, del;   // text editing keys this frame (repeat included)
} Ps5UsbFrame;
static volatile int g_usb_text_mode;
void ps5_usb_set_text_mode(int on) { g_usb_text_mode = on; }   // the launcher's text entry: letters, digits, Space and Backspace are text, not buttons
// Bedrock pointer sprite for the launcher: fills rgba (w*h*4 bytes, 0 alpha = transparent), returns w << 16 | h; the hotspot is the top-left pixel.
int ps5_cursor_pixels(unsigned char *rgba, int cap) {
    int rows = (int)(sizeof cur_art / sizeof cur_art[0]), w = 9;
    if (!rgba || cap < w * rows * 4) return w << 16 | rows;
    for (int r = 0; r < rows; r++) for (int c = 0; c < w; c++) {
        char k = cur_art[r][c]; unsigned char *o = rgba + (r * w + c) * 4;
        if (k == '.' || !k) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
        float cr, cg, cb; cur_color(k, &cr, &cg, &cb); o[0] = (unsigned char)(cr * 255 + .5f); o[1] = (unsigned char)(cg * 255 + .5f); o[2] = (unsigned char)(cb * 255 + .5f); o[3] = 255;
    }
    return w << 16 | rows;
}
uint32_t ps5_usb_poll(Ps5UsbFrame *f) {
    static double px = 960, py = 540, rep_t; static int caps, active, rep_u = -1; static uint32_t last_b;
    double t = now_s(); memset(f, 0, sizeof *f);
    usb_read(t);
    for (int i = 0; i < g_usbc.n; i++) {
        const UsbQE *e = &g_usbc.ev[i]; usb_apply(e);
        if (e->kind == 1) { if (e->down) { active = 1; if (e->code == 0) f->left_click = 1; else if (e->code == 1) f->right_click = 1; else if (e->code == 2) f->mid_click = 1; } continue; }
        int u = e->code, mods = usb_glfw_mods(&g_usb.keys);
        if (!e->down) { if (u == rep_u) rep_u = -1; continue; }
        if (u == 57) caps ^= 1;
        if (usb_is_mod(u)) continue;
        f->kbd = 1;
        if (u == 42) f->backspaces++; else if (u == 40 || u == 88) f->enter = 1; else if (u == 41) f->esc = 1;
        else if (u == 80) f->caret_l++; else if (u == 79) f->caret_r++; else if (u == 74) f->home++; else if (u == 77) f->end++; else if (u == 76) f->del++;
        else if (!(mods & (2 | 4 | 8))) { unsigned c = usb_char(u, mods & 1, caps); if (c >= 32 && c < 127 && f->typed_n < 23) f->typed[f->typed_n++] = (char)c; }
        rep_u = u; rep_t = t + 0.45;
    }
    if (rep_u >= 0 && t >= rep_t) {   // key repeat: 0.45 s delay, then 30 per second
        int mods = usb_glfw_mods(&g_usb.keys); rep_t = t + 1.0 / 30.0;
        if (rep_u == 42) f->backspaces++;
        else if (rep_u == 80) f->caret_l++; else if (rep_u == 79) f->caret_r++; else if (rep_u == 74) f->home++; else if (rep_u == 77) f->end++; else if (rep_u == 76) f->del++;
        else if (rep_u != 40 && rep_u != 88 && rep_u != 41 && !(mods & (2 | 4 | 8))) { unsigned c = usb_char(rep_u, mods & 1, caps); if (c >= 32 && c < 127 && f->typed_n < 23) f->typed[f->typed_n++] = (char)c; }
    }
    if (g_usb.ms.dx || g_usb.ms.dy) {
        px += g_usb.ms.dx; py += g_usb.ms.dy; f->moved = 1; active = 1;
        if (px < 0) px = 0; if (px > 1919) px = 1919; if (py < 0) py = 0; if (py > 1079) py = 1079;
    }
    f->wheel = g_usb.ms.wheel * g_usb.wheel_sign; f->hwheel = g_usb.ms.tilt;
    if (f->wheel && (g_usb.keys.down[0xE1] || g_usb.keys.down[0xE5])) { f->hwheel += f->wheel; f->wheel = 0; }   // Shift + wheel scrolls sideways
    if (f->wheel || f->hwheel) active = 1;
    g_usb.ms.dx = g_usb.ms.dy = g_usb.ms.wheel = g_usb.ms.tilt = 0;
    f->x = (float)px; f->y = (float)py; f->active = active; f->left_down = (g_usb.ms_buttons & 1) != 0;
    UsbKeys kk = g_usb.keys; for (int i = 0; i < 256; i++) kk.down[i] |= g_usbc.pulse.down[i];   // a tap shorter than a frame still counts
    if (g_usb_text_mode) { for (int i = 4; i <= 39; i++) kk.down[i] = 0; kk.down[44] = kk.down[42] = kk.down[43] = kk.down[74] = kk.down[76] = kk.down[40] = kk.down[88] = 0; kk.down[79] = kk.down[80] = kk.down[77] = 0; }
    uint32_t b = usb_pad_bits(&kk), mb = g_usb.ms_buttons | g_usbc.pulse_b;
    if (mb & 8) b |= 0x2000;                                  // mouse back button: back (Circle), like a desktop
    if (mb & 16) b |= 0x800;                                  // forward button: next page
    if (b || mb != last_b || f->moved) g_usb_act = t;
    last_b = mb;
    return b;
}

// ---- the game: callbacks ----
static int g_usb_ctrl_alt_q;
static void usb_game(double t, double dt, int gui) {
    (void)dt;
    usb_read(t);
    static int caps; static int rep_u = -1; static double rep_t;
    int mods = usb_glfw_mods(&g_usb.keys);
    for (int i = 0; i < g_usbc.n; i++) {   // every key / button edge in the order it happened, however long the frame was
        const UsbQE *e = &g_usbc.ev[i]; usb_apply(e);
        if (e->kind == 1) { g_usb_act = t; usb_mouse_set(e->code, e->down); if (gui) { g_cur_vis = 1; g_cur_t = t; } continue; }
        int u = e->code, k = usb_glfw_key(u); if (k < 0 || k > KEY_MAX) continue;
        mods = usb_glfw_mods(&g_usb.keys); g_usb_mods = (unsigned)mods;
        if (e->down) {
            g_usb_act = t;
            if (u == 57) caps ^= 1;
            if (g_osk_open && !usb_is_mod(u)) { g_osk_open = 0; g_osk_l2 = 0; g_osk_lock = 0; }   // typing on a real keyboard puts the on-screen one away
            usb_key_set(k, 1);
            if (!usb_is_mod(u)) {
                if (!(mods & (2 | 4 | 8))) { unsigned c = usb_char(u, mods & 1, caps); if (c) osk_char(c); }
                rep_u = u; rep_t = t + 0.45;
            }
        } else {
            usb_key_set(k, 0);
            if (u == rep_u) rep_u = -1;
        }
    }
    g_usb_mods = (unsigned)mods;
    if (rep_u >= 0 && t >= rep_t) {   // key repeat: 0.45 s delay, then 30 per second
        int k = usb_glfw_key(rep_u); rep_t = t + 1.0 / 30.0;
        if (k >= 0 && cb_key) cb_key(WIN, k, k, 2 /* GLFW_REPEAT */, mods);
        if (!(mods & (2 | 4 | 8))) { unsigned c = usb_char(rep_u, mods & 1, caps); if (c) osk_char(c); }
    }
    // Ctrl + Alt + Backspace: the same emergency exit as the pad's combo (ask the game to close)
    if (g_usb.keys.down[0xE0] && g_usb.keys.down[0xE2] && g_usb.keys.down[42]) { if (!g_usb_ctrl_alt_q) { g_usb_ctrl_alt_q = 1; TR("usb: Ctrl+Alt+Backspace: asking the game to close"); g_should_close = 1; if (cb_close) ((void (*)(void *))cb_close)(WIN); } }
    // mouse
    if (g_usb.ms.dx || g_usb.ms.dy) {
        double sc = gui ? g_w / 1920.0 : 1.0, mx = g_usb.ms.dx * sc, my = g_usb.ms.dy * sc;
        g_usb_act = t;
        if (gui) { g_cx += mx; g_cy += my; if (g_cx < 0) g_cx = 0; if (g_cx > g_w) g_cx = g_w; if (g_cy < 0) g_cy = 0; if (g_cy > g_h) g_cy = g_h; g_cur_vis = 1; g_cur_t = t; }
        else { g_rel_dx += mx; g_rel_dy += my; g_cx += mx; g_cy += my; }
    }
    if ((g_usb.ms.wheel || g_usb.ms.tilt) && cb_scroll) {
        static int wl; if (wl < 4 && g_usb.ms.wheel) { wl++; TR("usb: wheel %d (a positive value is taken as 'up')", g_usb.ms.wheel); }
        int w = g_usb.ms.wheel * g_usb.wheel_sign; int step = w > 0 ? 1 : -1;
        for (int i = 0; i < (w < 0 ? -w : w) && i < 10; i++) cb_scroll(WIN, 0.0, (double)step);
        if (g_usb.ms.tilt) cb_scroll(WIN, g_usb.ms.tilt > 0 ? 1.0 : -1.0, 0.0);
        g_usb_act = t;
    }
    g_usb.ms.dx = g_usb.ms.dy = g_usb.ms.wheel = g_usb.ms.tilt = 0;
    if (!(g_usb.keys.down[0xE0] && g_usb.keys.down[0xE2] && g_usb.keys.down[42])) g_usb_ctrl_alt_q = 0;
}
#endif
