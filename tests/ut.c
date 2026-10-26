#define PS5_UNCAP 1
#define PS5_GPUTIME 1
#include "../ps5lwjgl.c"
#include <unistd.h>
void ps5_trace(const char *m) { write(1, m, strlen(m)); write(1, "\n", 1); }
int scePadReadState(int h, PadState *s) { return -1; } int scePadInit(void) { return 0; } int scePadOpen(int a, int b, int c, const void *d) { return 1; }
int sceUserServiceInitialize(void *p) { return 0; } int sceUserServiceGetInitialUser(int *u) { *u = 1; return 0; }
size_t ps5_opengl_heap_live_bytes(void) { return 0; } void ps5_opengl_heap_stats_print(unsigned x) {}
static int last_mode, flip_result, n_flip, n_wait;
int __real_sceVideoOutSubmitFlip(int h, int idx, unsigned mode, long arg) { last_mode = (int)mode; n_flip++; return (mode == 2) ? flip_result : 0; }
int __real_sceVideoOutWaitVblank(int h) { n_wait++; return 0; }
/* ---- fake timestamp queries: every glQueryCounter returns a time 1 ms after the previous one ---- */
static unsigned long long qtime[10000]; static unsigned long long clk; static int next_q = 1;
static void f_GenQ(int n, unsigned *o) { for (int i = 0; i < n; i++) o[i] = (unsigned)next_q++; }
static void f_QC(unsigned id, int t) { clk += 1000000ULL; qtime[id] = clk; }
static void f_GetQi(unsigned id, int p, int *v) { *v = 1; }
static void f_GetQ64(unsigned id, int p, unsigned long long *v) { *v = qtime[id]; }
void *eglGetProcAddress(const char *n) { if (!strcmp(n, "glGenQueries")) return (void *)f_GenQ; if (!strcmp(n, "glQueryCounter")) return (void *)f_QC; if (!strcmp(n, "glGetQueryObjectiv")) return (void *)f_GetQi; if (!strcmp(n, "glGetQueryObjectui64v")) return (void *)f_GetQ64; return 0; }
static int fails;
#define CHECK(c, ...) do { if (!(c)) { dprintf(1, "FAIL line %d: %s  ", __LINE__, #c); dprintf(1, __VA_ARGS__); dprintf(1, "\n"); fails++; } } while (0)
int main(void) {
    /* ---- VSync mapping ---- */
    n_flip = n_wait = 0; __wrap_sceVideoOutSubmitFlip(1, 0, 1, 0); __wrap_sceVideoOutWaitVblank(1);
    CHECK(last_mode == 1 && n_wait == 1, "VSync ON must keep the vsync flip and the vblank wait (mode %d, waits %d)", last_mode, n_wait);
    glfwSwapInterval(0); CHECK(g_vsync == 0, "swap interval 0 must switch VSync off");
    flip_result = 0; n_wait = 0; __wrap_sceVideoOutSubmitFlip(1, 1, 1, 0); __wrap_sceVideoOutWaitVblank(1);
    CHECK(last_mode == 2 && n_wait == 0, "VSync OFF must flip in the tearing mode and skip the vblank wait (mode %d, waits %d)", last_mode, n_wait);
    glfwSwapInterval(1); n_wait = 0; __wrap_sceVideoOutSubmitFlip(1, 0, 1, 0); __wrap_sceVideoOutWaitVblank(1);
    CHECK(last_mode == 1 && n_wait == 1, "switching VSync back ON must restore vsynced flips live (mode %d, waits %d)", last_mode, n_wait);
    flip_result = (int)0x80290001; glfwSwapInterval(0); n_flip = 0; int r = __wrap_sceVideoOutSubmitFlip(1, 1, 1, 0);
    CHECK(r == 0 && last_mode == 1 && g_uncap_fail == 1 && n_flip == 2, "a refused tearing mode must fall back to a normal flip (r %d mode %d fail %d flips %d)", r, last_mode, g_uncap_fail, n_flip);
    n_wait = 0; __wrap_sceVideoOutWaitVblank(1); CHECK(n_wait == 1, "after a refusal the vblank wait must be back");
    /* ---- GPU timestamps: 130 frames; frame 60 and 120 are sampled; marks: frame start, 2 merged draws (begin/end), frame end -> 6 marks, 1 ms apart ---- */
    for (int f = 0; f < 130; f++) { gt_frame_begin(); if (gt_cur >= 0) { gt_mark(10); gt_mark(11); gt_mark(12); gt_mark(13); } gt_frame_end(); }
    CHECK(gt_acc_n >= 1, "no GPU timestamp sample was harvested");
    if (gt_acc_n) { CHECK(gt_acc_draws / gt_acc_n == 2, "two merged draws per sampled frame expected, got %d", gt_acc_draws / gt_acc_n);
        CHECK(gt_acc_draw / gt_acc_n > 1.99 && gt_acc_draw / gt_acc_n < 2.01, "merged draw time 2 x 1 ms expected, got %.3f", gt_acc_draw / gt_acc_n);
        CHECK(gt_acc_frame / gt_acc_n > 4.99 && gt_acc_frame / gt_acc_n < 5.01, "frame timeline 5 ms expected (6 marks), got %.3f", gt_acc_frame / gt_acc_n); }
    dprintf(1, "uncap/gputime harness: fails=%d\n", fails); return fails;
}
