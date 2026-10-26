#include "../ps5lwjgl.c"
#include <unistd.h>
void ps5_trace(const char *m) { write(1, m, strlen(m)); write(1, "\n", 1); }
static PadState g_script;
int scePadReadState(int h, PadState *s) { *s = g_script; s->connected = 1; return 0; }
int scePadInit(void) { return 0; } int scePadOpen(int a, int b, int c, const void *d) { return 1; }
int sceUserServiceInitialize(void *p) { return 0; } int sceUserServiceGetInitialUser(int *u) { *u = 1; return 0; }
size_t ps5_opengl_heap_live_bytes(void) { return 0; } void ps5_opengl_heap_stats_print(unsigned x) {}
static char out[4096]; static int on;
static void clog(const char *f, ...) { va_list a; va_start(a, f); on += vsnprintf(out + on, sizeof out - on, f, a); va_end(a); }
static void kcb(void *w, int k, int sc, int act, int mods) { clog("[key %d %s]", k, act ? "dn" : "up"); }
static void chm(void *w, unsigned c, int m) { clog("%c", (char)c); }
static int g_tn, g_tx, g_ty, g_tid = 1;
static int g_tl2;
static void poll(unsigned b, int lx, int ly, int r2);
static void pl(unsigned b) { g_tl2 = 1; poll(b, 128, 128, 0); g_tl2 = 0; }
static void poll(unsigned b, int lx, int ly, int r2) { memset(&g_script, 0, sizeof g_script); g_script.mid[40] = g_tn; g_script.mid[48] = g_tx & 255; g_script.mid[49] = g_tx >> 8; g_script.mid[50] = g_ty & 255; g_script.mid[51] = g_ty >> 8; g_script.mid[52] = g_tid; g_script.buttons = b; g_script.lx = lx; g_script.ly = ly; g_script.rx = g_script.ry = 128; g_script.r2 = r2 ? 255 : 0; g_script.l2 = g_tl2 ? 255 : 0; usleep(15000); glfwPollEvents(); }
static void idle(void) { poll(0, 128, 128, 0); }
static void tap(unsigned b) { poll(b, 128, 128, 0); idle(); }
static int fails;
#define EXPECT(s) do { if (strcmp(out, s)) { dprintf(1, "FAIL line %d: got \"%s\" expected \"%s\"\n", __LINE__, out, s); fails++; } on = 0; out[0] = 0; } while (0)
int main(void) {
    g_pad = 1; g_w = 1920; g_h = 1080;
    glfwSetKeyCallback(0, (void *)kcb); glfwSetCharModsCallback(0, (void *)chm);
    g_cursor_mode = 0;                                  // menu
    idle(); idle(); on = 0; out[0] = 0;
    tap(0x4);                                           // R3 opens the keyboard
    dprintf(1, "osk open=%d focus row=%d key=%c\n", g_osk_open, g_osk_row, OSKR(g_osk_row)[g_osk_k].c);
    tap(0x4000); EXPECT("q");                           // Cross types the focused key (q)
    tap(0x20); tap(0x4000); EXPECT("w");                // right, type
    pl(0); pl(0x4000); pl(0); idle(); EXPECT("W");       // hold L2 + Cross = shifted W (L2 is shift, like the PS5 keyboard)
    tap(0x4000); EXPECT("w");                           // released: lower case again
    { pl(0); idle(); pl(0); idle(); tap(0x4000); EXPECT("W"); tap(0x4000); EXPECT("W"); pl(0); idle(); pl(0); idle(); tap(0x4000); EXPECT("w"); }   // L2 twice quickly = caps lock on, again = off
    poll(0, 128, 128, 1); idle();
    if (!g_osk_page) { dprintf(1, "FAIL R2 did not switch to the symbols page\n"); fails++; }
    { g_osk_row = 3; g_osk_k = 0; tap(0x4000); EXPECT("\xA7"); }                      // symbols page, row 4 key 1 = the section sign
    { g_osk_row = 1; g_osk_k = 0; tap(0x4000); EXPECT("-"); g_osk_row = 2; g_osk_k = 0; tap(0x4000); EXPECT("/"); }   // punctuation Minecraft commands need
    poll(0, 128, 128, 1); idle(); if (g_osk_page) { dprintf(1, "FAIL R2 did not come back to the letters\n"); fails++; }
    g_osk_row = 1; g_osk_k = 1; on = 0; out[0] = 0;
    { g_osk_ox = 40; g_osk_oy = -30; tap(0x4); if (g_osk_ox != 0 || g_osk_oy != 0 || !g_osk_open) { dprintf(1, "FAIL R3 did not reset the panel (ox %.0f oy %.0f open %d)\n", g_osk_ox, g_osk_oy, g_osk_open); fails++; } }   // R3 = reset, the keyboard stays
    { tap(0x2); if (!g_osk_top) { dprintf(1, "FAIL L3 did not dock the panel at the top\n"); fails++; } tap(0x2); if (g_osk_top) { dprintf(1, "FAIL L3 did not dock it at the bottom again\n"); fails++; } }
    for (int i = 0; i < OSK_HINTS; i++) if (ov_text_w(osk_hint[i], 2) > 10 * OSK_KW + 11 * OSK_GAP + 24 - 28) { dprintf(1, "FAIL keyboard hint line %d is wider than the panel (%.0f px)\n", i, ov_text_w(osk_hint[i], 2)); fails++; }
    for (int pg = 0; pg < 2; pg++) for (int r = 0; r < 5; r++) { float w = 0; for (int k = 0; osk_rows[pg][r][k].c || osk_rows[pg][r][k].sp; k++) w += osk_rows[pg][r][k].w; if (w != 10) { dprintf(1, "FAIL page %d row %d is %.0f units wide, not 10\n", pg, r, w); fails++; } }
    on = 0; out[0] = 0;
    tap(0x1000); EXPECT(" ");                            // Triangle: space
    tap(0x8000); EXPECT("[key 259 dn][key 259 up]");     // Square backspace
    tap(0x8); EXPECT("[key 257 dn][key 257 up]");        // Options enter
    if (g_osk_open) { dprintf(1, "FAIL Options (enter) did not close the keyboard\n"); fails++; }
    tap(0x4); idle();                                     // open again
    tap(0x40); tap(0x40); tap(0x40);                     // down x3 -> row 4
    dprintf(1, "after 3 downs: row=%d key=%s\n", g_osk_row, OSKR(g_osk_row)[g_osk_k].lab ? OSKR(g_osk_row)[g_osk_k].lab : "(char)");
    tap(0x4000); on = 0; out[0] = 0;                    // press whatever is there (shift/key)
    g_osk_shift = 0;
    // numbers row + symbol via shift
    tap(0x10); tap(0x10); tap(0x10); tap(0x10);          // up to row 0
    dprintf(1, "row after ups: %d\n", g_osk_row);
    tap(0x1000); tap(0x4000);                            // shift + '?' key at current column -> symbol
    dprintf(1, "typed: \"%s\"\n", out); on = 0; out[0] = 0;
    tap(0x2000); dprintf(1, "osk open after circle=%d\n", g_osk_open);
    // gameplay: legacy console layout
    g_cursor_mode = GLFW_CURSOR_DISABLED; idle(); on = 0; out[0] = 0;
    tap(0x4); EXPECT("[key 340 dn][key 340 up]");                                                         // R3 = sneak (shift), no keyboard in gameplay
    tap(0x2); EXPECT("[key 294 dn][key 294 up]");                                                         // L3 = camera view (F5)
    tap(0x2000); EXPECT("[key 81 dn][key 81 up]");                                                        // Circle = drop (Q)
    tap(0x1000); EXPECT("[key 69 dn][key 69 up]");                                                        // Triangle = inventory (E)
    tap(0x8000); EXPECT("[key 70 dn][key 70 up]");                                                        // Square = swap hands (F)
    tap(0x4000); EXPECT("[key 32 dn][key 32 up]");                                                        // Cross = jump
    tap(0x20); EXPECT("[key 84 dn][key 84 up]");                                                          // D-pad right = chat (T)
    tap(0x40); EXPECT("");                                                                                // D-pad down: nothing
    poll(0, 128, 0, 0); poll(0, 128, 0, 0); idle(); EXPECT("[key 87 dn][key 87 up]");   // stick fully forward = W only, no auto-sprint
    poll(0x10, 128, 128, 0); if (!g_mouse[2]) { dprintf(1, "FAIL D-pad up did not press the middle button\n"); fails++; } idle(); if (g_mouse[2]) { dprintf(1, "FAIL middle button stuck\n"); fails++; }
    on = 0; out[0] = 0;
    g_tn = 1; g_tx = 300; g_ty = 400;                                                                      // touchpad click anywhere = player list (Tab)
    poll(0x100000, 128, 128, 0); poll(0x100000, 128, 128, 0); idle(); EXPECT("[key 258 dn][key 258 up]");
    g_tn = 0; idle(); on = 0; out[0] = 0;
    poll(0x80, 128, 128, 0); poll(0x80, 128, 128, 0); idle(); EXPECT("[key 292 dn][key 292 up]");            // D-pad left = debug (F3 tap)
    poll(0x80, 128, 128, 0); poll(0x80 | 0x4000, 128, 128, 0); poll(0x80, 128, 128, 0); idle();
    EXPECT("[key 292 dn][key 66 dn][key 66 up][key 292 up]");                                             // F3 + B (held debug layer + Cross)
    // menus: touchpad moves the cursor with acceleration, a light tap is the left button, the click opens the keyboard
    g_cursor_mode = 0; idle(); idle(); on = 0; out[0] = 0;
    { double x0 = g_cx, y0 = g_cy; g_tn = 1; g_tx = 500; g_ty = 400; poll(0, 128, 128, 0); g_tx = 600; g_ty = 450; poll(0, 128, 128, 0);
      dprintf(1, "touchpad swipe moved the cursor by %.0f,%.0f (expect > 0)\n", g_cx - x0, g_cy - y0); if (!(g_cx > x0 && g_cy > y0)) { dprintf(1, "FAIL touchpad did not move the cursor\n"); fails++; } }
    g_tn = 0; idle(); idle();
    g_tn = 1; g_tx = 900; g_ty = 400; poll(0, 128, 128, 0); g_tn = 0; poll(0, 128, 128, 0);               // finger down and up without moving = tap
    if (!g_mouse[0]) { dprintf(1, "FAIL a touchpad tap is not a left click\n"); fails++; } idle(); idle(); idle(); if (g_mouse[0]) { dprintf(1, "FAIL tap click stuck\n"); fails++; }
    g_tn = 1; poll(0x100000, 128, 128, 0); if (!g_osk_open) { dprintf(1, "FAIL touchpad click did not open the keyboard\n"); fails++; } g_tn = 0; idle(); tap(0x2000); idle();
    // triggers: a light pull (even with the pad's digital bit set) does nothing, a real pull acts, with hysteresis
    g_cursor_mode = GLFW_CURSOR_DISABLED; idle(); idle(); on = 0; out[0] = 0;
    { memset(&g_script, 0, sizeof g_script); g_script.lx = g_script.ly = g_script.rx = g_script.ry = 128; g_script.buttons = 0x200; g_script.r2 = 60; usleep(15000); glfwPollEvents(); usleep(15000); glfwPollEvents();
      if (g_mouse[0]) { dprintf(1, "FAIL light R2 pull attacked\n"); fails++; }
      g_script.buttons = 0; g_script.r2 = 200; usleep(15000); glfwPollEvents(); if (!g_mouse[0]) { dprintf(1, "FAIL firm R2 pull did not attack\n"); fails++; }
      g_script.r2 = 120; usleep(15000); glfwPollEvents(); if (!g_mouse[0]) { dprintf(1, "FAIL hysteresis: released too early\n"); fails++; }
      g_script.r2 = 40; usleep(15000); glfwPollEvents(); usleep(15000); glfwPollEvents(); if (g_mouse[0]) { dprintf(1, "FAIL R2 release\n"); fails++; } }
    // keyboard-used buttons must not reach the game: Options (enter) closing the chat must not pause, Circle closing the keyboard must not press Esc
    g_cursor_mode = 0; idle(); idle(); on = 0; out[0] = 0;
    tap(0x4); poll(0x8, 128, 128, 0); g_cursor_mode = GLFW_CURSOR_DISABLED; poll(0x8, 128, 128, 0); poll(0x8, 128, 128, 0); idle(); if (strstr(out, "[key 256")) { dprintf(1, "FAIL Options (enter) also paused the game: %s\n", out); fails++; }
    g_cursor_mode = 0; idle(); on = 0; out[0] = 0; tap(0x4); poll(0x2000, 128, 128, 0); poll(0x2000, 128, 128, 0); idle(); if (strstr(out, "[key 256")) { dprintf(1, "FAIL Circle closing the keyboard also pressed Esc: %s\n", out); fails++; }
    if (g_osk_open) { dprintf(1, "FAIL keyboard still open\n"); fails++; }
    tap(0x2000); if (!strstr(out, "[key 256 dn]")) { dprintf(1, "FAIL plain Circle in a menu no longer closes it: %s\n", out); fails++; }
    on = 0; out[0] = 0;
    // Circle closing the inventory (a menu) must not also drop the item once the game is back in gameplay
    g_cursor_mode = 0; idle(); idle(); on = 0; out[0] = 0;
    poll(0x2000, 128, 128, 0); g_cursor_mode = GLFW_CURSOR_DISABLED; poll(0x2000, 128, 128, 0); poll(0x2000, 128, 128, 0); idle();
    if (strstr(out, "[key 81")) { dprintf(1, "FAIL Circle closing a menu also dropped the item: %s\n", out); fails++; }
    on = 0; out[0] = 0; tap(0x2000); EXPECT("[key 81 dn][key 81 up]");        // a fresh Circle in gameplay still drops
    // padEmu=off: nothing is emulated
    g_pad_emu = 0; g_cursor_mode = GLFW_CURSOR_DISABLED; on = 0; out[0] = 0; tap(0x4000); tap(0x2000); EXPECT(""); g_pad_emu = 1;
    // joystick API (for mods with native controller support)
    { PadState q; memset(&q, 0, sizeof q); g_script = q; g_script.lx = 255; g_script.ly = 0; g_script.rx = g_script.ry = 128; g_script.buttons = 0x4000 | 0x10; unsigned char gp[64]; float *ax = (float *)(gp + 16);
      int ok = glfwJoystickPresent(0) && glfwGetGamepadState(0, gp);
      if (!ok || !gp[0] || !gp[11] || gp[1] || ax[0] < 0.99f || ax[1] > -0.99f) { dprintf(1, "FAIL gamepad state: ok=%d A=%d up=%d lx=%.2f ly=%.2f\n", ok, gp[0], gp[11], ax[0], ax[1]); fails++; }
      int nb = 0, na = 0, nh = 0; const unsigned char *bt = glfwGetJoystickButtons(0, &nb); const float *a2 = glfwGetJoystickAxes(0, &na); const unsigned char *h = glfwGetJoystickHats(0, &nh);
      if (nb != 14 || na != 6 || nh != 1 || !bt[0] || h[0] != 1 || a2[0] < 0.99f) { dprintf(1, "FAIL joystick arrays nb=%d na=%d nh=%d\n", nb, na, nh); fails++; }
      g_script.connected = 0; }
    dprintf(1, "fails=%d\n", fails); return fails;
}
