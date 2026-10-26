// Sandbox test of the pure part of ps5usbin.inc: HID usage -> GLFW key, characters, record decoding, diffing, launcher pad bits.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#define PS5_USB_PURE_ONLY 1
#include "../ps5usbin.inc"
static int fails;
#define CHK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static void put32(uint8_t *p, int32_t v) { memcpy(p, &v, 4); }
int main(void) {
    // letters / digits / function keys / arrows
    CHK(usb_glfw_key(4) == 'A'); CHK(usb_glfw_key(29) == 'Z'); CHK(usb_glfw_key(30) == '1'); CHK(usb_glfw_key(38) == '9'); CHK(usb_glfw_key(39) == '0');
    CHK(usb_glfw_key(58) == 290); CHK(usb_glfw_key(69) == 301); CHK(usb_glfw_key(79) == 262); CHK(usb_glfw_key(80) == 263); CHK(usb_glfw_key(81) == 264); CHK(usb_glfw_key(82) == 265);
    CHK(usb_glfw_key(40) == 257); CHK(usb_glfw_key(41) == 256); CHK(usb_glfw_key(42) == 259); CHK(usb_glfw_key(43) == 258); CHK(usb_glfw_key(44) == 32);
    CHK(usb_glfw_key(0xE0) == 341); CHK(usb_glfw_key(0xE1) == 340); CHK(usb_glfw_key(0xE5) == 344); CHK(usb_glfw_key(0xE2) == 342);
    CHK(usb_glfw_key(89) == 321); CHK(usb_glfw_key(98) == 320); CHK(usb_glfw_key(88) == 335); CHK(usb_glfw_key(1) == -1); CHK(usb_glfw_key(200) == -1);
    // every usage maps to a key inside the shim's table (KEY_MAX 349)
    for (int u = 0; u < 256; u++) { int k = usb_glfw_key(u); CHK(k == -1 || (k >= 0 && k <= 349)); }
    // characters
    CHK(usb_char(4, 0, 0) == 'a'); CHK(usb_char(4, 1, 0) == 'A'); CHK(usb_char(4, 0, 1) == 'A'); CHK(usb_char(4, 1, 1) == 'a');
    CHK(usb_char(30, 0, 0) == '1'); CHK(usb_char(30, 1, 0) == '!'); CHK(usb_char(39, 0, 0) == '0'); CHK(usb_char(39, 1, 0) == ')'); CHK(usb_char(31, 1, 0) == '@');
    CHK(usb_char(44, 0, 0) == ' '); CHK(usb_char(45, 0, 0) == '-'); CHK(usb_char(45, 1, 0) == '_'); CHK(usb_char(56, 0, 0) == '/'); CHK(usb_char(56, 1, 0) == '?'); CHK(usb_char(52, 1, 0) == '"'); CHK(usb_char(51, 1, 0) == ':');
    CHK(usb_char(53, 0, 0) == '`'); CHK(usb_char(53, 1, 0) == '~'); CHK(usb_char(40, 0, 0) == 0); CHK(usb_char(42, 0, 0) == 0); CHK(usb_char(79, 0, 0) == 0); CHK(usb_char(89, 0, 0) == '1');
    CHK(usb_char(4, 0, 1) == 'A' && usb_char(30, 0, 1) == '1');   // caps lock does not shift digits
    // keyboard decode
    uint8_t st[96]; memset(st, 0, sizeof st); UsbKeys k; memset(&k, 0, sizeof k);
    CHK(usb_decode_keyboard(st, &k) == 0);                                  // not connected
    st[16] = 1; put32(st + 20, 3); put32(st + 28, 0x02);                    // 3 keys, left shift
    uint16_t us[3] = { 4, 26, 44 }; memcpy(st + 32, us, 6);
    CHK(usb_decode_keyboard(st, &k) == 1); CHK(k.down[4] && k.down[26] && k.down[44] && k.down[0xE1] && !k.down[0xE0]);
    CHK(usb_glfw_mods(&k) == 1);
    put32(st + 20, 99); CHK(usb_decode_keyboard(st, &k) == 1);              // an absurd count is clamped, not read past the record
    put32(st + 20, -5); CHK(usb_decode_keyboard(st, &k) == 1);
    // diff: releases first, then presses
    UsbKeys a, b; memset(&a, 0, sizeof a); memset(&b, 0, sizeof b); a.down[4] = 1; a.down[5] = 1; b.down[5] = 1; b.down[6] = 1; UsbEv ev[8];
    int n = usb_diff(&a, &b, ev, 8); CHK(n == 2); CHK(ev[0].usage == 4 && ev[0].down == 0); CHK(ev[1].usage == 6 && ev[1].down == 1);
    CHK(usb_diff(&a, &a, ev, 8) == 0);
    n = usb_diff(&a, &b, ev, 1); CHK(n == 1);                              // capacity honoured
    // mouse decode
    uint8_t rec[80]; memset(rec, 0, sizeof rec); UsbMouse m; memset(&m, 0, sizeof m);
    rec[8] = 1; put32(rec + 12, 1); put32(rec + 16, 5); put32(rec + 20, -3); put32(rec + 24, 1);
    rec[40 + 8] = 1; put32(rec + 40 + 12, 3); put32(rec + 40 + 16, 2); put32(rec + 40 + 20, 4); put32(rec + 40 + 24, -1);
    CHK(usb_decode_mouse(rec, 2, &m) == 2); CHK(m.dx == 7 && m.dy == 1 && m.wheel == 0 && m.buttons == 3 && m.connected);
    memset(&m, 0, sizeof m); rec[8] = 0; rec[48] = 0; CHK(usb_decode_mouse(rec, 2, &m) == 0 && !m.connected);
    // launcher pad bits
    UsbKeys p; memset(&p, 0, sizeof p); CHK(usb_pad_bits(&p) == 0);
    p.down[82] = 1; CHK(usb_pad_bits(&p) == 0x10); p.down[82] = 0; p.down[40] = 1; CHK(usb_pad_bits(&p) == 0x4000); p.down[40] = 0; p.down[41] = 1; CHK(usb_pad_bits(&p) == 0x2000);
    p.down[41] = 0; p.down[43] = 1; CHK(usb_pad_bits(&p) == 0x800); p.down[0xE1] = 1; CHK(usb_pad_bits(&p) == 0x400);
    printf("fails=%d\n", fails); return fails != 0;
}
