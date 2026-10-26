// ps5lwjgl.c - what LWJGL needs on the PS5, in one file:
//  1. dlopen/dlsym/dlerror/dlclose replacements (the console has no dynamic loading): "glfw" -> the GLFW below, "GL" -> EGL's lookup
//  2. JNI_OnLoad_<lib> for the statically linked LWJGL natives
//  3. a GLFW subset on top of EGL (OpenGL 4.6 Core) and the DualSense pad (stick/buttons -> keyboard/mouse events)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#ifdef PS5_VK   // Vulkan build (make_mcvk_title.sh): no GL context is ever made; the window is the display, Vulkan comes from the statically linked RADV (ps5vk.inc). The GL code below is compiled but never runs.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#endif
#include <errno.h>
#include <jni.h>
#include <math.h>
#include <pthread.h>
static int g_sched_prio = 767;   // DEFAULT ON since log 20c0bb68 (threads were FIFO 700: equal-priority FIFO threads never time-slice, so a spinner starves a lock holder; MihawkOS measured exactly this stall class). launcher.cfg sched=0 turns it off, sched=256..767 sets the level (experiment from MihawkOS docs/PLAN.md "Kernel mprotect stall": threads level with the console's own at RR 700 starved a kernel mprotect; RR 767 = lowest ran clean). 0 = leave as the kernel made them
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <semaphore.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

extern void ps5_trace(const char *);
extern void kit_boot_stop(void) __attribute__((weak));   // kit_stage.cpp (the launcher's loading screen); absent in builds without the native launcher
static void TR(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void TR(const char *fmt, ...) { char b[200]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a); ps5_trace(b); }
int sceUserServiceInitialize(void *);
int sceUserServiceGetInitialUser(int *);
int scePadInit(void);
int scePadOpen(int, int, int, const void *);
typedef struct { uint32_t buttons; uint8_t lx, ly, rx, ry, l2, r2, pad0[2]; uint8_t mid[64]; int32_t connected; uint8_t tail[40]; } PadState;   // 120 bytes
int scePadReadState(int, PadState *);
int sceUserServiceGetLoginUserIdList(int *) __attribute__((weak));   // int[4]: the signed-in users (every extra controller belongs to one of them)
typedef struct { int type, user; } UserEv;
int sceUserServiceGetEvent(UserEv *) __attribute__((weak));          // login / logout events (type 0 login, 1 logout); drained and logged by pad_scan
extern size_t ps5_opengl_heap_live_bytes(void);
extern void ps5_opengl_heap_stats_print(unsigned);
static int g_hz = 60;          // refresh rate wanted by launcher.cfg 'hz' (60|120), set by ps5_set_hz() before the VM starts
static int g_hz_real;          // refresh rate the picked/queried display mode actually has (0 = unknown)
void ps5_set_hz(int hz);
#ifdef PS5_ZINK
// renderer=zink: the same EGL calls go to Mesa's EGL (zink.o, renamed zink_eglXxx) instead of the GL SDK. Every eglXxx below is redirected at run time by g_zk (set by ps5_set_renderer(2)).
static int g_zk;
extern void *zink_eglGetPlatformDisplay(unsigned, void *, const long *);
extern void *zink_eglGetProcAddress(const char *);
extern unsigned zink_eglInitialize(void *, int *, int *);
extern unsigned zink_eglBindAPI(unsigned);
extern unsigned zink_eglChooseConfig(void *, const int *, void **, int, int *);
extern void *zink_eglCreateContext(void *, void *, void *, const int *);
extern unsigned zink_eglMakeCurrent(void *, void *, void *, void *);
extern void *zink_eglCreateWindowSurface(void *, void *, void *, const int *);
extern unsigned zink_eglSwapBuffers(void *, void *);
extern unsigned zink_eglSwapInterval(void *, int);
extern unsigned zink_eglQuerySurface(void *, void *, int, int *);
extern void *zink_eglGetCurrentContext(void);
extern int zink_eglGetError(void);
static void kmem_report(void);   // defined further down
static int zk_nf, zk_nd;   // files / directories counted by the last zk_cache_bytes()
static long zk_cache_bytes(const char *dir, int depth);   // total size of the Mesa shader cache tree (defined after the shim's opendir/readdir)
static int g_zk_report;   // set by the 2 s perf line: log the GPU-backlog figures now
static int g_zk_win[2];   // the "native window" Mesa's surfaceless platform takes for a kopper window surface: {width, height}
#define eglGetProcAddress(n) (g_zk ? (void (*)(void))zink_eglGetProcAddress(n) : eglGetProcAddress(n))
#define eglGetDisplay(x) (g_zk ? (EGLDisplay)zink_eglGetPlatformDisplay(0x31DD, 0, 0) : eglGetDisplay(x))
#define eglInitialize(d, a, b) (g_zk ? zink_eglInitialize((void *)(d), (a), (b)) : eglInitialize((d), (a), (b)))
#define eglBindAPI(a) (g_zk ? zink_eglBindAPI(a) : eglBindAPI(a))
#define eglChooseConfig(d, a, c, m, n) (g_zk ? zink_eglChooseConfig((void *)(d), (a), (void **)(c), (m), (n)) : eglChooseConfig((d), (a), (c), (m), (n)))
#define eglCreateContext(d, c, s, a) (g_zk ? (EGLContext)zink_eglCreateContext((void *)(d), (void *)(c), (void *)(s), (a)) : eglCreateContext((d), (c), (s), (a)))
#define eglMakeCurrent(d, w, r, c) (g_zk ? zink_eglMakeCurrent((void *)(d), (void *)(w), (void *)(r), (void *)(c)) : eglMakeCurrent((d), (w), (r), (c)))
#define eglCreateWindowSurface(d, c, w, a) (g_zk ? (EGLSurface)zink_eglCreateWindowSurface((void *)(d), (void *)(c), (void *)g_zk_win, (a)) : eglCreateWindowSurface((d), (c), (w), (a)))
#define eglSwapBuffers(d, s) (g_zk ? zink_eglSwapBuffers((void *)(d), (void *)(s)) : eglSwapBuffers((d), (s)))
#define eglSwapInterval(d, i) (g_zk ? zink_eglSwapInterval((void *)(d), (i)) : eglSwapInterval((d), (i)))
#define eglQuerySurface(d, s, a, v) (g_zk ? zink_eglQuerySurface((void *)(d), (void *)(s), (a), (v)) : eglQuerySurface((d), (s), (a), (v)))
#define eglGetCurrentContext() (g_zk ? (EGLContext)zink_eglGetCurrentContext() : eglGetCurrentContext())
#define eglGetError() (g_zk ? zink_eglGetError() : eglGetError())
#else
#define g_zk 0
#endif

#define GLFW_PRESS 1
#define GLFW_RELEASE 0
#define GLFW_CURSOR 0x33001
#define GLFW_CURSOR_DISABLED 0x34003
#define KEY_MAX 349

typedef struct { const char *name; void *fn; } Sym;
typedef struct { int width, height, redBits, greenBits, blueBits, refreshRate; } VidMode;
typedef void (*KeyFn)(void *, int, int, int, int);
typedef void (*CharFn)(void *, unsigned);
typedef void (*MouseFn)(void *, int, int, int);
typedef void (*PosFn)(void *, double, double);
typedef void (*SizeFn)(void *, int, int);
typedef void (*FocusFn)(void *, int);

static int g_win_tag, g_mon_tag;
#define WIN ((void *)&g_win_tag)
#define MON ((void *)&g_mon_tag)
static EGLDisplay g_dpy; static EGLSurface g_surf; static EGLContext g_ctx;
static double g_rel_dx, g_rel_dy;   // camera movement of THIS poll (gameplay right stick); the SDL layer sends it as xrel/yrel, independent of the absolute pointer position
static int g_w = 1920, g_h = 1080, g_inited, g_created, g_cursor_mode, g_should_close, g_pad = -1, g_gui_mode, g_pad_user = -1;
static double g_cx = 960, g_cy = 540, g_t0, g_last_poll;
static void kmem_report(void);
static int g_pad_rest;   // GLFW games: the pad has been seen at rest since this game started; before that a held button (the launcher's Play press) or a drifting stick must not switch the menu pointer on
static int sdl_pad_armed;   // see the pad poll: input only counts after the pad was seen neutral once in this game
static int sdl_real_in;   // set by the pad poll once a button / stick is actually used (ps5sdl.inc stops its idle keep-alive bursts then)
static double g_in_maxgap, g_in_lostpx; static unsigned long g_in_capped;   // look diagnostics (printed with the "sdl: input" line): longest gap between pad polls, polls longer than the 0.1 s cap, look distance lost to the cap
static unsigned char g_keys[KEY_MAX + 1], g_mouse[8];
static KeyFn cb_key; static CharFn cb_char; static MouseFn cb_mouse; static PosFn cb_pos, cb_scroll; static SizeFn cb_fb, cb_size; static FocusFn cb_focus;
static void *cb_err, *cb_drop, *cb_close, *cb_enter, *cb_charmods, *cb_wpos, *cb_icon, *cb_max;
extern int ps5_java_dump_pending(void) __attribute__((weak));
extern void ps5_request_java_dump(void) __attribute__((weak));   // jvm_host.cpp: McBoot's crashwatch thread prints all Java stacks
extern void ps5_host_quit(const char *) __attribute__((weak));   // jvm_host.cpp: ask the host to return to the launcher (absent in the sandbox tests)
static uint32_t g_prev_buttons; static volatile uint32_t g_pad_latch; static int g_ver[3] = {3, 3, 8};

static long stub_zero(void) { return 0; }
static long stub_cursor(void) { return 1; }   // glfwCreate*Cursor: LWJGL's destroy NULL-checks the handle, so a mod (owo-lib) that makes and frees cursors needs a non-zero one
// ---------------- fast clock ----------------
// NPROF (1.16.5, distance 16): ~40% of the render thread's time sat in the clock syscall called from c23_timespec_get (the SDK/Mesa timestamps every prepare/submit/draw), and a clock_gettime
// costs ~3 us on this console. link adds --wrap=clock_gettime: CLOCK_MONOTONIC / CLOCK_REALTIME are answered from the TSC (anchor + ticks * ns/tick, re-anchored every 0.5 s from the real clock,
// ns/tick calibrated over the whole run so far, never goes backwards); every other clock id passes through.
extern int __real_clock_gettime(int, struct timespec *);
static struct { volatile unsigned seq; unsigned long long tsc0; long long mono0, real_off; double nspt; } g_fc;
static volatile int g_fc_lock; static unsigned long long g_fc_cal_tsc; static long long g_fc_cal_mono, g_fc_last; static unsigned long g_fc_fast, g_fc_slow;
static inline unsigned long long fc_rdtsc(void) { unsigned lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((unsigned long long)hi << 32) | lo; }
static void fc_put(int mono, long long m, long long ro, struct timespec *ts) { long long v = mono ? m : m + ro; ts->tv_sec = v / 1000000000LL; ts->tv_nsec = v % 1000000000LL; }
int __wrap_clock_gettime(int clk, struct timespec *ts) {
    int mono = clk == 4 || clk == 11 || clk == 12, real = clk == 0 || clk == 9 || clk == 10;
    if (!mono && !real) return __real_clock_gettime(clk, ts);
    unsigned long long tsc = fc_rdtsc(), t0; unsigned sq; long long m0, ro; double nspt;
    do { sq = g_fc.seq; t0 = g_fc.tsc0; m0 = g_fc.mono0; ro = g_fc.real_off; nspt = g_fc.nspt; __sync_synchronize(); } while ((sq & 1) || sq != g_fc.seq);
    if (nspt > 0 && tsc >= t0 && (double)(tsc - t0) * nspt < 500e6) {
        long long m = m0 + (long long)((double)(tsc - t0) * nspt), last = g_fc_last;
        if (m < last) m = last; else g_fc_last = m;
        fc_put(mono, m, ro, ts); g_fc_fast++; return 0;
    }
    g_fc_slow++;
    if (!__sync_bool_compare_and_swap(&g_fc_lock, 0, 1)) return __real_clock_gettime(clk, ts);
    struct timespec a, b; unsigned long long ta = fc_rdtsc(); __real_clock_gettime(4, &a); __real_clock_gettime(0, &b);
    long long m = (long long)a.tv_sec * 1000000000LL + a.tv_nsec, r = (long long)b.tv_sec * 1000000000LL + b.tv_nsec - m;
    if (!g_fc_cal_tsc) { g_fc_cal_tsc = ta; g_fc_cal_mono = m; }
    double n = g_fc.nspt;
    if (ta - g_fc_cal_tsc > 1000000ULL && m - g_fc_cal_mono >= 50000000LL) n = (double)(m - g_fc_cal_mono) / (double)(ta - g_fc_cal_tsc);
    g_fc.seq++; __sync_synchronize(); g_fc.tsc0 = ta; g_fc.mono0 = m; g_fc.real_off = r; g_fc.nspt = n; __sync_synchronize(); g_fc.seq++;
    if (m > g_fc_last) g_fc_last = m;
    g_fc_lock = 0;
    fc_put(mono, m, r, ts); return 0;
}
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

// ---------------- GL call profiler ----------------
// LWJGL gets every GL function through gl_proc(). For the calls that dominate Minecraft's render thread we hand out a timing wrapper instead: it forwards up to
// 10 integer/pointer arguments (all these functions take only integers/pointers), counts calls, accumulates time and tracks the GL state a slow draw needs
// explaining (program, FBO, texture, viewport, blend/depth/cull/scissor, masks). glfwSwapBuffers prints a summary with the perf line every ~2 s and a few
// "slowdraw" lines (>3 ms) per window.
typedef long (*glfn_t)(long, long, long, long, long, long, long, long, long, long);
enum { G_DRAWELEM, G_DRAWARR, G_BUFDATA, G_BUFSUB, G_CLEAR, G_TEXIMG, G_TEXSUB, G_USEPROG, G_BINDFBO, G_BINDTEX, G_GENMIP, G_FBTEX, G_READPIX, G_FLUSH, G_FINISH, G_BLIT,
       G_LINK, G_MAPRANGE, G_UNMAP, G_ENABLE, G_DISABLE, G_VIEWPORT, G_DEPTHMASK, G_COLORMASK, G_COMPILE,
       G_BINDVAO, G_UNI3FV, G_UNI4FV, G_UNIM4FV, G_VATTRI, G_VATTR, G_SHSRC, G_BINDATTR, G_N };
static const char *const g_gl_name[G_N] = { "glDrawElements", "glDrawArrays", "glBufferData", "glBufferSubData", "glClear", "glTexImage2D", "glTexSubImage2D", "glUseProgram",
    "glBindFramebuffer", "glBindTexture", "glGenerateMipmap", "glFramebufferTexture2D", "glReadPixels", "glFlush", "glFinish", "glBlitFramebuffer", "glLinkProgram",
    "glMapBufferRange", "glUnmapBuffer", "glEnable", "glDisable", "glViewport", "glDepthMask", "glColorMask", "glCompileShader",
    "glBindVertexArray", "glUniform3fv", "glUniform4fv", "glUniformMatrix4fv", "glVertexAttribIPointer", "glVertexAttribPointer", "glShaderSource", "glBindAttribLocation" };
static glfn_t g_gl_real[G_N];
static unsigned long g_gl_n[G_N];
static double g_gl_t[G_N], g_gl_max[G_N];
static long g_cur_prog, g_cur_fbo, g_cur_tex, g_vp_w, g_vp_h, g_read_fbo, g_draw_fbo; static int g_blitlog;   // g_read_fbo/g_draw_fbo/g_blitlog: the first glBlitFramebuffer calls are logged with their arguments ("blit #n ...")
static int g_blend, g_depth_test, g_cull, g_scissor, g_dmask = 1, g_cmask = 15, g_slow_budget = 6;
#ifdef PS5_GLALL
static void (*g_ga_fin)(void);   // glFinish: the GLALL probe calls it after every wrapped GL call; the timing wrapper below does it INSIDE its measured region for draws and clears, so "slowdraw" reports GPU time
#define SLOW_DRAW_S 0.050   // draws slower than this are listed (GPU time included)
#define SLOW_BUDGET 40
#else
#define SLOW_DRAW_S 0.003
#define SLOW_BUDGET 6
#endif
static void cap_probe(int idx, long tex);
static int g_texlog;   // texture-upload log: first 220 glTexImage2D/glTexSubImage2D/glGenerateMipmap calls ("texcall ...")
static unsigned (*g_real_glGetError)(void);
static int g_cap, g_frame_draws;   // draw capture: every ~8 s the next frame's first 50 draws are listed ("cap #n ..."); g_frame_draws counts all draws of a frame
static void gl_track(int i, long a, long b, long c, long d) {
    switch (i) {
    case G_USEPROG: g_cur_prog = a; break;
    case G_BINDFBO: g_cur_fbo = b; if (a == 0x8CA8) g_read_fbo = b; else if (a == 0x8CA9) g_draw_fbo = b; else if (a == 0x8D40) g_read_fbo = g_draw_fbo = b; break;   // READ / DRAW / both
    case G_BINDTEX: if (a == 0x0DE1) g_cur_tex = b; break;
    case G_VIEWPORT: g_vp_w = c; g_vp_h = d; break;
    case G_ENABLE: case G_DISABLE: { int on = (i == G_ENABLE); if (a == 0x0BE2) g_blend = on; else if (a == 0x0B71) g_depth_test = on; else if (a == 0x0B44) g_cull = on; else if (a == 0x0C11) g_scissor = on; break; }
    case G_DEPTHMASK: g_dmask = (int)(a & 1); break;
    case G_COLORMASK: g_cmask = (int)((a & 1) | ((b & 1) << 1) | ((c & 1) << 2) | ((d & 1) << 3)); break;
    default: break;
    }
}
#ifdef PS5_GLALL
#define GLW_FINISH(I) if (g_ga_fin && ((I) == G_DRAWELEM || (I) == G_DRAWARR || (I) == G_CLEAR)) g_ga_fin();
#else
#define GLW_FINISH(I)
#endif
#define GLW(I) static long gw_##I(long a, long b, long c, long d, long e, long f, long g, long h, long i, long j) { \
    gl_track(I, a, b, c, d); double t0 = now_s(); long r = g_gl_real[I](a, b, c, d, e, f, g, h, i, j); GLW_FINISH(I) double dt = now_s() - t0; \
    g_gl_n[I]++; g_gl_t[I] += dt; if (dt > g_gl_max[I]) g_gl_max[I] = dt; \
    if (((I) == G_TEXIMG || (I) == G_TEXSUB || (I) == G_GENMIP) && g_texlog < 220 && a == 0x0DE1) { g_texlog++; unsigned er = g_real_glGetError ? g_real_glGetError() : 0; \
        if ((I) == G_TEXIMG) TR("texcall #%d bound=%ld TexImage2D level=%ld ifmt=0x%lx %ldx%ld fmt=0x%lx type=0x%lx data=%s err=0x%x", g_texlog, g_cur_tex, b, c, d, e, g, h, i ? "yes" : "NULL", er); \
        else if ((I) == G_TEXSUB) TR("texcall #%d bound=%ld TexSubImage2D level=%ld at %ld,%ld size %ldx%ld fmt=0x%lx data=%s err=0x%x", g_texlog, g_cur_tex, b, c, d, e, f, g, i ? "yes" : "NULL", er); \
        else TR("texcall #%d bound=%ld GenerateMipmap err=0x%x", g_texlog, g_cur_tex, er); } \
    if ((I) == G_DRAWELEM || (I) == G_DRAWARR) { g_frame_draws++; if (g_cap > 0) { g_cap--; \
        TR("cap #%d %s count=%ld prog=%ld fbo=%ld tex=%ld vp=%ldx%ld blend=%d depth=%d cull=%d scis=%d dmask=%d cmask=%x %.2fms", 50 - g_cap, g_gl_name[I] + 2, (I) == G_DRAWELEM ? b : c, \
           g_cur_prog, g_cur_fbo, g_cur_tex, g_vp_w, g_vp_h, g_blend, g_depth_test, g_cull, g_scissor, g_dmask, g_cmask, dt * 1e3); \
        { int ci = 50 - g_cap; if ((I) == G_DRAWELEM && (ci == 1 || ci == 6 || ci == 12 || ci == 24 || ci == 26)) cap_probe(ci, g_cur_tex); } } } \
    if (((I) == G_DRAWELEM || (I) == G_DRAWARR) && dt > SLOW_DRAW_S && g_slow_budget > 0) { g_slow_budget--; \
        TR("slowdraw %.1fms %s count=%ld prog=%ld fbo=%ld tex=%ld vp=%ldx%ld blend=%d depth=%d cull=%d scis=%d dmask=%d cmask=%x", dt * 1e3, g_gl_name[I], \
           (I) == G_DRAWELEM ? b : c, g_cur_prog, g_cur_fbo, g_cur_tex, g_vp_w, g_vp_h, g_blend, g_depth_test, g_cull, g_scissor, g_dmask, g_cmask); } \
    if ((I) == G_BLIT && g_blitlog < 16) { g_blitlog++; TR("blit #%d read_fbo=%ld draw_fbo=%ld src %ld,%ld..%ld,%ld -> dst %ld,%ld..%ld,%ld mask=0x%lx (color 0x4000, depth 0x100, stencil 0x400) filter=0x%lx (nearest 0x2600, linear 0x2601) %.1fms", \
        g_blitlog, g_read_fbo, g_draw_fbo, a, b, c, d, e, f, g, h, i, j, dt * 1e3); } \
    return r; }
GLW(G_DRAWELEM) GLW(G_DRAWARR) GLW(G_BUFDATA) GLW(G_BUFSUB) GLW(G_CLEAR) GLW(G_TEXIMG) GLW(G_TEXSUB) GLW(G_USEPROG) GLW(G_BINDFBO) GLW(G_BINDTEX) GLW(G_GENMIP)
GLW(G_FBTEX) GLW(G_READPIX) GLW(G_FLUSH) GLW(G_FINISH) GLW(G_BLIT) GLW(G_LINK) GLW(G_MAPRANGE) GLW(G_UNMAP) GLW(G_ENABLE) GLW(G_DISABLE) GLW(G_VIEWPORT)
GLW(G_DEPTHMASK) GLW(G_COLORMASK) GLW(G_COMPILE) GLW(G_BINDVAO) GLW(G_UNI3FV) GLW(G_UNI4FV) GLW(G_UNIM4FV)
#ifndef PS5_VFIX
GLW(G_VATTRI) GLW(G_VATTR) GLW(G_SHSRC) GLW(G_BINDATTR)
#else
// ---- vertex format fix (build with VFIX=1) ----
// The SDK fetches only a few vertex formats natively. Profiling showed Mesa's u_vbuf converting Minecraft's integer light/overlay attribute (two GL_SHORT,
// R16G16_SINT) on the CPU for every draw (util_format_r16g16_sint_unpack_signed was the hottest SDK function, with its cache flushes and index-bounds scan), and the
// 3-byte normal (R8G8B8_SNORM) the same way. Formats that ARE native in the shipped build: R32_UINT, R8G8B8A8_SNORM/UNORM, R32/R32G32/R32G32B32 float.
// So: (1) glVertexAttribIPointer(2 x GL_SHORT) becomes 1 x GL_UNSIGNED_INT over the same 4 bytes, and the vertex shaders get "in uint UVn_raw_" plus a macro that
// rebuilds the signed ivec2 (UV1/UV2 keep their names in the shader body); glBindAttribLocation also binds UVn_raw_ at the same index. (2) glVertexAttribPointer(
// 3 x normalized GL_BYTE) becomes 4 x: MC's formats always pad the normal to 4 bytes, and "in vec3 Normal" simply ignores the fourth component.
static int g_vf_ptr, g_vf_nrm, g_vf_sh_ok, g_vf_sh_warn;
#ifdef PS5_MEGA
static void mg_mark_shader(unsigned id, int chunk);
static int mg_rewrite_chunk(const char *in, char *out);
#endif
// Sodium (0.6+) vertex layout, 20 bytes per vertex, all INTEGER attributes (log: glVertexAttribIPointer):  a_Position uvec2 = 2 x UNSIGNED_INT,  a_TexCoord uvec2 = 2 x UNSIGNED_SHORT,
// a_LightAndData uvec4 = 4 x UNSIGNED_BYTE. None of these is a format the SDK fetches natively (only R32_UINT, 8-bit normalised and 32-bit float are), and Sodium's terrain came out as
// stretched spikes. Same remedy as UV1/UV2: every attribute becomes plain R32_UINT (a_Position is split into two attributes, the second one at the fixed index SOD_POS_Y) and the shader
// rebuilds the vector (sod_rewrite below). Active only after a Sodium shader was seen (g_sod_active) so no other integer attribute is touched. NOSODFIX=1 (build_lwjgl.sh) switches it off.
// vfmt: every DISTINCT vertex attribute pointer format the application sets up (first 64), logged with the arguments as received (before any conversion): shows which layouts a renderer
// (vanilla, Sodium, Iris + a shader pack) really uses. Called from the vertex-format wrappers and from the chunk-merging layer's hooks.
static void vf_log_attr(const char *fn, long idx, long size, long type, long norm, long stride, long off) {
    static long seen[64][6]; static int n;
    long t[6] = { fn[14] == 'I' ? 1 : 0, idx, size, type | (norm << 20), stride, off };
    for (int k = 0; k < n; k++) if (!memcmp(seen[k], t, sizeof t)) return;
    if (n >= 64) return;
    memcpy(seen[n++], t, sizeof t);
    TR("vfmt: %s(index %ld, size %ld, type 0x%lx, normalized %ld, stride %ld, offset %ld)", fn, idx, size, type, norm, stride, off);
}
#define SOD_POS_Y 15   // Iris uses attribute indices 10, 11 and 12 for its own extra inputs (mc_Entity, at_midBlock, mc_midTexCoord): the second half of a_Position goes to the last of 16
static int g_sod_active, g_sod_conv, g_sod_seen;
// The one place that converts Sodium's integer attributes: called by gw_G_VATTRI below AND by the chunk-merging layer's own glVertexAttribIPointer hook (mw_VertexAttribIPointer in ps5mega.inc),
// because with MEGA=1 that hook sits in front of this wrapper and issues the driver call itself. Returns 1 when the call was split and fully issued through `real` (a_Position); otherwise
// *size / *type may have been changed and the caller issues the single call.
static int sod_iptr(unsigned idx, int *size, int *type, int stride, const void *ptr, void (*real)(unsigned, int, int, int, const void *)) {
#ifndef NO_SODFIX
    if (!g_sod_active) return 0;
    int os = *size, ot = *type, handled = 0;
    // The stride is not part of the rule: Sodium's own format is 20 bytes, Iris's extended one (extra mc_Entity / midBlock / midTexCoord) is 32.
    if (os == 2 && ot == 0x1403) { *size = 1; *type = 0x1405; g_sod_conv++; }                 // a_TexCoord: 2 x USHORT -> 1 x UINT
    else if (os == 4 && ot == 0x1401) { *size = 1; *type = 0x1405; g_sod_conv++; }            // a_LightAndData: 4 x UBYTE -> 1 x UINT
    else if (os == 2 && ot == 0x1405) {                                                       // a_Position: 2 x UINT -> two separate 1 x UINT attributes
        static void (*en)(unsigned); if (!en) en = (void (*)(unsigned))eglGetProcAddress("glEnableVertexAttribArray");
        real(idx, 1, 0x1405, stride, ptr); real(SOD_POS_Y, 1, 0x1405, stride, (const char *)ptr + 4); if (en) en(SOD_POS_Y);
        g_sod_conv++; handled = 1;
    }
    if (g_sod_seen < 10) { g_sod_seen++; TR("sodfix: glVertexAttribIPointer(%u, %d, 0x%x, stride %d, offset %ld) -> %s", idx, os, ot, stride, (long)ptr, handled ? "split into two R32_UINT attributes (index and 15)" : (*size != os || *type != ot) ? "converted to 1 x R32_UINT" : "unchanged"); }
    return handled;
#else
    return 0;
#endif
}
// Iris's terrain format also carries mc_midTexCoord as glVertexAttribPointer(2 x UNSIGNED_SHORT, NOT normalised): a float attribute from 16-bit "scaled" integers, again not a native fetch format. Same remedy: one
// R32_UINT attribute (glVertexAttribIPointer) and, in terrain shaders that declare it, a macro that rebuilds the vec2. Returns 1 when it issued the converted call through `reali`.
static int sod_fptr(unsigned idx, int size, int type, int norm, int stride, const void *ptr, void (*reali)(unsigned, int, int, int, const void *)) {
#ifndef NO_SODFIX
    if (!g_sod_active || size != 2 || type != 0x1403 || norm) return 0;
    reali(idx, 1, 0x1405, stride, ptr);
    static int logged; if (logged < 2) { logged++; TR("sodfix: glVertexAttribPointer(%u, 2, UNSIGNED_SHORT, not normalised, stride %d, offset %ld) -> 1 x R32_UINT (Iris mc_midTexCoord)", idx, stride, (long)ptr); }
    return 1;
#else
    return 0;
#endif
}
static void sod_real_g(unsigned i, int sz, int ty, int st, const void *p);
static long gw_G_VATTRI(long a, long b, long c, long d, long e, long f, long g, long h, long i, long j) {
    vf_log_attr("glVertexAttribIPointer", a, b, c, 0, d, e);
    if (b == 2 && c == 0x1402) { b = 1; c = 0x1405; g_vf_ptr++; }
    else { int sz = (int)b, ty = (int)c; if (sod_iptr((unsigned)a, &sz, &ty, (int)d, (const void *)e, sod_real_g)) return 0; b = sz; c = ty; }
    return g_gl_real[G_VATTRI](a, b, c, d, e, f, g, h, i, j);
}
static void sod_real_g(unsigned i, int sz, int ty, int st, const void *p) { g_gl_real[G_VATTRI](i, sz, ty, st, (long)p, 0, 0, 0, 0, 0); }
static long gw_G_VATTR(long a, long b, long c, long d, long e, long f, long g, long h, long i, long j) {
    vf_log_attr("glVertexAttribPointer", a, b, c, d, e, f);
    if (sod_fptr((unsigned)a, (int)b, (int)c, (int)d, (int)e, (const void *)f, sod_real_g)) return 0;
    if (b == 3 && c == 0x1400 && d) { b = 4; g_vf_nrm++; }
    return g_gl_real[G_VATTR](a, b, c, d, e, f, g, h, i, j);
}
static long gw_G_BINDATTR(long a, long b, long c, long d, long e, long f, long g, long h, long i, long j) {
    long r = g_gl_real[G_BINDATTR](a, b, c, d, e, f, g, h, i, j);
    if (c) { const char *n = (const char *)c;
        if (!strcmp(n, "UV1")) g_gl_real[G_BINDATTR](a, b, (long)"UV1_raw_", d, e, f, g, h, i, j);
        else if (!strcmp(n, "UV2")) g_gl_real[G_BINDATTR](a, b, (long)"UV2_raw_", d, e, f, g, h, i, j);
        else if (!strcmp(n, "iris_UV1")) g_gl_real[G_BINDATTR](a, b, (long)"iris_UV1_raw_", d, e, f, g, h, i, j);
        else if (!strcmp(n, "iris_UV2")) g_gl_real[G_BINDATTR](a, b, (long)"iris_UV2_raw_", d, e, f, g, h, i, j);
#ifndef NO_SODFIX
        else if (!strcmp(n, "a_Position")) { g_gl_real[G_BINDATTR](a, b, (long)"a_Position_x_", d, e, f, g, h, i, j); g_gl_real[G_BINDATTR](a, SOD_POS_Y, (long)"a_Position_y_", d, e, f, g, h, i, j); }
        else if (!strcmp(n, "a_TexCoord")) g_gl_real[G_BINDATTR](a, b, (long)"a_TexCoord_raw_", d, e, f, g, h, i, j);
        else if (!strcmp(n, "a_LightAndData")) g_gl_real[G_BINDATTR](a, b, (long)"a_LightAndData_raw_", d, e, f, g, h, i, j);
        else if (!strcmp(n, "mc_midTexCoord")) g_gl_real[G_BINDATTR](a, b, (long)"mc_midTexCoord_raw_", d, e, f, g, h, i, j);   // harmless when the shader is not a terrain one: the name does not exist there
#endif
    }
    return r;
}
// Sodium's three integer vertex inputs (see gw_G_VATTRI): the declarations are replaced by R32_UINT inputs plus a macro that rebuilds the original vector, so the shader body is untouched.
static int sod_has(const char *q, size_t l, const char *n) { size_t m = strlen(n); for (size_t i = 0; i + m <= l; i++) if (!memcmp(q + i, n, m)) return 1; return 0; }   // memmem is not in this console's libc
static int sod_rewrite(const char *in, char *out) {
    // The declared TYPE differs between Sodium/Iris versions (0.6: "in uvec4 a_LightAndData;", Iris on 1.20.1: "in ivec4 a_LightAndData;"), and the pointer side (sod_iptr) converts by format only,
    // so every signedness variant of a name must be rewritten, rebuilt with the type the shader declared (T below stands for that type's constructor).
    static const struct { const char *name; const char *types[3]; int kind; } T[] = {
        { "a_Position", { "uvec2", "ivec2", 0 }, 0 }, { "a_TexCoord", { "uvec2", "ivec2", 0 }, 1 }, { "a_LightAndData", { "uvec4", "ivec4", 0 }, 2 }, { "mc_midTexCoord", { "vec2", 0, 0 }, 3 } };
    char decl[8][64]; int dk[8], dn = 0; const int nt = strstr(in, "a_Position") ? 4 : 3;   // mc_midTexCoord only in terrain shaders (they declare a_Position)
    for (int k = 0; k < nt; k++) for (int t = 0; t < 3 && T[k].types[t]; t++) { snprintf(decl[dn], sizeof decl[dn], "in %s %s;", T[k].types[t], T[k].name); dk[dn] = k * 4 + t; dn++; }
    int hits = 0; const char *p = in; char *o = out;
    for (;;) {
        const char *best = 0; int bi = -1;
        for (int k = 0; k < dn; k++) { const char *q = strstr(p, decl[k]); if (q && (!best || q < best)) { best = q; bi = k; } }
        if (!best) break;
        memcpy(o, p, (size_t)(best - p)); o += best - p;
        const char *N = T[dk[bi] / 4].name, *Ty = T[dk[bi] / 4].types[dk[bi] % 4]; int kind = T[dk[bi] / 4].kind;
        switch (kind) {
        case 0: o += sprintf(o, "in uint %s_x_;\nin uint %s_y_;\n#define %s %s(%s_x_, %s_y_)\n", N, N, N, Ty, N, N); break;
        case 1: o += sprintf(o, "in uint %s_raw_;\n#define %s %s(%s_raw_ & 0xFFFFu, %s_raw_ >> 16u)\n", N, N, Ty, N, N); break;
        case 2: o += sprintf(o, "in uint %s_raw_;\n#define %s %s(%s_raw_ & 0xFFu, (%s_raw_ >> 8u) & 0xFFu, (%s_raw_ >> 16u) & 0xFFu, %s_raw_ >> 24u)\n", N, N, Ty, N, N, N, N); break;
        default: o += sprintf(o, "in uint %s_raw_;\n#define %s vec2(float(%s_raw_ & 0xFFFFu), float(%s_raw_ >> 16u))\n", N, N, N, N); break;
        }
        p = best + strlen(decl[bi]); hits++;
    }
    strcpy(o, p); return hits;
}
// Replaces every "in ivec2 UV1;" / "in ivec2 UV2;" declaration; returns the number of replacements. out must have room for in + 600 bytes.
static int vf_rewrite(const char *in, char *out) {
    // vanilla names UV1/UV2 and Iris's renamed copies (shader packs: "in ivec2 iris_UV2;"): the pointer conversion applies to every 2 x SHORT integer attribute, so every declaration must follow
    static const struct { const char *decl, *name; } T[4] = { { "in ivec2 UV1;", "UV1" }, { "in ivec2 UV2;", "UV2" }, { "in ivec2 iris_UV1;", "iris_UV1" }, { "in ivec2 iris_UV2;", "iris_UV2" } };
    int hits = 0; const char *p = in; char *o = out;
    for (;;) {
        const char *best = 0; int bi = -1;
        for (int k = 0; k < 4; k++) { const char *q = strstr(p, T[k].decl); if (q && (!best || q < best)) { best = q; bi = k; } }
        if (!best) break;
        memcpy(o, p, (size_t)(best - p)); o += best - p;
        const char *nm = T[bi].name;
        o += sprintf(o, "in uint %s_raw_;\n#define %s ivec2((int(%s_raw_) << 16) >> 16, int(%s_raw_) >> 16)\n", nm, nm, nm, nm);
        p = best + strlen(T[bi].decl); hits++;
    }
    strcpy(o, p); return hits;
}
static long gw_G_SHSRC(long a, long b, long c, long d, long e, long f, long g, long h, long i, long j) {   // glShaderSource(shader, count, strings, lengths)
    int n = (int)b; const char *const *st = (const char *const *)c; const int *ln = (const int *)d; size_t tot = 0;
    for (int k = 0; k < n; k++) tot += (ln && ln[k] >= 0) ? (size_t)ln[k] : strlen(st[k]);
    char *cat = (char *)malloc(tot + 1), *out = (char *)malloc(tot + 1200); size_t at = 0;
    if (!cat || !out) { free(cat); free(out); return g_gl_real[G_SHSRC](a, b, c, d, e, f, g, h, i, j); }
    for (int k = 0; k < n; k++) { size_t l = (ln && ln[k] >= 0) ? (size_t)ln[k] : strlen(st[k]); memcpy(cat + at, st[k], l); at += l; }
    cat[at] = 0;
    { static int sod_logged;   // Sodium's chunk shaders: show their vertex inputs (the attribute formats the SDK has to fetch)
      if (sod_logged < 3 && (strstr(cat, "a_PosId") || strstr(cat, "a_Pos ") || strstr(cat, "a_Color"))) {
          sod_logged++; TR("sodsh: shader %ld inputs (#version/in/layout lines of the source, %zu bytes):", a, at);
          int shown = 0; for (const char *q = cat; *q && shown < 24; ) { const char *eol = strchr(q, '\n'); size_t l = eol ? (size_t)(eol - q) : strlen(q);
              if ((l > 3 && !strncmp(q, "in ", 3)) || (l > 6 && !strncmp(q, "layout", 6)) || (l > 8 && !strncmp(q, "#version", 8)) || (l > 7 && !strncmp(q, "#define", 7) && (strstr(q, "VERT") || strstr(q, "POS") || strstr(q, "TEX")))) {
                  char b[160]; snprintf(b, sizeof b, "%.*s", (int)(l < 150 ? l : 150), q); TR("sodsh:   %s", b); shown++; }
              if (!eol) break; q = eol + 1; }
          if (sod_logged == 1) { int shown2 = 0; for (const char *q = cat; *q && shown2 < 48; ) { const char *eol = strchr(q, '\n'); size_t l = eol ? (size_t)(eol - q) : strlen(q);   // how the body decodes the attributes
              if (l > 3 && (strstr(q, "a_Position") || strstr(q, "a_TexCoord") || strstr(q, "a_LightAndData") || strstr(q, "_draw_id") || strstr(q, "gl_Position") || strstr(q, "u_RegionOffset") || strstr(q, "VERT"))) {
                  char b[200]; snprintf(b, sizeof b, "%.*s", (int)(l < 190 ? l : 190), q); TR("sodsh2: %s", b); shown2++; }
              if (!eol) break; q = eol + 1; } } } }
    { static int ir_logged;   // vertex shaders patched by Iris (shader packs): which inputs do they declare?
      if (ir_logged < 5 && strstr(cat, "gl_Position") && (strstr(cat, "iris_") || strstr(cat, "mc_Entity") || strstr(cat, "at_midBlock") || strstr(cat, "at_tangent")) && (ir_logged < 2 || strstr(cat, "a_Position"))) {
          ir_logged++; TR("irsh: Iris vertex shader %ld (%zu bytes), inputs:", a, at);
          int shown = 0; for (const char *q = cat; *q && shown < 22; ) { const char *eol = strchr(q, '\n'); size_t l = eol ? (size_t)(eol - q) : strlen(q);
              if ((l > 3 && !strncmp(q, "in ", 3)) || (l > 6 && !strncmp(q, "layout", 6)) || (l > 8 && !strncmp(q, "#version", 8))) { char b[160]; snprintf(b, sizeof b, "%.*s", (int)(l < 150 ? l : 150), q); TR("irsh:   %s", b); shown++; }
              if (!eol) break; q = eol + 1; } } }
    int hits = vf_rewrite(cat, out), mentions = (strstr(cat, "UV1") != 0 || strstr(cat, "UV2") != 0) && strstr(cat, "ivec2") != 0;
#ifndef NO_SODFIX
    { char *t = (char *)malloc(strlen(out) + 1200);
      if (t) { int sh = sod_rewrite(out, t); if (sh) { strcpy(out, t); hits += sh; if (!g_sod_active) TR("sodfix: Sodium vertex shader seen: integer inputs rewritten to R32_UINT (%d declarations in shader %ld)", sh, a); g_sod_active = 1; } free(t); }
      static int left_logged; if (g_sod_active && left_logged < 12) {   // any integer vertex input (int/uint/ivec/uvec) the rewrite left alone: its pointer format is converted by the pointer wrappers regardless of the name, so a mismatch here is the next artifact
          for (const char *q = out; q && *q; ) { const char *eol = strchr(q, '\n'); size_t l = eol ? (size_t)(eol - q) : strlen(q);
              if (l > 8 && !strncmp(q, "in ", 3) && (!strncmp(q + 3, "uvec", 4) || !strncmp(q + 3, "ivec", 4) || !strncmp(q + 3, "uint ", 5) || !strncmp(q + 3, "int ", 4)) && left_logged < 12 && !sod_has(q, l, "_raw_") && !sod_has(q, l, "_x_;") && !sod_has(q, l, "_y_;")) {
                  left_logged++; TR("sodfix: WARNING shader %ld still declares an integer input: %.*s", a, (int)(l < 100 ? l : 100), q); }
              q = eol ? eol + 1 : 0; } } }
#endif
    long r;
#ifdef PS5_MEGA
    { char *out2 = (char *)malloc(strlen(out) + 1200);   // chunk vertex shaders: ChunkOffset may come from the page table (see ps5mega.inc)
      if (out2 && mg_rewrite_chunk(out, out2) == 0) { mg_mark_shader((unsigned)a, 1); hits++; strcpy(out, out2); TR("mega: shader %ld is a chunk vertex shader, ChunkOffset rewritten", a); } else mg_mark_shader((unsigned)a, 0);
      free(out2); }
#endif
    if (hits) { const char *one = out; r = g_gl_real[G_SHSRC](a, 1, (long)&one, 0, e, f, g, h, i, j); g_vf_sh_ok++; if (g_vf_sh_ok <= 24) TR("vfix: shader %ld: %d integer attribute declaration(s) rewritten", a, hits); }
    else { r = g_gl_real[G_SHSRC](a, b, c, d, e, f, g, h, i, j);
           if (mentions) { g_vf_sh_warn++; const char *u = strstr(cat, "ivec2"); TR("vfix: WARNING shader %ld mentions UV1/UV2 and ivec2 but no declaration matched near: %.60s", a, u ? u : ""); } }
    free(cat); free(out); return r;
}
#endif
static glfn_t g_gl_wrap[G_N] = { gw_G_DRAWELEM, gw_G_DRAWARR, gw_G_BUFDATA, gw_G_BUFSUB, gw_G_CLEAR, gw_G_TEXIMG, gw_G_TEXSUB, gw_G_USEPROG, gw_G_BINDFBO, gw_G_BINDTEX,
    gw_G_GENMIP, gw_G_FBTEX, gw_G_READPIX, gw_G_FLUSH, gw_G_FINISH, gw_G_BLIT, gw_G_LINK, gw_G_MAPRANGE, gw_G_UNMAP, gw_G_ENABLE, gw_G_DISABLE, gw_G_VIEWPORT,
    gw_G_DEPTHMASK, gw_G_COLORMASK, gw_G_COMPILE, gw_G_BINDVAO, gw_G_UNI3FV, gw_G_UNI4FV, gw_G_UNIM4FV, gw_G_VATTRI, gw_G_VATTR, gw_G_SHSRC, gw_G_BINDATTR };
#ifdef PS5_GPUTIME
static void gt_mark(int tag);
#endif
#ifdef PS5_MEGA
#ifndef PS5_VFIX
#error MEGA needs VFIX (the chunk vertex layout is only recognisable after the vertex-format rewrite)
#endif
#include "ps5mega.inc"
#endif
static const unsigned char *(*g_real_getstring)(int);
static const unsigned char *gw_getstring(int name) {   // the returned strings never change; Minecraft's debug screen asks for them every frame
    static const unsigned char *cache[8]; static int cached[8]; static unsigned long hits;
    int slot = name >= 0x1F00 && name <= 0x1F03 ? name - 0x1F00 : name == 0x8B8C ? 4 : -1;
    if (slot < 0) return g_real_getstring(name);
    if (!cached[slot]) { cache[slot] = g_real_getstring(name); cached[slot] = 1; }
    else if (++hits % 2000 == 0) TR("glstring: %lu cached glGetString calls served without entering the driver", hits);
    return cache[slot];
}
// ---------------- shadow GL state ----------------
// Reading state back costs a full sync with the SDK's GL thread (measured: 16 queries = 16-58 ms per frame in the overlay). Minecraft sets all of this through calls we already see, so the few
// values the overlay has to restore are recorded as they go past, for free. The values are seeded once from the driver and re-checked rarely (SH_VERIFY_EVERY overlay frames) in case something
// sets them through a path we do not wrap; a mismatch is logged and the shadow corrected.
#define SH_VERIFY_EVERY 900
static struct { int dfbo, rfbo, vp[4], prog, vao, abuf, bf[4], blend, depth, cull, scis; unsigned char cm[4]; int seeded; } g_sh;
typedef long (*shfn_t)(long, long, long, long, long, long);
static shfn_t g_sh_in[10];
enum { SH_BINDFB, SH_VIEWPORT, SH_USEPROG, SH_BINDVAO, SH_BINDBUF, SH_ENABLE, SH_DISABLE, SH_COLORMASK, SH_BLENDFUNCSEP, SH_BLENDFUNC };
static void sh_cap(int cap, int on) { if (cap == 0x0BE2) g_sh.blend = on; else if (cap == 0x0B71) g_sh.depth = on; else if (cap == 0x0B44) g_sh.cull = on; else if (cap == 0x0C11) g_sh.scis = on; }
static long sh_BindFB(long t, long f, long c, long d, long e, long g) { if (t == 0x8D40 || t == 0x8CA9) g_sh.dfbo = (int)f; if (t == 0x8D40 || t == 0x8CA8) g_sh.rfbo = (int)f; return g_sh_in[SH_BINDFB](t, f, c, d, e, g); }
static long sh_Viewport(long x, long y, long w, long h, long e, long g) { g_sh.vp[0] = (int)x; g_sh.vp[1] = (int)y; g_sh.vp[2] = (int)w; g_sh.vp[3] = (int)h; return g_sh_in[SH_VIEWPORT](x, y, w, h, e, g); }
static long sh_UseProg(long p, long b, long c, long d, long e, long g) { g_sh.prog = (int)p; return g_sh_in[SH_USEPROG](p, b, c, d, e, g); }
static long sh_BindVAO(long v, long b, long c, long d, long e, long g) { g_sh.vao = (int)v; return g_sh_in[SH_BINDVAO](v, b, c, d, e, g); }
static long sh_BindBuf(long t, long b, long c, long d, long e, long g) { if (t == 0x8892) g_sh.abuf = (int)b; return g_sh_in[SH_BINDBUF](t, b, c, d, e, g); }
static long sh_Enable(long c, long b, long x, long d, long e, long g) { sh_cap((int)c, 1); return g_sh_in[SH_ENABLE](c, b, x, d, e, g); }
static long sh_Disable(long c, long b, long x, long d, long e, long g) { sh_cap((int)c, 0); return g_sh_in[SH_DISABLE](c, b, x, d, e, g); }
static long sh_ColorMask(long r, long gg, long b, long a, long e, long f) { g_sh.cm[0] = (unsigned char)r; g_sh.cm[1] = (unsigned char)gg; g_sh.cm[2] = (unsigned char)b; g_sh.cm[3] = (unsigned char)a; return g_sh_in[SH_COLORMASK](r, gg, b, a, e, f); }
static long sh_BlendSep(long sr, long dr, long sa, long da, long e, long f) { g_sh.bf[0] = (int)sr; g_sh.bf[1] = (int)dr; g_sh.bf[2] = (int)sa; g_sh.bf[3] = (int)da; return g_sh_in[SH_BLENDFUNCSEP](sr, dr, sa, da, e, f); }
static long sh_BlendFunc(long sf, long df, long c, long d, long e, long g) { g_sh.bf[0] = g_sh.bf[2] = (int)sf; g_sh.bf[1] = g_sh.bf[3] = (int)df; return g_sh_in[SH_BLENDFUNC](sf, df, c, d, e, g); }
static const struct { const char *name; void *wrap; int idx; } g_sh_tab[] = {
    { "glBindFramebuffer", (void *)sh_BindFB, SH_BINDFB }, { "glViewport", (void *)sh_Viewport, SH_VIEWPORT }, { "glUseProgram", (void *)sh_UseProg, SH_USEPROG },
    { "glBindVertexArray", (void *)sh_BindVAO, SH_BINDVAO }, { "glBindBuffer", (void *)sh_BindBuf, SH_BINDBUF }, { "glEnable", (void *)sh_Enable, SH_ENABLE },
    { "glDisable", (void *)sh_Disable, SH_DISABLE }, { "glColorMask", (void *)sh_ColorMask, SH_COLORMASK }, { "glBlendFuncSeparate", (void *)sh_BlendSep, SH_BLENDFUNCSEP },
    { "glBlendFunc", (void *)sh_BlendFunc, SH_BLENDFUNC }, { 0, 0, 0 } };
static void *sh_lookup(const char *n, void *inner) {
    for (int i = 0; g_sh_tab[i].name; i++) if (!strcmp(n, g_sh_tab[i].name)) { g_sh_in[g_sh_tab[i].idx] = (shfn_t)inner; return g_sh_tab[i].wrap; }
    return 0;
}
#include <dirent.h>
// Relative paths. The platform refuses chdir (EPERM, even to /app0), so a mod that writes "./config/x.properties" (Sodium, Lithium, ModernFix ...) hit the process directory, where it cannot create anything.
// McBoot's Hello.chdir(gameDir) now also records the folder here (ps5_set_vcwd), and the path calls the JDK makes with a RELATIVE path are redone against it. Absolute paths pass untouched.
static char g_vcwd[512];
void ps5_set_vcwd(const char *p) { snprintf(g_vcwd, sizeof g_vcwd, "%s", p ? p : ""); }
static const char *vpath(const char *p, char *b, size_t n) {
    if (!p || !p[0] || p[0] == '/' || !g_vcwd[0]) return p;
    if (p[0] == '.' && !p[1]) return g_vcwd;
    snprintf(b, n, "%s/%s", g_vcwd, (p[0] == '.' && p[1] == '/') ? p + 2 : p);
    return b;
}
#define PS5_HAVE_VPATH 1
#define VP char vb_[1100]; p = vpath(p, vb_, sizeof vb_)
#include "ps5fdv.inc"   // virtual descriptors for .jar/.zip files (the console allows only ~264 open files)
#ifndef PS5_VK
#define LPB(site) (void)0
#define LPA(site) (void)0
#endif
#ifdef PS5_VK
// pthread_create ERANGE: the log of a 1.20.1 world on Zink ended with  "Failed to start thread - pthread_create failed (ERANGE) for attributes: stacksize: 1024k, guardsize: 134250438k"  (a ~128 GB
// guard size on a thread that asked for none). The console's pthread_attr_init leaves the guard size unset, so it holds whatever the allocator handed back; most of the time that is 0, when it is
// a leftover pointer the stack allocation is refused and the VM dies. Any guard size above 1 MB is such garbage (nobody asks for one): replace it with one page.
int __real_pthread_attr_setstacksize(pthread_attr_t *, size_t);
extern int __real_pthread_attr_init(pthread_attr_t *) __attribute__((weak));
// Weak: an existing wrapper elsewhere wins instead of clashing at link time. Zero first, so the fields the console's pthread_attr_init leaves alone are 0, not stale stack bytes (garbage guard size -> pthread_create ERANGE).

extern int ps5_p_getaff(pthread_t, size_t, void *) __asm__("pthread_getaffinity_np") __attribute__((weak));   // other C names + asm labels: no clash with whatever the SDK headers declare, and weak so a missing symbol cannot break the link
extern int ps5_p_attr_setaff(pthread_attr_t *, size_t, const void *) __asm__("pthread_attr_setaffinity_np") __attribute__((weak));
extern int ps5_p_cpuclock(pthread_t, clockid_t *) __asm__("pthread_getcpuclockid") __attribute__((weak));
// Log 2a069276: pthread_mutex_init fails with ENOMEM (12) once ~9,800 mutexes are alive (HotSpot makes ~9,250 before the game even starts, ~2 per class loader, and the console's libc has a fixed pool), and HotSpot ignores
// the result: the junk left in the mutex makes pthread_mutex_lock fault (SIGBUS). When the pool is empty a new mutex now shares the libc object of one of the first mutexes made by the SAME call site (the VM's own
// start-up mutexes live for the whole run, and a site's mutexes are the same kind and rank, so they are never nested). The handle is just copied, so lock/unlock/cond_wait need no wrapper; destroy must never free a
// shared object, so it is skipped for any handle that is a donor.
extern int __real_pthread_mutex_init(pthread_mutex_t *, const pthread_mutexattr_t *) __attribute__((weak));
extern int __real_pthread_mutex_destroy(pthread_mutex_t *) __attribute__((weak));
#define MX_SITES 96
#define MX_DON 12
static struct { void *site; int n; unsigned rr; unsigned tot; unsigned long long h[MX_DON]; } g_mxs[MX_SITES];
static volatile int g_mx_lock, g_mx_nsites;
static volatile int g_mx_n, g_mx_type = -9, g_mx_ps = -9, g_mx_pr = -9, g_mx_fail, g_mx_destroyed, g_mx_shared;
#define MXL() while (__sync_lock_test_and_set(&g_mx_lock, 1)) { }
#define MXU() __sync_lock_release(&g_mx_lock)
static int mx_is_donor(unsigned long long h) { int i, k, r = 0; MXL(); for (i = 0; i < g_mx_nsites && !r; i++) for (k = 0; k < g_mxs[i].n; k++) if (g_mxs[i].h[k] == h) { r = 1; break; } MXU(); return r; }
static int mx_real_init(pthread_mutex_t *m) { return __real_pthread_mutex_init ? __real_pthread_mutex_init(m, NULL) : pthread_mutex_init(m, NULL); }
static volatile int g_mx_real, g_mx_realdel, g_mx_lazy;
#define MX_BULK 64   // a call site that already made this many mutexes is a table (log dda11ab6: two JDK 'init' sites made 4,096 each = 8,192 of the pool's ~9,800 at start-up; the fd tables of libnet): its further mutexes are not allocated now
// Which code makes the mutexes (log 08ad4da3: ~8,800 made by the 25th thread, hidden-class loaders were only ~5%). Prints the 8 busiest creators as  return-address=count  (resolve with addr2line).
#include <sys/mman.h>
static unsigned long long *g_lazy_p[8192];   // addresses of the deferred (zeroed) mutexes: a non-zero handle later means the libc allocated it at a first lock
#include <sys/resource.h>
// How many separate mappings can still be made (each one leaves a hole behind it so the kernel cannot merge it with a neighbour), and the process limits: the libc's internal allocator for mutex/cond/attr
// objects is page based (mmap); if the number of mappings or the address space is what ends, that explains why every small object fails together while malloc and a 1 MB mmap still work.
static int map_headroom(int cap, int *err) {
    size_t pg = (size_t)getpagesize(); void **v = (void **)calloc(cap, sizeof(void *)); int n = 0; *err = 0; if (!v) { *err = -1; return -1; }
    while (n < cap) { char *p = (char *)mmap(0, 2 * pg, 3, MAP_ANON | MAP_PRIVATE, -1, 0); if (p == (char *)MAP_FAILED) { *err = errno; break; } munmap(p + pg, pg); v[n++] = p; }
    for (int i = 0; i < n; i++) munmap(v[i], pg);
    free(v); return n; }
extern int sysctlbyname(const char *, void *, size_t *, const void *, size_t) __attribute__((weak));
static int g_fd_err;
static int fd_headroom(int cap) {   // how many more descriptors this process can still open (open /dev/null until it fails, errno in g_fd_err; all closed again). dup/fcntl are not usable on this console.
    int *v = (int *)calloc(cap, sizeof(int)), n = 0; g_fd_err = 0; if (!v) return -2;
    while (n < cap) { int d = open("/dev/null", O_RDONLY); if (d < 0) { g_fd_err = errno; break; } v[n++] = d; }
    for (int i = 0; i < n; i++) close(v[i]);
    free(v); return n; }
static int fd_open_now(int cap) { int n = 0; struct stat st; for (int i = 0; i < cap; i++) if (fstat(i, &st) == 0) n++; return n; }
static void fd_text(char *b, size_t nb, int full) {   // one line: descriptors open now, still possible, and the kernel's own limits when sysctl answers
    int now = fd_open_now(4096), more = full ? fd_headroom(70000) : -1;   /* the headroom test takes every free descriptor for a moment: start-up only, never while the game runs */ int mf = -1, mp = -1, op = -1; size_t l = sizeof(int);
    if (sysctlbyname) { l = sizeof(int); if (sysctlbyname("kern.maxfiles", &mf, &l, 0, 0)) mf = -1; l = sizeof(int); if (sysctlbyname("kern.maxfilesperproc", &mp, &l, 0, 0)) mp = -1; l = sizeof(int); if (sysctlbyname("kern.openfiles", &op, &l, 0, 0)) op = -1; }
    snprintf(b, nb, "fds: %d open now, %d more possible (cap 70000, errno %d) | kern.maxfiles=%d maxfilesperproc=%d openfiles=%d", now, more, g_fd_err, mf, mp, op); }
static void limits_text(char *b, size_t nb) {
    static const struct { int r; const char *n; } L[] = { { RLIMIT_AS, "AS" }, { RLIMIT_DATA, "DATA" }, { RLIMIT_STACK, "STACK" }, { RLIMIT_NPROC, "NPROC" }, { RLIMIT_NOFILE, "NOFILE" }, { RLIMIT_MEMLOCK, "MEMLOCK" } };
    size_t o = 0; b[0] = 0;
    for (unsigned i = 0; i < sizeof L / sizeof L[0]; i++) { struct rlimit rl; if (getrlimit(L[i].r, &rl) == 0) o += snprintf(b + o, nb - o, "%s=%lld/%lld ", L[i].n, rl.rlim_cur == RLIM_INFINITY ? -1LL : (long long)rl.rlim_cur, rl.rlim_max == RLIM_INFINITY ? -1LL : (long long)rl.rlim_max); else o += snprintf(b + o, nb - o, "%s=? ", L[i].n); }
}
static void kmem_report(void);
extern void *__real_malloc(size_t) __attribute__((weak)); extern void __real_free(void *) __attribute__((weak));   // the libc's OWN heap (malloc is wrapped by the SDK app_heap.c, so the probe below only ever tested that)
extern int malloc_stats_fast(void *) __attribute__((weak));   // Sony libc: {size, maxSystemSize, currentSystemSize, maxInuseSize, currentInuseSize}
// The libc's OWN heap (everything the system libraries and the libc itself allocate; the pthread objects too): how much can still be taken from it, and what its stats say. Log b7f3a58d: at the stall
// __real_malloc(64) returned NULL while the wrapped malloc still served 128 MB. Takes up to 128 MB for a moment: called at start-up, at the cliff and at stalls only.
// Cheap read of the libc heap's free space: how many 256 KB pieces (cap 64 = 16 MB) it can still hand out. A few tens of microseconds; used per tick and per new thread to find what drains it.
static int g_libprobe = -1;   /* diagnostic only: the probe takes up to 16 MB of the libc heap for a moment, and libc is what libthr/sce use for thread starts: that can turn into the freeze it looks for. Off unless .kit/libprobe exists */
static int libc_free_pieces(void) {
    if (g_libprobe < 0) g_libprobe = access("/app0/mc/.kit/libprobe", F_OK) == 0;
    if (!g_libprobe) return -1;
    if (!__real_malloc || !__real_free) return -1;
    void *v[64]; int n = 0, i; while (n < 64 && (v[n] = __real_malloc(256u << 10))) n++;
    for (i = 0; i < n; i++) __real_free(v[i]);
    return n;
}
/* Where does the libc heap drain? Each bracketed call samples the free 256K pieces before and after; a loss inside the call is credited to it (a leak of a few KB per call still shows: the piece count drops exactly when
   the heap frontier crosses a piece boundary, and that happens inside the leaking call). Sites: 0 flip, 1 pad read, 2 audio output, 3 kernel-memory wraps, 4 pthread_create. */
static volatile long g_lp_loss[6], g_lp_n[6];
#define LPB(site) int lpb_ = g_lp_on ? libc_free_pieces() : -1
#define LPA(site) do { if (lpb_ >= 0) { int a_ = libc_free_pieces(); __atomic_fetch_add(&g_lp_n[site], 1, __ATOMIC_RELAXED); if (a_ >= 0 && a_ < lpb_) __atomic_fetch_add(&g_lp_loss[site], lpb_ - a_, __ATOMIC_RELAXED); } } while (0)
static void *churn_fn(void *a) { (void)a; return 0; }
static volatile int g_lp_on;   /* switched on once the first world is entered (the tick sets it); the probe costs ~0.1 ms per bracket */
static void libc_heap_probe(const char *why) {
    if (!__real_malloc || !__real_free) { static int once; if (!once++) TR("libcheap: __real_malloc is not linked (malloc is not wrapped in this build)"); return; }
    static void *v[512]; int n = 0, i; void *a = __real_malloc(64), *b = __real_malloc(4096), *c = __real_malloc(1u << 20); char st[200] = "";
    while (n < 512 && (v[n] = __real_malloc(256u << 10))) n++;
    for (i = 0; i < n; i++) __real_free(v[i]);
    if (malloc_stats_fast) { size_t m[5] = { 0, 0, 0, 0, 0 }; int r = malloc_stats_fast(m); snprintf(st, sizeof st, " | stats rc %d size %zu maxSys %zu curSys %zu maxInuse %zu curInuse %zu", r, m[0], m[1], m[2], m[3], m[4]); }
    TR("libcheap (%s): 64B=%s 4K=%s 1M=%s, %d MB more available in 256K pieces (cap 128)%s", why, a ? "ok" : "NULL", b ? "ok" : "NULL", c ? "ok" : "NULL", n / 4, st);
    if (a) __real_free(a); if (b) __real_free(b); if (c) __real_free(c);
}
static void enomem_probe(const char *why) {   // what still works at the moment pthread_attr_init / pthread_mutex_init return ENOMEM?
    libc_heap_probe(why);
    static const size_t sz[] = { 64, 4096, 65536, 1u << 20, 16u << 20, 128u << 20 }; char b[420]; int o = 0, i, r; void *q;
    for (i = 0; i < 6; i++) { q = malloc(sz[i]); o += snprintf(b + o, sizeof b - o, "malloc(%zuK)=%s ", sz[i] >> 10, q ? "ok" : "NULL"); free(q); }
    { pthread_cond_t c; memset((void *)&c, 0, sizeof c); r = pthread_cond_init(&c, 0); o += snprintf(b + o, sizeof b - o, "cond=%d ", r); if (!r) pthread_cond_destroy(&c); }
    { pthread_key_t k; r = pthread_key_create(&k, 0); o += snprintf(b + o, sizeof b - o, "key=%d ", r); if (!r) pthread_key_delete(k); }
    { pthread_mutex_t mm; memset((void *)&mm, 0, sizeof mm); r = __real_pthread_mutex_init ? __real_pthread_mutex_init(&mm, 0) : pthread_mutex_init(&mm, 0); o += snprintf(b + o, sizeof b - o, "mutex=%d ", r); if (!r && __real_pthread_mutex_destroy) __real_pthread_mutex_destroy(&mm); }
    { pthread_attr_t at; memset((void *)&at, 0, sizeof at); r = __real_pthread_attr_init ? __real_pthread_attr_init(&at) : pthread_attr_init(&at); o += snprintf(b + o, sizeof b - o, "attr=%d ", r); if (!r) pthread_attr_destroy(&at); }
    q = mmap(0, 1u << 20, 3, MAP_ANON | MAP_PRIVATE, -1, 0); o += snprintf(b + o, sizeof b - o, "mmap(1M)=%s", q == MAP_FAILED ? "FAILED" : "ok"); if (q != MAP_FAILED) munmap(q, 1u << 20);
    TR("threads: ENOMEM probe (%s): %s", why, b);
    { static int mapped; if (mapped++ < 2) { int e = 0, n = map_headroom(100000, &e); char lb[200]; limits_text(lb, sizeof lb);
        TR("threads: ENOMEM probe: %d more separate mappings possible (cap 100000, errno %d), page %d B", n, e, getpagesize()); TR("threads: limits (cur/max, -1 = none): %s", lb); } }
    kmem_report();
}
static void mx_dump(const char *why) {
    void *sites[MX_SITES]; unsigned t[MX_SITES]; int n, i, k, o = 0; char buf[400];
    MXL(); n = g_mx_nsites < MX_SITES ? g_mx_nsites : MX_SITES; for (i = 0; i < n; i++) { sites[i] = g_mxs[i].site; t[i] = g_mxs[i].tot; } MXU();
    for (k = 0; k < 8 && k < n; k++) { int b = k; for (i = k + 1; i < n; i++) if (t[i] > t[b]) b = i;
        { void *ts = sites[k]; unsigned tt = t[k]; sites[k] = sites[b]; t[k] = t[b]; sites[b] = ts; t[b] = tt; }
        o += snprintf(buf + o, sizeof buf - o, " %p=%u", sites[k], t[k]); }
    { int k, nz = 0, nl = g_mx_lazy < 8192 ? g_mx_lazy : 8192; for (k = 0; k < nl; k++) if (g_lazy_p[k] && *g_lazy_p[k]) nz++;
      TR("threads: mutex creators at %s (made %d, live %d, shared %d, deferred %d of which the libc has allocated %d since):%s", why, g_mx_n, g_mx_real - g_mx_realdel, g_mx_shared, g_mx_lazy, nz, buf); }
}   // libc mutexes actually alive (the pool is about 9,800)
#define MX_SHARE_ABOVE 9000   // from here new mutexes of a site that has donors share one: the last ~800 libc mutexes stay free for lazily created ones (statically initialised mutexes allocate on first lock and crash when the pool is empty: log d0188657)
// Reserve of REAL mutexes made at start-up while the libc still has room (ps5_calib). Log 0c100db5: after the pool ran dry every new mutex was handed a donor's libc mutex ("shares a start-up mutex of its call site"),
// and every such event was followed by a whole-JVM stop of 13-52 s: HotSpot makes all its Monitors at ONE call site, so two different VM locks became one pthread mutex (nested lock = self-deadlock until a timeout
// breaks it). A reserve mutex stays unique; sharing is the last resort once the reserve is used up.
#define MX_RES 512
static pthread_mutex_t g_mx_res[MX_RES]; static volatile int g_mx_res_n, g_mx_res_used;
static int mx_share(pthread_mutex_t *m, void *site, int rc, int live) {
    int i, f; unsigned long long dh = 0; if (sizeof(pthread_mutex_t) != 8) return 0;
    MXL(); for (i = 0; i < g_mx_nsites; i++) if (g_mxs[i].site == site) break;
    if (i < g_mx_nsites && g_mxs[i].n > 0) dh = g_mxs[i].h[g_mxs[i].rr++ % g_mxs[i].n]; MXU();
    if (!dh) return 0;
    *(unsigned long long *)m = dh; __sync_fetch_and_add(&g_mx_shared, 1); f = __sync_fetch_and_add(&g_mx_fail, 1);
    if (f == 0) { mx_dump("the first shared mutex"); enomem_probe("first mutex_init failure"); }
    if (f < 6 || (f % 2000) == 0) TR("threads: mutex pool %d alive (init rc=%d): new mutex #%d shares a start-up mutex of its call site %p", live, rc, g_mx_n, site);
    return 1;
}
__attribute__((weak)) int __wrap_pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *at) {
    void *site = __builtin_return_address(0); int i, k, rc, ok8 = sizeof(pthread_mutex_t) == 8, live;
    if (at && !g_mx_n) { int t = -1, ps = -1, pr = -1; pthread_mutexattr_gettype(at, &t); pthread_mutexattr_getpshared(at, &ps); pthread_mutexattr_getprotocol(at, &pr); g_mx_type = t; g_mx_ps = ps; g_mx_pr = pr; }
    __sync_fetch_and_add(&g_mx_n, 1);
    // Table-sized sites: leave the mutex zeroed. A zeroed mutex is a statically initialised one (PTHREAD_MUTEX_INITIALIZER): the console's libc allocates it at its first lock, so a table of 4,096 per-fd
    // mutexes costs one pool entry per fd that is really used instead of 4,096.
    if (ok8) { int big = 0; MXL(); for (i = 0; i < g_mx_nsites; i++) if (g_mxs[i].site == site) { big = i < MX_SITES && g_mxs[i].tot >= MX_BULK; break; } MXU();
        if (big) { int li; memset((void *)m, 0, sizeof *m); li = __sync_fetch_and_add(&g_mx_lazy, 1); if (li < 8192) g_lazy_p[li] = (unsigned long long *)m; return 0; } }
    live = g_mx_real - g_mx_realdel;
    if (live >= MX_SHARE_ABOVE && mx_share(m, site, 0, live)) return 0;
    rc = mx_real_init(m);
    for (k = 0; rc != 0 && k < 3; k++) { usleep(500); rc = mx_real_init(m); }
    if (rc == 0) {
        __sync_fetch_and_add(&g_mx_real, 1);
        if (ok8) { unsigned long long h = *(unsigned long long *)m; MXL(); for (i = 0; i < g_mx_nsites; i++) if (g_mxs[i].site == site) break;
            if (i == g_mx_nsites && i < MX_SITES) { g_mxs[i].site = site; g_mx_nsites++; }
            if (i < MX_SITES) g_mxs[i].tot++;
            if (i < MX_SITES && g_mxs[i].n < MX_DON && h > 0x10000) g_mxs[i].h[g_mxs[i].n++] = h; MXU(); }
        return 0;
    }
    if (ok8) { int ri = __sync_fetch_and_add(&g_mx_res_used, 1); if (ri < g_mx_res_n) { memcpy((void *)m, (void *)&g_mx_res[ri], sizeof *m); __sync_fetch_and_add(&g_mx_real, 1); if (ri < 3 || ri == MX_RES - 1 || ri % 100 == 0) TR("threads: pthread_mutex_init rc=%d: reserve mutex #%d of %d handed out (a real one, made at start-up)", rc, ri + 1, g_mx_res_n); return 0; } }
    if (mx_share(m, site, rc, live)) return 0;
    { int f = __sync_fetch_and_add(&g_mx_fail, 1); if (f < 8) TR("threads: pthread_mutex_init FAILED rc=%d, no donor for call site %p (alive %d): mutex zeroed", rc, site, live); }
    memset((void *)m, 0, sizeof *m);
    return 0;
}
__attribute__((weak)) int __wrap_pthread_mutex_destroy(pthread_mutex_t *m) {
    __sync_fetch_and_add(&g_mx_destroyed, 1);
    if (sizeof(pthread_mutex_t) == 8 && *(unsigned long long *)m == 0) return 0;   // a zeroed (donor-less) mutex owns nothing
    if (sizeof(pthread_mutex_t) == 8 && g_mx_shared >= 0 && mx_is_donor(*(unsigned long long *)m)) return 0;   // a shared libc mutex is never freed
    __sync_fetch_and_add(&g_mx_realdel, 1);
    return __real_pthread_mutex_destroy ? __real_pthread_mutex_destroy(m) : pthread_mutex_destroy(m);
}
// The JDK derives file.encoding and sun.jnu.encoding (not overridable with -D) from setlocale(). The JDK 25 stand-in answered "C": every non-ASCII file name became an InvalidPathException (log 3981c9d6: resource packs
// named "NewGlowingOres-\xC2\xA76[Border]\xC2\xA7r.zip" failed to open). --wrap routes the JDK's own calls here, so this holds for every linked JDK without rebuilding any of them.
char *__wrap_setlocale(int cat, const char *loc) { (void)cat; (void)loc; return (char *)"en_US.UTF-8"; }

#ifndef PS5_NO_OWNMALLOC
#define OM_LOG(...) TR(__VA_ARGS__)
#include "ps5malloc.inc"   // the title's own malloc (see the file): native memory no longer drains the console libc's bounded heap
#ifndef PS5_NO_RTLDHEAP
// The console's runtime linker (libkernel) allocates its own bookkeeping - and, it appears, the userland objects behind pthread mutexes / conditions / attributes and the thread control blocks - through an
// "application heap API" that the clean-room libc.prx shim sets at load time to the console libc's malloc / free / posix_memalign (tooling/native/libc_builder.cpp: heap API at +0 malloc, +8 free, +0x30 posix_memalign;
// the other slots stay zero). That is the libc heap whose ~12 MB runs dry (pthread_mutex_init / attr_init ENOMEM, new threads that never start). Re-pointing it at the title's own allocator takes all of that off the
// libc heap. Pointers the runtime linker already got from the libc go through __wrap_free's "not ours" path into the libc's free.
extern long _sceKernelRtldSetApplicationHeapAPI(void *api) __attribute__((weak));   /* the SDK stubs of libkernel export it */
static volatile unsigned long g_rt_malloc, g_rt_free, g_rt_pm; static volatile int g_rt_on;
static void *rt_malloc(size_t n) { __atomic_fetch_add(&g_rt_malloc, 1, __ATOMIC_RELAXED); return __wrap_malloc(n); }
static void rt_free(void *p) { __atomic_fetch_add(&g_rt_free, 1, __ATOMIC_RELAXED); __wrap_free(p); }
static int rt_pm(void **mp, size_t al, size_t sz) { __atomic_fetch_add(&g_rt_pm, 1, __ATOMIC_RELAXED); return __wrap_posix_memalign(mp, al, sz); }
static void *g_rt_api[9];
static void rtld_heap_install(void) {
    if (g_rt_on) return; g_rt_on = 1;
    g_rt_api[0] = (void *)rt_malloc; g_rt_api[1] = (void *)rt_free; g_rt_api[6] = (void *)rt_pm;
    if (!_sceKernelRtldSetApplicationHeapAPI) { TR("rtld heap: _sceKernelRtldSetApplicationHeapAPI is not linked"); return; }
    long rc = _sceKernelRtldSetApplicationHeapAPI(g_rt_api);
    TR("rtld heap: application heap API re-pointed at the title's allocator (rc 0x%lx)", rc);
}
#endif
#endif
// ---- Suspects for the libc-heap drain (log ea5bc9b3: the pool of free 256K pieces falls 49 -> 0 during play with no new threads, mutexes or attrs). Library functions that allocate INSIDE the libc (where our malloc wrap
// cannot see them) are counted here; the per-second "tl:" line prints the counters that moved next to the change in free pieces, so a drop can be matched to its caller. They do nothing but count and call through.
enum { SU_FOPEN, SU_FDOPEN, SU_OPENDIR, SU_STRDUP, SU_STRNDUP, SU_SETENV, SU_GETCWD, SU_REALPATH, SU_DLOPEN, SU_KEYCREATE, SU_SEMINIT, SU_LOCALTIME, SU_TZSET, SU_MXATTR, SU_CVATTR, SU_ASPRINTF, SU_VASPRINTF, SU_TMPFILE, SU_SCANDIR, SU_N };
static volatile unsigned long g_su[SU_N];
static const char *const g_su_name[SU_N] = { "fopen", "fdopen", "opendir", "strdup", "strndup", "setenv", "getcwd", "realpath", "dlopen", "key_create", "sem_init", "localtime", "tzset", "mutexattr", "condattr", "asprintf", "vasprintf", "tmpfile", "scandir" };
#define SU(i) __atomic_fetch_add(&g_su[i], 1, __ATOMIC_RELAXED)
#define SU_UNCOUNT(i) __atomic_fetch_sub(&g_su[i], 1, __ATOMIC_RELAXED)
extern FILE *__real_fopen(const char *, const char *) __attribute__((weak));
extern FILE *__real_fdopen(int, const char *) __attribute__((weak));
extern DIR *__real_opendir(const char *) __attribute__((weak));
extern char *__real_strdup(const char *) __attribute__((weak));
extern char *__real_strndup(const char *, size_t) __attribute__((weak));
extern int __real_setenv(const char *, const char *, int) __attribute__((weak));
extern char *__real_getcwd(char *, size_t) __attribute__((weak));
extern char *__real_realpath(const char *, char *) __attribute__((weak));
extern void *__real_dlopen(const char *, int) __attribute__((weak));
extern int __real_pthread_key_create(pthread_key_t *, void (*)(void *)) __attribute__((weak));
extern int __real_sem_init(sem_t *, int, unsigned) __attribute__((weak));
extern struct tm *__real_localtime_r(const time_t *, struct tm *) __attribute__((weak));
extern void __real_tzset(void) __attribute__((weak));
extern int __real_pthread_mutexattr_init(pthread_mutexattr_t *) __attribute__((weak));
extern int __real_pthread_condattr_init(pthread_condattr_t *) __attribute__((weak));
FILE *__wrap_fdopen(int fd, const char *m) { SU(SU_FDOPEN); return __real_fdopen ? __real_fdopen(fd, m) : fdopen(fd, m); }
char *__wrap_strdup(const char *p) {   // 6000+ calls while the JVM starts: kept off the libc heap (the memory comes from the title's own malloc when that is built in)
    SU(SU_STRDUP);
#ifndef PS5_NO_OWNMALLOC
    size_t n = strlen(p) + 1; char *q = (char *)__wrap_malloc(n); if (q) memcpy(q, p, n); return q;
#else
    return __real_strdup ? __real_strdup(p) : strdup(p);
#endif
}
char *__wrap_strndup(const char *p, size_t n) {
    SU(SU_STRNDUP);
#ifndef PS5_NO_OWNMALLOC
    size_t l = strnlen(p, n); char *q = (char *)__wrap_malloc(l + 1); if (q) { memcpy(q, p, l); q[l] = 0; } return q;
#else
    return __real_strndup ? __real_strndup(p, n) : strndup(p, n);
#endif
}
int __wrap_setenv(const char *k, const char *v, int o) { SU(SU_SETENV); return __real_setenv ? __real_setenv(k, v, o) : setenv(k, v, o); }
char *__wrap_getcwd(char *b, size_t n) { if (!b) SU(SU_GETCWD); return __real_getcwd ? __real_getcwd(b, n) : getcwd(b, n); }
char *__wrap_realpath(const char *p, char *b) { if (!b) SU(SU_REALPATH); return __real_realpath ? __real_realpath(p, b) : realpath(p, b); }
void *__wrap_dlopen(const char *p, int f) { SU(SU_DLOPEN); return __real_dlopen ? __real_dlopen(p, f) : dlopen(p, f); }
int __wrap_pthread_key_create(pthread_key_t *k, void (*d)(void *)) { SU(SU_KEYCREATE); return __real_pthread_key_create ? __real_pthread_key_create(k, d) : pthread_key_create(k, d); }
int __wrap_sem_init(sem_t *s, int ps, unsigned v) { SU(SU_SEMINIT); return __real_sem_init ? __real_sem_init(s, ps, v) : sem_init(s, ps, v); }
struct tm *__wrap_localtime_r(const time_t *t, struct tm *r) { SU(SU_LOCALTIME); return __real_localtime_r ? __real_localtime_r(t, r) : localtime_r(t, r); }
void __wrap_tzset(void) { SU(SU_TZSET); if (__real_tzset) __real_tzset(); else tzset(); }
int __wrap_pthread_mutexattr_init(pthread_mutexattr_t *a) { SU(SU_MXATTR); return __real_pthread_mutexattr_init ? __real_pthread_mutexattr_init(a) : pthread_mutexattr_init(a); }
int __wrap_pthread_condattr_init(pthread_condattr_t *a) { SU(SU_CVATTR); return __real_pthread_condattr_init ? __real_pthread_condattr_init(a) : pthread_condattr_init(a); }
// More libc-internal allocators (Mesa calls asprintf/vasprintf all over its shader paths; the libc versions take the memory from the small libc heap, which is what drains). asprintf/vasprintf are served by the
// title's own allocator (as MihawkOS's wine/ps5/pw_wine_heap_libc.c does); the rest are only counted (getline/getdelim are already wrapped by the Vulkan template's heap_wrap.o; open_memstream must NOT be wrapped here: vk_relink.sh then drops the template's --defsym=open_memstream=ps5_open_memstream and the link fails) so the per-second "tl:" line can name them.
extern int __real_vasprintf(char **, const char *, va_list) __attribute__((weak));
extern FILE *__real_tmpfile(void) __attribute__((weak));
extern int __real_scandir(const char *, struct dirent ***, int (*)(const struct dirent *), int (*)(const struct dirent **, const struct dirent **)) __attribute__((weak));
int __wrap_vasprintf(char **out, const char *fmt, va_list ap) {
    SU(SU_VASPRINTF);
#ifndef PS5_NO_OWNMALLOC
    va_list m; va_copy(m, ap); int n = vsnprintf(0, 0, fmt, m); va_end(m); if (n < 0) { *out = 0; return -1; }
    char *q = (char *)__wrap_malloc((size_t)n + 1); if (!q) { *out = 0; return -1; }
    vsnprintf(q, (size_t)n + 1, fmt, ap); *out = q; return n;
#else
    return __real_vasprintf ? __real_vasprintf(out, fmt, ap) : (*out = 0, -1);
#endif
}
int __wrap_asprintf(char **out, const char *fmt, ...) { SU(SU_ASPRINTF); va_list ap; va_start(ap, fmt); int r = __wrap_vasprintf(out, fmt, ap); va_end(ap); SU_UNCOUNT(SU_VASPRINTF); return r; }
FILE *__wrap_tmpfile(void) { SU(SU_TMPFILE); return __real_tmpfile ? __real_tmpfile() : (FILE *)0; }
int __wrap_scandir(const char *p, struct dirent ***l, int (*f)(const struct dirent *), int (*c)(const struct dirent **, const struct dirent **)) { SU(SU_SCANDIR); return __real_scandir ? __real_scandir(p, l, f, c) : -1; }
extern char *__real_nl_langinfo(int) __attribute__((weak));
// ... and the charset comes from nl_langinfo(CODESET): the JDK 17 object still said US-ASCII (jnu=US-ASCII in the log) although setlocale answered en_US.UTF-8, so the resource packs "...\xC2\xA76..." stayed unopenable.
char *__wrap_nl_langinfo(int item) { return item == 0 /* CODESET */ ? (char *)"UTF-8" : (__real_nl_langinfo ? __real_nl_langinfo(item) : (char *)""); }

extern int __real_mkdir(const char *, mode_t) __attribute__((weak));
int __wrap_mkdir(const char *p, mode_t m) { VP; return __real_mkdir ? __real_mkdir(p, m) : mkdir(p, m); }
extern int __real_stat(const char *, struct stat *) __attribute__((weak));
int __wrap_stat(const char *p, struct stat *st) { VP; return __real_stat ? __real_stat(p, st) : stat(p, st); }
extern int __real_access(const char *, int) __attribute__((weak));
int __wrap_access(const char *p, int m) { VP; return __real_access ? __real_access(p, m) : access(p, m); }
extern int __real_unlink(const char *) __attribute__((weak));
int __wrap_unlink(const char *p) { VP; return __real_unlink ? __real_unlink(p) : unlink(p); }
extern int __real_rmdir(const char *) __attribute__((weak));
int __wrap_rmdir(const char *p) { VP; return __real_rmdir ? __real_rmdir(p) : rmdir(p); }
extern int __real_rename(const char *, const char *) __attribute__((weak));
int __wrap_rename(const char *p, const char *q) { char vc_[1100]; q = vpath(q, vc_, sizeof vc_); VP; return __real_rename ? __real_rename(p, q) : rename(p, q); }
extern FILE *__real_fopen(const char *, const char *) __attribute__((weak));
FILE *__wrap_fopen(const char *p, const char *m) { VP; SU(SU_FOPEN); return __real_fopen ? __real_fopen(p, m) : fopen(p, m); }
// Thread-local storage fallback. HotSpot keeps Thread::current() in a pthread key. Once the libc pool is dry (log 2efa7c7f), pthread_setspecific fails (ENOMEM) for a new thread, HotSpot ignores
// the error, and the thread faults at once on Thread::current()==NULL (addr 0x238). A failed set is kept here, keyed by thread and key, and pthread_getspecific looks here when the libc has nothing.
extern int __real_pthread_setspecific(pthread_key_t, const void *) __attribute__((weak));
extern void *__real_pthread_getspecific(pthread_key_t) __attribute__((weak));
#define SS_N 256
static struct { volatile uintptr_t self; pthread_key_t key; void *volatile val; } g_ss[SS_N];
static volatile int g_ss_n, g_ss_fail;
static void ss_del(uintptr_t me, pthread_key_t k) { int i; for (i = 0; i < SS_N; i++) if (g_ss[i].self == me && g_ss[i].key == k) { g_ss[i].self = 0; __sync_fetch_and_sub(&g_ss_n, 1); } }
int __wrap_pthread_setspecific(pthread_key_t k, const void *v) {
    int rc = __real_pthread_setspecific ? __real_pthread_setspecific(k, v) : pthread_setspecific(k, v); uintptr_t me = (uintptr_t)pthread_self(); int i;
    if (rc == 0) { if (g_ss_n > 0) ss_del(me, k); return 0; }
    if (!v) { ss_del(me, k); return 0; }
    for (i = 0; i < SS_N; i++) if (g_ss[i].self == me && g_ss[i].key == k) { g_ss[i].val = (void *)v; return 0; }
    for (i = 0; i < SS_N; i++) if (__sync_bool_compare_and_swap(&g_ss[i].self, 0, me)) {
        g_ss[i].key = k; g_ss[i].val = (void *)v; __sync_fetch_and_add(&g_ss_n, 1);
        if (__sync_fetch_and_add(&g_ss_fail, 1) < 4) TR("threads: pthread_setspecific rc=%d (libc pool dry): the value of key %d for this thread is kept by the shim", rc, (int)k);
        return 0;
    }
    return rc;
}
void *__wrap_pthread_getspecific(pthread_key_t k) {
    void *v = __real_pthread_getspecific ? __real_pthread_getspecific(k) : pthread_getspecific(k);
    if (!v && g_ss_n > 0) { uintptr_t me = (uintptr_t)pthread_self(); int i; for (i = 0; i < SS_N; i++) if (g_ss[i].self == me && g_ss[i].key == k) return g_ss[i].val; }
    return v;
}
static volatile int g_at_fail, g_reg_n, g_at_n;   // g_reg_n: thread registry count (defined below)
// Attributes the libc could not back (pool full): the stand-in handed out by pthread_attr_init (kind 1, zeroed) or one whose pthread_attr_get_np failed (kind 2). Keyed by address (they live on the thread's stack).
static void *volatile g_fb_a[32]; static volatile int g_fb_k[32];
static void fb_add(void *a, int k) { for (int i = 0; i < 32; i++) if (__sync_bool_compare_and_swap(&g_fb_a[i], (void *)0, a)) { g_fb_k[i] = k; return; } }
static int fb_kind(const void *a) { for (int i = 0; i < 32; i++) if (g_fb_a[i] == a) return g_fb_k[i]; return 0; }
static void fb_del(const void *a) { for (int i = 0; i < 32; i++) if (g_fb_a[i] == a) { g_fb_a[i] = 0; return; } }
// Reserve of REAL attributes made at start-up while the libc still has room: a thread made from a stand-in attribute (default stack, no cpuset) overflowed the stack HotSpot assumed for it (log bfd392bf: SIGSEGV
// inside the stack, outside the yellow zone). A reserve attribute behaves like any other, so the thread gets its full requested stack.
#define AT_RES 96
static pthread_attr_t g_at_res[AT_RES]; static volatile int g_at_res_n, g_at_used;
__attribute__((weak)) int __wrap_pthread_attr_init(pthread_attr_t *a) {
    int rc, k;
    __sync_fetch_and_add(&g_at_n, 1); if (a) fb_del(a);
    if (a) memset((void *)a, 0, sizeof *a);
    rc = __real_pthread_attr_init ? __real_pthread_attr_init(a) : pthread_attr_init(a);
    // Logs 5efc5bc1 / fddf54d3 / 08ad4da3: after 1.5-2.5 minutes the console's pthread_attr_init returned ENOMEM (12) and HotSpot aborted ("pthread_attr_init failed with error = 12"): the libc's pool for pthread
    // objects is full. Retry (threads that exit free slots), and as a last resort hand back a zeroed attribute (HotSpot only reads the stack bounds from it).
    for (k = 0; rc != 0 && k < 50; k++) { usleep(10000); if (a) memset((void *)a, 0, sizeof *a); rc = __real_pthread_attr_init ? __real_pthread_attr_init(a) : pthread_attr_init(a); }
    if (rc != 0 && sizeof *a == sizeof(void *)) { int i = __sync_fetch_and_add(&g_at_used, 1); if (i < g_at_res_n) { memcpy((void *)a, (void *)&g_at_res[i], sizeof *a); rc = 0; if (i < 3 || i == AT_RES - 1) TR("threads: pthread_attr_init rc=12: reserve attribute #%d of %d handed out (a real one, made at start-up)", i + 1, AT_RES); } }
    if (rc != 0) { int f = __sync_fetch_and_add(&g_at_fail, 1); if (f < 4) { TR("threads: pthread_attr_init rc=%d after retries (mutexes alive %d shared %d, threads registered %d): zeroed attribute returned", rc, g_mx_real - g_mx_realdel, g_mx_shared, g_reg_n); if (f == 0 || f == 3) { mx_dump("a pthread_attr_init failure"); enomem_probe("pthread_attr_init failure"); } } if (a) memset((void *)a, 0, sizeof *a); if (a) fb_add(a, 1); return 0; }
    return 0;
}
// Logs f9638ec0 / 8e2dfd75: the libc ran out of pthread objects (always at ~1,700 live mutexes / ~44 threads), pthread_attr_init or pthread_attr_get_np failed and HotSpot died in the new thread's start-up
// ("pthread_attr_get_np failed with error = 22" / "Can not locate current stack attributes!", os_bsd_x86.cpp:773). HotSpot only needs the stack bounds from that attribute, and the thread already exists, so they
// are answered from the thread's own stack instead (top = just above the current frame, 512 KB below it: smaller than every HotSpot stack, so a stack check can only trigger early, never late).
extern int pthread_attr_get_np(pthread_t, pthread_attr_t *);
extern int __real_pthread_attr_get_np(pthread_t, pthread_attr_t *) __attribute__((weak));
extern int __real_pthread_attr_getstack(const pthread_attr_t *, void **, size_t *) __attribute__((weak));
extern int __real_pthread_attr_getstackaddr(const pthread_attr_t *, void **) __attribute__((weak));
extern int __real_pthread_attr_getstacksize(const pthread_attr_t *, size_t *) __attribute__((weak));
extern int __real_pthread_attr_destroy(pthread_attr_t *) __attribute__((weak));
static void fb_bounds(void **bottom, size_t *n) {
    static int logged; unsigned long long top = ((unsigned long long)__builtin_frame_address(0) + 65536ull) & ~16383ull; *n = 512u << 10; *bottom = (void *)(top - *n);
    if (logged++ < 3) TR("threads: the libc has no pthread object left for this thread's attribute: stack bounds taken from its own stack (top %llx, %zu KB)", top, *n >> 10);
}
int __wrap_pthread_attr_get_np(pthread_t t, pthread_attr_t *a) {
    int rc;
    if (a && t == pthread_self()) {
        if (fb_kind(a)) return 0;
        rc = __real_pthread_attr_get_np ? __real_pthread_attr_get_np(t, a) : pthread_attr_get_np(t, a);
        if (rc) { fb_add(a, 2); return 0; }
        return 0;
    }
    return __real_pthread_attr_get_np ? __real_pthread_attr_get_np(t, a) : pthread_attr_get_np(t, a);
}
int __wrap_pthread_attr_getstack(const pthread_attr_t *a, void **addr, size_t *sz) {
    if (a && fb_kind(a)) { fb_bounds(addr, sz); return 0; }
    return __real_pthread_attr_getstack ? __real_pthread_attr_getstack(a, addr, sz) : pthread_attr_getstack(a, addr, sz);
}
int __wrap_pthread_attr_getstackaddr(const pthread_attr_t *a, void **addr) {
    if (a && fb_kind(a)) { size_t n; fb_bounds(addr, &n); return 0; }
    return __real_pthread_attr_getstackaddr ? __real_pthread_attr_getstackaddr(a, addr) : pthread_attr_getstackaddr(a, addr);
}
int __wrap_pthread_attr_getstacksize(const pthread_attr_t *a, size_t *sz) {
    if (a && fb_kind(a)) { void *b; fb_bounds(&b, sz); return 0; }
    return __real_pthread_attr_getstacksize ? __real_pthread_attr_getstacksize(a, sz) : pthread_attr_getstacksize(a, sz);
}
int __wrap_pthread_attr_destroy(pthread_attr_t *a) {
    int k = a ? fb_kind(a) : 0;
    if (k) fb_del(a);
    if (k == 1) return 0;   // the zeroed stand-in owns nothing
    return __real_pthread_attr_destroy ? __real_pthread_attr_destroy(a) : pthread_attr_destroy(a);
}
// The same stand-in handed to pthread_create (the wait above gave up): use default attributes instead. Weak: a pthread_create wrapper of the boilerplate wins if there is one.
extern int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *) __attribute__((weak));
static volatile int g_pc_n;   // pthread_create calls so far (the "pool:" line prints it next to the libc heap: does each new thread cost it a piece?)
__attribute__((weak)) int __wrap_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*f)(void *), void *arg) {
    __sync_fetch_and_add(&g_pc_n, 1);
    if (a && fb_kind(a) == 1) { int rc = __real_pthread_create ? __real_pthread_create(t, 0, f, arg) : pthread_create(t, 0, f, arg); if (rc == 0) pthread_detach(*t); return rc; }
    return __real_pthread_create ? __real_pthread_create(t, a, f, arg) : pthread_create(t, a, f, arg);
}
// Conditions fail the same way and HotSpot ignores the result (the junk left in the condition then hangs or faults at its first wait): 96 are made at start-up, while the pool is still wide open
// (ps5_calib), and handed out when pthread_cond_init fails. A condition is a single libc handle here; destroying a handed-out one gives its slot back to the pool like any other.
extern int __real_pthread_cond_init(pthread_cond_t *, const pthread_condattr_t *) __attribute__((weak));
#define CV_RES 96
static pthread_cond_t g_cv_res[CV_RES]; static volatile int g_cv_res_n, g_cv_used;
static int cv_real_init(pthread_cond_t *c, const pthread_condattr_t *at) { return __real_pthread_cond_init ? __real_pthread_cond_init(c, at) : pthread_cond_init(c, at); }
int __wrap_pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *at) {
    int rc = cv_real_init(c, at);
    if (rc && sizeof *c == sizeof(void *)) { int i = __sync_fetch_and_add(&g_cv_used, 1); if (i < g_cv_res_n) { memcpy((void *)c, (void *)&g_cv_res[i], sizeof *c); if (i < 3 || i == CV_RES - 1) TR("threads: pthread_cond_init rc=%d: reserve condition #%d of %d handed out", rc, i + 1, g_cv_res_n); return 0; } }
    return rc;
}
// Probe: is the pool still open? One mutex, cond and attr made and freed again, every report tick; the first tick where one fails is the cliff, printed with what the wrappers counted by then
// (mutexes alive / made, attributes made, threads seen), so the next log says whether it is a count, the time, or something that grows.
#define REG_N 256
static volatile int g_reg_free[REG_N]; static int g_reg_printed;   // libc heap free pieces (x256 KB) when each registered thread started, printed once by pool_probe
static void pool_probe(void) {
    static int cliff; int rm, rc2, ra; pthread_mutex_t m; pthread_cond_t c; pthread_attr_t at; memset((void *)&m, 0, sizeof m); memset((void *)&c, 0, sizeof c); memset((void *)&at, 0, sizeof at);
    rm = mx_real_init(&m); if (!rm) { if (__real_pthread_mutex_destroy) __real_pthread_mutex_destroy(&m); else pthread_mutex_destroy(&m); }
    rc2 = cv_real_init(&c, 0); if (!rc2) pthread_cond_destroy(&c);
    ra = __real_pthread_attr_init ? __real_pthread_attr_init(&at) : pthread_attr_init(&at); if (!ra) { if (__real_pthread_attr_destroy) __real_pthread_attr_destroy(&at); else pthread_attr_destroy(&at); }
    TR("pool: pthread_create calls %d | mutex alive %d (made %d, destroyed %d, shared %d) | attr made %d, init failures %d | threads seen %d | thread-local fallback: %d failed sets, %d live | can still make: mutex rc %d, cond rc %d, attr rc %d", g_pc_n, g_mx_real - g_mx_realdel, g_mx_n, g_mx_destroyed, g_mx_shared, g_at_n, g_at_fail, g_reg_n, g_ss_fail, g_ss_n, rm, rc2, ra);
    { int fp = libc_free_pieces(), n = g_reg_n < REG_N ? g_reg_n : REG_N, o = 0; char tb[300]; tb[0] = 0;
      for (; g_reg_printed < n && o < (int)sizeof tb - 24; g_reg_printed++) o += snprintf(tb + o, sizeof tb - o, " #%d:%d", g_reg_printed + 1, g_reg_free[g_reg_printed]);
      TR("libcfree: %d x256K pieces free now (%d MB); free pieces when threads registered:%s", fp, fp / 4, o ? tb : " (no new thread)");
      { static int churn_done; if (!churn_done && fp >= 30) { churn_done = 1; int before = libc_free_pieces(), k, ok = 0; long pc0 = g_pc_n;   /* thread churn probe: does creating and ending threads cost the libc heap? 60 short threads, one after the other */
          for (k = 0; k < 60; k++) { pthread_t th; if (pthread_create(&th, 0, churn_fn, 0) == 0) { pthread_join(th, 0); ok++; } }
          { int after = libc_free_pieces(); TR("threadchurn: %d threads created and joined: libc free pieces %d -> %d (a thread that leaks costs ~1 piece per 1-4 threads); pthread_create wrapper calls +%ld", ok, before, after, (long)g_pc_n - pc0); } } }
      if (malloc_stats_fast) { size_t m[5] = { 0, 0, 0, 0, 0 }; int r = malloc_stats_fast(m); TR("libcstats: rc %d size %zu maxSys %zu curSys %zu maxInuse %zu curInuse %zu (bytes; in use growing = a leak in a libc-internal allocation, in use flat but pieces falling = fragmentation)", r, m[0], m[1], m[2], m[3], m[4]); }
    }
    if ((rm || rc2 || ra) && !cliff++) { TR("pool: CLIFF reached here (mutex rc %d, cond rc %d, attr rc %d)", rm, rc2, ra); libc_heap_probe("cliff"); }
}
// ---- start-up calibration of the console libc's pthread-object budget (probe, rev ctl5). Run ONCE by jvm_host before the VM thread exists (nothing else creates pthread objects then): how many mutexes / conds /
// attributes / threads can be made until the libc says ENOMEM or EAGAIN, whether destroying them gives the capacity back, and whether 400 threads created and joined one after another use up anything for good.
// The result is printed with the 25th-thread marker (earlier TR lines are lost). It answers "which pool runs out at ~50-150 s in every game with many threads" without guessing.
#define CAL_MAX 30000
static char g_cal_a[190], g_cal_b[190], g_cal_c[190], g_cal_d[190], g_cal_e[200];
static void *cal_idle(void *p) { while (!*(volatile int *)p) usleep(1000); return 0; }
static void *cal_quick(void *p) { (void)p; return 0; }
static int cal_mx(int *rc_out) { pthread_mutex_t *a = (pthread_mutex_t *)calloc(CAL_MAX, sizeof *a); int n = 0, rc = 0; if (!a) return -1;
    while (n < CAL_MAX) { rc = mx_real_init(&a[n]); if (rc) break; n++; }
    for (int i = 0; i < n; i++) { if (__real_pthread_mutex_destroy) __real_pthread_mutex_destroy(&a[i]); else pthread_mutex_destroy(&a[i]); }
    free(a); *rc_out = rc; return n; }
static int cal_cv(int *rc_out) { pthread_cond_t *a = (pthread_cond_t *)calloc(CAL_MAX, sizeof *a); int n = 0, rc = 0; if (!a) return -1;
    while (n < CAL_MAX) { rc = pthread_cond_init(&a[n], 0); if (rc) break; n++; }
    for (int i = 0; i < n; i++) pthread_cond_destroy(&a[i]);
    free(a); *rc_out = rc; return n; }
static int cal_at(int *rc_out) { pthread_attr_t *a = (pthread_attr_t *)calloc(CAL_MAX, sizeof *a); int n = 0, rc = 0; if (!a) return -1;
    while (n < CAL_MAX) { memset((void *)&a[n], 0, sizeof a[n]); rc = __real_pthread_attr_init ? __real_pthread_attr_init(&a[n]) : pthread_attr_init(&a[n]); if (rc) break; n++; }
    for (int i = 0; i < n; i++) pthread_attr_destroy(&a[i]);
    free(a); *rc_out = rc; return n; }
static int cal_mixed(int *m, int *c, int *t, int *which) {   // one of each, round after round, until any of the three fails
    pthread_mutex_t *pm = (pthread_mutex_t *)calloc(CAL_MAX, sizeof *pm); pthread_cond_t *pc = (pthread_cond_t *)calloc(CAL_MAX, sizeof *pc); pthread_attr_t *pt = (pthread_attr_t *)calloc(CAL_MAX, sizeof *pt);
    int n = 0, nm = 0, nc = 0, nt = 0, w = 0;
    if (pm && pc && pt) for (; n < CAL_MAX; n++) {
        if (mx_real_init(&pm[n])) { w = 1; break; } nm++;
        if (pthread_cond_init(&pc[n], 0)) { w = 2; break; } nc++;
        memset((void *)&pt[n], 0, sizeof pt[n]); if (__real_pthread_attr_init ? __real_pthread_attr_init(&pt[n]) : pthread_attr_init(&pt[n])) { w = 3; break; } nt++;
    }
    for (int i = 0; i < nm; i++) { if (__real_pthread_mutex_destroy) __real_pthread_mutex_destroy(&pm[i]); else pthread_mutex_destroy(&pm[i]); }
    for (int i = 0; i < nc; i++) pthread_cond_destroy(&pc[i]);
    for (int i = 0; i < nt; i++) pthread_attr_destroy(&pt[i]);
    free(pm); free(pc); free(pt); *m = nm; *c = nc; *t = nt; *which = w; return n; }
static int cal_threads(int cap, int *rc_out) {   // live threads that stay parked until all are made
    volatile int stop = 0; pthread_t *t = (pthread_t *)calloc(cap, sizeof *t); int n = 0, rc = 0; pthread_attr_t at; if (!t) return -1;
    while (n < cap) {
        memset((void *)&at, 0, sizeof at); rc = __real_pthread_attr_init ? __real_pthread_attr_init(&at) : pthread_attr_init(&at); if (rc) { rc += 1000; break; }
        __real_pthread_attr_setstacksize(&at, 128 << 10); rc = pthread_create(&t[n], &at, cal_idle, (void *)&stop); pthread_attr_destroy(&at); if (rc) break; n++;
    }
    stop = 1; for (int i = 0; i < n; i++) pthread_join(t[i], 0);
    free(t); *rc_out = rc; return n; }
// ---- libc heap leak hunt (log dbd4d9be: the libc heap drains ~1.4 MB per 5 s during play and is empty after ~90 s). Each primitive the game uses constantly is run N times here at start-up and the libc heap's free
// space is read before and after (16 KB resolution, 32 MB cap): a primitive that leaks shows as B/op. Results are kept and printed with the 25th-thread marker ("leak:" lines).
#include <dirent.h>
#include <sys/socket.h>
static char g_leak[32][130]; static int g_leak_n;
static int lk_kb(void) { if (!__real_malloc || !__real_free) return 0; static void *v[2048]; int n = 0, i; while (n < 2048 && (v[n] = __real_malloc(16u << 10))) n++; for (i = 0; i < n; i++) __real_free(v[i]); return n * 16; }
#define LK(NAME, COUNT, STMT) do { int lb_ = lk_kb(); for (int i_ = 0; i_ < (COUNT); i_++) { STMT; } int la_ = lk_kb(); if (g_leak_n < 32) snprintf(g_leak[g_leak_n++], sizeof g_leak[0], "%-22s x%-5d libc heap %+d KB = %.1f B/op", NAME, (COUNT), la_ - lb_, (double)(lb_ - la_) * 1024.0 / (COUNT)); } while (0)
extern int sceKernelCreateEqueue(void *, const char *) __attribute__((weak)); extern int sceKernelDeleteEqueue(void *) __attribute__((weak));
extern int sceKernelCreateSema(void *, const char *, unsigned, int, int, const void *) __attribute__((weak)); extern int sceKernelDeleteSema(void *) __attribute__((weak));
extern int sceKernelCreateEventFlag(void *, const char *, unsigned, unsigned long long, const void *) __attribute__((weak)); extern int sceKernelDeleteEventFlag(void *) __attribute__((weak));
extern int sceKernelUsleep(unsigned) __attribute__((weak));
static void *lk_quick(void *p) { (void)p; return 0; }
static volatile int g_leak_done;
// Blocking paths: parking a thread (cond wait, semaphore wait, contended mutex) is what the JVM does thousands of times a second. The log 12c7d196 showed the pool shrinking while every thread sat parked, and none of the
// primitives above ever blocks, so these run two threads against each other and read the libc heap before and after.
static struct { pthread_mutex_t m, m2; pthread_cond_t c; sem_t s1, s2; volatile int turn, stop; int n; } g_lkp;
static void *lk_cond_helper(void *p) { (void)p; for (int i = 0; i < g_lkp.n; i++) { pthread_mutex_lock(&g_lkp.m); while (g_lkp.turn != 1) pthread_cond_wait(&g_lkp.c, &g_lkp.m); g_lkp.turn = 0; pthread_cond_broadcast(&g_lkp.c); pthread_mutex_unlock(&g_lkp.m); } return 0; }
static void *lk_sem_helper(void *p) { (void)p; for (int i = 0; i < g_lkp.n; i++) { sem_wait(&g_lkp.s1); sem_post(&g_lkp.s2); } return 0; }
static void *lk_mx_helper(void *p) { (void)p; for (int i = 0; i < g_lkp.n; i++) { pthread_mutex_lock(&g_lkp.m2); for (volatile int k = 0; k < 200; k++) { } pthread_mutex_unlock(&g_lkp.m2); } return 0; }
static void *lk_tw_helper(void *p) { (void)p; for (int i = 0; i < g_lkp.n; i++) { usleep(100); pthread_mutex_lock(&g_lkp.m); g_lkp.turn = 1; pthread_cond_signal(&g_lkp.c); pthread_mutex_unlock(&g_lkp.m); } return 0; }

static void *ps5_leaktest_body(void *unused) {
    (void)unused; char b[128]; struct timespec ts; pthread_mutex_t m; pthread_cond_t c; pthread_attr_t at; pthread_key_t k; struct stat st;
    LK("snprintf %.2f", 3000, snprintf(b, sizeof b, "%.2f %d", i_ * 0.37, i_));
    LK("clock_gettime", 5000, clock_gettime(CLOCK_MONOTONIC, &ts));
    LK("usleep(50)", 300, usleep(50));
    LK("mutex init/destroy", 3000, { memset((void *)&m, 0, sizeof m); if (!mx_real_init(&m)) { if (__real_pthread_mutex_destroy) __real_pthread_mutex_destroy(&m); else pthread_mutex_destroy(&m); } });
    LK("cond init/destroy", 3000, { memset((void *)&c, 0, sizeof c); if (!cv_real_init(&c, 0)) pthread_cond_destroy(&c); });
    LK("cond timedwait 0.1ms", 300, { memset((void *)&m, 0, sizeof m); memset((void *)&c, 0, sizeof c); if (!mx_real_init(&m) && !cv_real_init(&c, 0)) { clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 100000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; } pthread_mutex_lock(&m); pthread_cond_timedwait(&c, &m, &ts); pthread_mutex_unlock(&m); pthread_cond_destroy(&c); pthread_mutex_destroy(&m); } });
    LK("attr init/destroy", 3000, { memset((void *)&at, 0, sizeof at); if (!(__real_pthread_attr_init ? __real_pthread_attr_init(&at) : pthread_attr_init(&at))) pthread_attr_destroy(&at); });
    LK("key create+set/get", 1000, { if (!pthread_key_create(&k, 0)) { pthread_setspecific(k, &b); (void)pthread_getspecific(k); pthread_key_delete(k); } });
    LK("open/close /dev/null", 2000, { int fd = open("/dev/null", O_RDONLY); if (fd >= 0) close(fd); });
    LK("fopen/fclose", 500, { FILE *f = fopen("/dev/null", "r"); if (f) fclose(f); });
    LK("stat", 2000, stat("/app0", &st));
    LK("opendir/readdir/close", 200, { DIR *d = opendir("/app0"); if (d) { while (readdir(d)) { } closedir(d); } });
    LK("mmap/munmap 64K", 2000, { void *p = mmap(0, 65536, 3, MAP_ANON | MAP_PRIVATE, -1, 0); if (p != MAP_FAILED) munmap(p, 65536); });
    LK("getenv", 3000, (void)getenv("HOME"));
    LK("socket(UDP)+close", 100, { int sk = socket(AF_INET, SOCK_DGRAM, 0); if (sk >= 0) close(sk); });
    LK("sched_yield", 3000, sched_yield());
    { int sk = socket(2, SOCK_DGRAM, 0); if (sk >= 0) { unsigned char sa[16]; memset(sa, 0, sizeof sa); sa[0] = 16; sa[1] = 2; sa[3] = 9; sa[4] = 127; sa[7] = 1;   /* sockaddr_in by hand (BSD layout): 127.0.0.1:9 */
        LK("sendto UDP loopback", 1000, sendto(sk, "leaktest", 8, 0, (struct sockaddr *)sa, sizeof sa)); close(sk); } }
    LK("thread create+join", 150, { pthread_t x; if (!pthread_create(&x, 0, lk_quick, 0)) pthread_join(x, 0); });
    { pthread_t h; memset((void *)&g_lkp, 0, sizeof g_lkp); mx_real_init(&g_lkp.m); mx_real_init(&g_lkp.m2); cv_real_init(&g_lkp.c, 0);
      g_lkp.n = 1500; int lb_ = lk_kb(); if (!pthread_create(&h, 0, lk_cond_helper, 0)) { for (int i = 0; i < 1500; i++) { pthread_mutex_lock(&g_lkp.m); g_lkp.turn = 1; pthread_cond_broadcast(&g_lkp.c); while (g_lkp.turn != 0) pthread_cond_wait(&g_lkp.c, &g_lkp.m); pthread_mutex_unlock(&g_lkp.m); } pthread_join(h, 0); }
      int la_ = lk_kb(); if (g_leak_n < 32) snprintf(g_leak[g_leak_n++], sizeof g_leak[0], "%-22s x%-5d libc heap %+d KB = %.1f B/op", "cond wait ping-pong", 1500, la_ - lb_, (double)(lb_ - la_) * 1024.0 / 1500); }
    { pthread_t h; sem_init(&g_lkp.s1, 0, 0); sem_init(&g_lkp.s2, 0, 0); g_lkp.n = 1500; int lb_ = lk_kb(); if (!pthread_create(&h, 0, lk_sem_helper, 0)) { for (int i = 0; i < 1500; i++) { sem_post(&g_lkp.s1); sem_wait(&g_lkp.s2); } pthread_join(h, 0); }
      int la_ = lk_kb(); if (g_leak_n < 32) snprintf(g_leak[g_leak_n++], sizeof g_leak[0], "%-22s x%-5d libc heap %+d KB = %.1f B/op", "sem wait ping-pong", 1500, la_ - lb_, (double)(lb_ - la_) * 1024.0 / 1500); sem_destroy(&g_lkp.s1); sem_destroy(&g_lkp.s2); }
    { pthread_t h; g_lkp.n = 4000; int lb_ = lk_kb(); if (!pthread_create(&h, 0, lk_mx_helper, 0)) { for (int i = 0; i < 4000; i++) { pthread_mutex_lock(&g_lkp.m2); for (volatile int k = 0; k < 200; k++) { } pthread_mutex_unlock(&g_lkp.m2); } pthread_join(h, 0); }
      int la_ = lk_kb(); if (g_leak_n < 32) snprintf(g_leak[g_leak_n++], sizeof g_leak[0], "%-22s x%-5d libc heap %+d KB = %.1f B/op", "mutex contended", 4000, la_ - lb_, (double)(lb_ - la_) * 1024.0 / 4000); }
    { pthread_t h; g_lkp.n = 300; g_lkp.turn = 0; int lb_ = lk_kb(); if (!pthread_create(&h, 0, lk_tw_helper, 0)) { for (int i = 0; i < 300; i++) { struct timespec t2; clock_gettime(CLOCK_REALTIME, &t2); t2.tv_nsec += 5000000; if (t2.tv_nsec >= 1000000000) { t2.tv_sec++; t2.tv_nsec -= 1000000000; } pthread_mutex_lock(&g_lkp.m); while (g_lkp.turn != 1) if (pthread_cond_timedwait(&g_lkp.c, &g_lkp.m, &t2)) break; g_lkp.turn = 0; pthread_mutex_unlock(&g_lkp.m); } pthread_join(h, 0); }
      int la_ = lk_kb(); if (g_leak_n < 32) snprintf(g_leak[g_leak_n++], sizeof g_leak[0], "%-22s x%-5d libc heap %+d KB = %.1f B/op", "cond timedwait signalled", 300, la_ - lb_, (double)(lb_ - la_) * 1024.0 / 300); }
    if (g_pad >= 0) { PadState ps_; LK("scePadReadState", 2000, scePadReadState(g_pad, &ps_)); }
    LK("nanosleep 50us", 300, { struct timespec q; q.tv_sec = 0; q.tv_nsec = 50000; nanosleep(&q, 0); });
    if (sceKernelUsleep) LK("sceKernelUsleep(50)", 300, sceKernelUsleep(50));
    if (sceKernelCreateEqueue && sceKernelDeleteEqueue) LK("equeue create/delete", 300, { void *q = 0; if (sceKernelCreateEqueue(&q, "lk") == 0) sceKernelDeleteEqueue(q); });
    if (sceKernelCreateSema && sceKernelDeleteSema) LK("kernel sema c/d", 300, { void *sm = 0; if (sceKernelCreateSema(&sm, "lk", 0, 0, 1, 0) == 0) sceKernelDeleteSema(sm); });
    g_leak_done = 1; return 0;
}
// Runs the test on its own thread with a 8 s limit, and only if the previous attempt did not die inside it (marker files): a crash or hang here must never stop the title from starting.
static void ps5_leaktest(void) {
    const char *try_f = "/app0/mc/.leak_try", *ok_f = "/app0/mc/.leak_ok"; struct stat st;
    if (stat(try_f, &st) == 0 && stat(ok_f, &st) != 0) { snprintf(g_leak[g_leak_n++], sizeof g_leak[0], "SKIPPED: the previous start died inside the leak test (delete /app0/mc/.leak_try to run it again)"); return; }
    unlink(ok_f); { int fd = open(try_f, O_WRONLY | O_CREAT, 0644); if (fd >= 0) close(fd); }
    pthread_t th; if (pthread_create(&th, 0, ps5_leaktest_body, 0) == 0) { pthread_detach(th); for (int w = 0; w < 800 && !g_leak_done; w++) usleep(10000); }
    if (g_leak_done) { int fd = open(ok_f, O_WRONLY | O_CREAT, 0644); if (fd >= 0) close(fd); unlink(try_f); }
    else if (g_leak_n < 32) snprintf(g_leak[g_leak_n++], sizeof g_leak[0], "TIMEOUT: the leak test did not finish in 8 s (stuck in a call; its marker stays)");
}
void ps5_calib(void) {
#if defined(PS5_VK) && !defined(PS5_NO_OWNMALLOC) && !defined(PS5_NO_RTLDHEAP)
    rtld_heap_install();
#endif
    static int done; if (done) return; done = 1;
    int r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0, r7 = 0, r8 = 0, r9 = 0, m = 0, c = 0, t = 0, w = 0;
    int mx1 = cal_mx(&r1), cv1 = cal_cv(&r2), at1 = cal_at(&r3);
    int mix = cal_mixed(&m, &c, &t, &w);
    int me = 0, mh = map_headroom(100000, &me); char lb[200]; limits_text(lb, sizeof lb);
    int th = cal_threads(200, &r4);
    int fails = 0, frc = 0; for (int i = 0; i < 400; i++) { pthread_t x; int rc = pthread_create(&x, 0, cal_quick, 0); if (rc) { if (!fails) frc = rc; fails++; usleep(1000); continue; } pthread_join(x, 0); }
    int mx2 = cal_mx(&r5), cv2 = cal_cv(&r6), at2 = cal_at(&r7), th2 = cal_threads(200, &r8); (void)r9;
    snprintf(g_cal_a, sizeof g_cal_a, "calib 1/3 (cap %d, rc 0 = cap reached): max mutex %d rc %d | cond %d rc %d | attr %d rc %d", CAL_MAX, mx1, r1, cv1, r2, at1, r3);
    snprintf(g_cal_b, sizeof g_cal_b, "calib 2/3: mixed round-robin ends at %d rounds (mutex %d cond %d attr %d, first failure: %s) | live threads %d rc %d (1000+ = attr_init)", mix, m, c, t, w == 1 ? "mutex" : w == 2 ? "cond" : w == 3 ? "attr" : "none", th, r4);
    snprintf(g_cal_d, sizeof g_cal_d, "calib start: %d separate mappings possible (cap 100000, errno %d), page %d B | %s", mh, me, getpagesize(), lb);
    fd_text(g_cal_e, sizeof g_cal_e, 1);
    snprintf(g_cal_c, sizeof g_cal_c, "calib 3/3 after 400 create+join: %d create failures (rc %d) | again: mutex %d rc %d cond %d rc %d attr %d rc %d threads %d rc %d", fails, frc, mx2, r5, cv2, r6, at2, r7, th2, r8);
    if (sizeof(pthread_mutex_t) == sizeof(void *)) { int n = 0; while (n < MX_RES && mx_real_init(&g_mx_res[n]) == 0) n++; g_mx_res_n = n; }   // the reserve of mutexes (see __wrap_pthread_mutex_init)
    if (sizeof(pthread_attr_t) == sizeof(void *)) { int n = 0; while (n < AT_RES) { memset((void *)&g_at_res[n], 0, sizeof g_at_res[n]); if ((__real_pthread_attr_init ? __real_pthread_attr_init(&g_at_res[n]) : pthread_attr_init(&g_at_res[n])) != 0) break; n++; } g_at_res_n = n; }   // the reserve of attributes (see __wrap_pthread_attr_init)
    if (sizeof(pthread_cond_t) == sizeof(void *)) { int n = 0; while (n < CV_RES && cv_real_init(&g_cv_res[n], 0) == 0) n++; g_cv_res_n = n; }   // the reserve of conditions (see __wrap_pthread_cond_init)
    ps5_leaktest();   // after the reserves: the reserves keep their objects, the test measures only its own
}
int __wrap_pthread_attr_setstacksize(pthread_attr_t *a, size_t sz) {   // log e9b8dc24: an attribute that is still the zeroed stand-in here is about to create a thread, and a thread made from it never ran (cpuset 0, "NO ANSWER ... never scheduled"): the VM then waited for it
    // forever at a handshake. Wait up to 2 s for the libc to have a pthread object again (threads exit, attributes are destroyed all the time) and make the real attribute in place.
    if (a && fb_kind(a) == 1) { int k, rc = -1; for (k = 0; k < 100 && rc; k++) { memset((void *)a, 0, sizeof *a); rc = __real_pthread_attr_init ? __real_pthread_attr_init(a) : pthread_attr_init(a); if (rc) usleep(20000); }
        { static int lg; if (lg++ < 4) TR("threads: thread attribute for a new thread was a stand-in: %s after %d tries", rc ? "still no pthread object (the thread is made with default attributes)" : "real attribute made", k); }
        if (rc == 0) fb_del(a); else memset((void *)a, 0, sizeof *a); }   // the VM calls this on every thread attribute it builds, right before pthread_create (the title's own pthread_create wrapper stays untouched)
    { static unsigned calls; calls++; if (calls == 25 || calls == 250 || calls == 1000 || calls == 2000) mx_dump("a thread-attribute checkpoint");
      if (calls == 25 || calls == 250 || calls == 1000) libc_heap_probe(calls == 25 ? "25th thread" : calls == 250 ? "250th thread" : "1000th thread");
      if (calls == 25) for (int q = 0; q < g_leak_n; q++) TR("leak: %s", g_leak[q]);
      if (calls == 25) { if (g_cal_a[0]) { TR("threads: %s", g_cal_a); TR("threads: %s", g_cal_b); TR("threads: %s", g_cal_c); TR("threads: %s", g_cal_d); TR("threads: %s", g_cal_e); } else TR("threads: calibration did not run (jvm_host does not call ps5_calib)"); }
      if (calls == 25 || calls == 250 || calls == 1000) TR("threads: ps5lwjgl.c rev ctl5 (probe + calibration); attribute wrapper active: %u thread attributes seen; mutexes made %d destroyed %d failed %d shared %d (HotSpot mutexattr type=%d pshared=%d protocol=%d, dropped)", calls, g_mx_n, g_mx_destroyed, g_mx_fail, g_mx_shared, g_mx_type, g_mx_ps, g_mx_pr); }
    if (a) {
        size_t g = 0;
        static int first; if (!first && pthread_attr_getguardsize(a, &g) == 0) { first = 1; TR("threads: guard-size check is active (first thread attribute: guard %zu KB)", g >> 10); }
        if (pthread_attr_getguardsize(a, &g) == 0 && g > ((size_t)1 << 20)) {
            // pthread_attr_t is stored inline (log 74ab798a: the first word read like a stack slot, and treating it as a pointer to a struct zeroed a live stack frame: crash right after). The console's
            // pthread_attr_init only sets some fields and leaves the rest as stale stack bytes; __wrap_pthread_attr_init below now zeroes the attribute first. This is the fallback for an attribute made elsewhere.
            static int fixed; fixed++;
            int ds = 0; pthread_attr_getdetachstate(a, &ds); memset((void *)a, 0, sizeof *a); if (__real_pthread_attr_init) __real_pthread_attr_init(a); else pthread_attr_init(a); pthread_attr_setdetachstate(a, ds);
            { size_t g3 = 0; int still = pthread_attr_getguardsize(a, &g3) == 0 && g3 > ((size_t)1 << 20);
              if (fixed <= 8) TR("threads: thread attribute had a garbage guard size (%zu KB): attribute zeroed and re-initialised%s (fix #%d, sizeof attr %zu)", g >> 10, still ? ", STILL DIRTY" : "", fixed, sizeof *a); }
        }
    }
    // Every thread gets the creator's CPU affinity written explicitly. The console's pthread_attr_init leaves fields uninitialised (the guard-size slot held a stale pointer in three logs); the neighbouring cpuset fields are
    // the likely cause of pthread_create ERANGE (a garbage cpuset size) and of threads that never run (log 8c75d14e: a live thread that did not answer a signal for 200 ms while the VM waited for it).
    if (a && ps5_p_getaff && ps5_p_attr_setaff) {
        static const size_t szs[] = { 16, 32, 128, 8 }; unsigned long long m[16]; int i, ok = 0;
        for (i = 0; i < 4 && !ok; i++) { memset(m, 0, sizeof m); if (ps5_p_getaff(pthread_self(), szs[i], m) == 0) { int r = ps5_p_attr_setaff(a, szs[i], m); ok = 1; if (r) { static int sf; if (sf++ < 6) TR("threads: pthread_attr_setaffinity_np rc=%d (size %zu): this thread is created without the inherited affinity", r, szs[i]); }
            static int logged; if (logged++ < 3) TR("threads: affinity inherited explicitly (cpuset %zu bytes, mask %llx, set rc %d)", szs[i], m[0], r); } }
        static int warned; if (!ok && !warned++) TR("threads: pthread_getaffinity_np failed for every cpuset size: attribute affinity left as is");
    } else { static int w2; if (!w2++) TR("threads: affinity API not available (get=%p set=%p)", (void *)ps5_p_getaff, (void *)ps5_p_attr_setaff); }
    return __real_pthread_attr_setstacksize(a, sz);
}
// Thread registry (HotSpot's Java/VM/GC/compiler threads, registered by their first pthread_sigmask call): when the whole JVM stops, pad_sampler samples all of them to name the one that blocks the safepoint.
static pthread_t g_reg_th[REG_N]; static unsigned long long g_reg_aff[REG_N];   // g_reg_free (declared at pool_probe): libc heap free pieces (x256 KB) when the thread registered
int __real_pthread_sigmask(int, const sigset_t *, sigset_t *);
static void sched_note(int idx) {
    struct sched_param sp; int pol = -1, rc = 0; memset(&sp, 0, sizeof sp);
    if (g_sched_prio >= 256 && g_sched_prio <= 767) { sp.sched_priority = g_sched_prio; rc = pthread_setschedparam(pthread_self(), 3, &sp); }
    if (idx < 6 || (g_sched_prio && rc && idx < 40)) { memset(&sp, 0, sizeof sp); int g = pthread_getschedparam(pthread_self(), &pol, &sp); TR("sched: thread #%d policy=%d prio=%d (get rc=%d)%s", idx + 1, pol, sp.sched_priority, g, g_sched_prio ? (rc ? " set RR FAILED" : " set RR ok") : ""); if (g_sched_prio && rc) TR("sched: pthread_setschedparam(RR,%d) rc=%d", g_sched_prio, rc); }
}
int __wrap_pthread_sigmask(int how, const sigset_t *set, sigset_t *old) {   // every HotSpot thread calls this once at start (pthread_create itself is already wrapped elsewhere in the title): that registers it
    pthread_t me = pthread_self(); int n = g_reg_n, seen = 0;
    for (int i = 0; i < n && i < REG_N; i++) if (g_reg_th[i] == me) { seen = 1; break; }
    if (!seen) { int i = __sync_fetch_and_add(&g_reg_n, 1); if (i < REG_N) { g_reg_th[i] = me; g_reg_free[i] = libc_free_pieces(); sched_note(i); } }   /* no affinity query here: it runs inside the new thread's start-up and both stalls had a fresh thread stuck in it */
    return __real_pthread_sigmask(how, set, old);
}
extern volatile int ps5_sample_ack __attribute__((weak));
extern volatile int ps5_sample_compact __attribute__((weak));   // jvm_host.cpp: nonzero = the SIGUSR1 handler prints one short SAMPLE line instead of the long CRASH block
static void sample_all_threads(void) {
    if (!&ps5_sample_compact) return;
    int n = g_reg_n; if (n > REG_N) n = REG_N;
    TR("stall: native sample of %d threads follows (SAMPLE #i lines)", n); for (int i = 0; i < n; i++) if (g_reg_aff[i]) { static unsigned long long prev = ~0ull; if (g_reg_aff[i] != prev) TR("thr #%d affinity %llx", i + 1, g_reg_aff[i]); prev = g_reg_aff[i]; }
    for (int i = 0; i < n; i++) {
        ps5_sample_compact = i + 1; ps5_sample_ack = 0; int rc = pthread_kill(g_reg_th[i], SIGUSR1);
        if (rc) { continue; }   // thread is gone
        int w = 0; while (ps5_sample_ack != i + 1 && w < 40) { usleep(5000); w++; }
        if (ps5_sample_ack != i + 1) {
            long long c0 = -1, c1 = -1; clockid_t cid; struct timespec t0, t1;
            if (ps5_p_cpuclock && ps5_p_cpuclock(g_reg_th[i], &cid) == 0 && clock_gettime(cid, &t0) == 0) { usleep(300000); if (clock_gettime(cid, &t1) == 0) { c0 = t0.tv_sec * 1000000000LL + t0.tv_nsec; c1 = t1.tv_sec * 1000000000LL + t1.tv_nsec; } }
            TR("SAMPLE #%d NO ANSWER in 200 ms (affinity %llx): thread cpu time over the next 300 ms: %lld us (%s)", i + 1, g_reg_aff[i], c0 < 0 ? -1LL : (c1 - c0) / 1000, c0 < 0 ? "cpu clock unavailable" : c1 - c0 > 100000000LL ? "busy: spinning, signals blocked?" : c1 - c0 < 1000000LL ? "idle: asleep in the kernel or never scheduled" : "light");
        }
        else usleep(20000);
    }
    ps5_sample_compact = 0;
}
#else
static void sample_all_threads(void) { }
#endif
static void *gl_proc_core(const char *n) {
    void *p = (void *)eglGetProcAddress(n);
    if (!p || n[0] != 'g' || n[1] != 'l') return p;
    if (!g_real_glGetError) g_real_glGetError = (unsigned (*)(void))eglGetProcAddress("glGetError");
#ifdef PS5_MEGA
    if (!g_zk) { void *w = mg_lookup(n, p); if (w) return w; }   // chunk merging is an SDK-GL workaround; Mesa gets the game's own draw calls
#endif
    if (!strcmp(n, "glGetString")) { g_real_getstring = (const unsigned char *(*)(int))p; return (void *)gw_getstring; }
    { void *inner = p;
#ifdef PS5_MEGA
      /* mg_lookup above already returned for its own names, so anything reaching here is unwrapped */
#endif
      void *sw = sh_lookup(n, inner); if (sw) return sw; }
    // Zink (Mesa) fetches every vertex format and compiles every shader as the game wrote it. The vertex-format / shader-source rewriters below exist only because the SDK's GL cannot fetch
    // integer or 16-bit attributes; on Mesa they half-rewrite Iris's glsl-transformer output (the log showed "still declares an integer input") and the terrain vanished.
    if (g_zk) return p;
    for (int i = 0; i < G_N; i++) if (!strcmp(n, g_gl_name[i])) {
#ifndef PS5_GLPROF
        // Lean default: the timing wrappers cost two clock_gettime() syscalls per GL call (about 3 us each on this console, ~2600 per frame at render distance 4 =
        // ~8 ms), which is why they are opt-in (GLPROF=1). Only the vertex-format rewriters stay (VFIX=1).
#ifdef PS5_VFIX
        if (!(i == G_VATTRI || i == G_VATTR || i == G_SHSRC || i == G_BINDATTR)) return p;
#else
        return p;
#endif
#endif
        if (!g_gl_real[i]) g_gl_real[i] = (glfn_t)p; return (void *)g_gl_wrap[i]; }
    return p;
}
// ---------------- no-op blit fix ----------------
// Found with GLALL=1 (per-function GPU time): with Sodium, ONE glBlitFramebuffer per frame - read FBO 0, draw FBO 0, source rectangle == destination rectangle (0,0..1920,1080), colour
// only - took ~203 ms of GPU time (glFinish before the swap: 205 ms), i.e. the frame rate was 4.8 fps. Copying a region of the default framebuffer onto itself changes nothing, so it is skipped.
// Only the default framebuffer is treated this way (a blit inside another FBO can copy between its different read/draw attachments). NOBLITFIX=1 (build_lwjgl.sh) switches this off.
static glfn_t g_bf_real_bind, g_bf_real_blit; static long g_bf_read, g_bf_draw; static unsigned long g_bf_skipped;
static long gw_bf_bind(long a, long b, long c, long d, long e, long f, long g, long h, long i, long j) {
    if (a == 0x8CA8) g_bf_read = b; else if (a == 0x8CA9) g_bf_draw = b; else if (a == 0x8D40) g_bf_read = g_bf_draw = b;   // GL_READ_FRAMEBUFFER / GL_DRAW_FRAMEBUFFER / GL_FRAMEBUFFER
    return g_bf_real_bind(a, b, c, d, e, f, g, h, i, j);
}
static int g_bf_seen;
#ifndef NO_PRESENT_DRAW
// The blit that really is slow (see the GLALL numbers): Sodium presents Minecraft's main render target with glBlitFramebuffer(FBO 1 -> screen, 1920x1080, colour, same size), ~203 ms of GPU time on
// this console's GL layer, while vanilla presents the same image with a shader-drawn quad at full speed. So that one blit is not issued; present_draw() (below, runs inside glfwSwapBuffers right
// before the cursor overlay) draws the main target's colour texture to the screen with a full-screen triangle instead. NO_PRESENT_DRAW (build_lwjgl.sh NOPRESENT=1) keeps the real blit.
static int g_pr_pending, g_pr_valid; static unsigned long g_pr_replaced;
static int present_prepare(long fbo, long w, long h, int named);
// Every call that can change what a framebuffer's colour attachment is (shader packs re-attach textures every frame, with the DSA variants too) makes the cached texture id stale: look it up again.
// Only calls that can change THE cached source (colour attachment 0 of the presented FBO, or a deletion of that FBO / texture) make the cached texture id stale. Minecraft 26 re-attaches textures of
// other FBOs every frame; invalidating on those cost one driver query per frame (it syncs with the SDK's GL thread).
static long g_pr_cfbo; static unsigned g_pr_ctex;   // what present_prepare cached
static int pr_has_id(long n, long ptr, long id) { const unsigned *p = (const unsigned *)ptr; if (!p || n <= 0 || n > 64) return 1; for (long k = 0; k < n; k++) if ((long)p[k] == id) return 1; return 0; }
static long pr_bound(long target) { return target == 0x8CA8 ? g_bf_read : g_bf_draw; }
// Re-attaching a level-0 texture to the presented FBO's colour 0 is followed from the call arguments (TEX = the texture argument, LVL = the level argument); anything else that touches it invalidates.
#define PR_INV(NAME, COND) static glfn_t g_pr_real_##NAME; static long gw_pr_##NAME(long a, long b, long c, long d, long e, long f, long g, long h, long i, long j) { if (COND) g_pr_valid = 0; return g_pr_real_##NAME(a, b, c, d, e, f, g, h, i, j); }
#define PR_TRK(NAME, FBO, TEX, LVL, EXTRA) static glfn_t g_pr_real_##NAME; static long gw_pr_##NAME(long a, long b, long c, long d, long e, long f, long g, long h, long i, long j) { \
    if (b == 0x8CE0 && (FBO) == g_pr_cfbo) { if ((TEX) > 0 && (LVL) == 0 && (EXTRA)) g_pr_ctex = (unsigned)(TEX); else g_pr_valid = 0; } return g_pr_real_##NAME(a, b, c, d, e, f, g, h, i, j); }
PR_TRK(ft2d, pr_bound(a), d, e, c == 0x0DE1) PR_TRK(ftex, pr_bound(a), c, d, 1) PR_TRK(ftexlayer, pr_bound(a), c, d, 0) PR_TRK(nftex, a, c, d, 1) PR_TRK(nftexlayer, a, c, d, 0)
PR_INV(deltex, pr_has_id(a, b, (long)g_pr_ctex)) PR_INV(frb, b == 0x8CE0 && pr_bound(a) == g_pr_cfbo) PR_INV(nfrb, b == 0x8CE0 && a == g_pr_cfbo) PR_INV(delfb, pr_has_id(a, b, g_pr_cfbo))
static const struct { const char *name; glfn_t *real; glfn_t wrap; } g_pr_inv[] = { { "glFramebufferTexture2D", &g_pr_real_ft2d, gw_pr_ft2d }, { "glDeleteTextures", &g_pr_real_deltex, gw_pr_deltex }, { "glFramebufferTexture", &g_pr_real_ftex, gw_pr_ftex },
    { "glFramebufferTextureLayer", &g_pr_real_ftexlayer, gw_pr_ftexlayer }, { "glNamedFramebufferTexture", &g_pr_real_nftex, gw_pr_nftex }, { "glNamedFramebufferTextureLayer", &g_pr_real_nftexlayer, gw_pr_nftexlayer },
    { "glFramebufferRenderbuffer", &g_pr_real_frb, gw_pr_frb }, { "glNamedFramebufferRenderbuffer", &g_pr_real_nfrb, gw_pr_nfrb }, { "glDeleteFramebuffers", &g_pr_real_delfb, gw_pr_delfb }, { 0, 0, 0 } };
#endif
static long gw_bf_blit(long a, long b, long c, long d, long e, long f, long g, long h, long i, long j) {
    int noop = g_bf_read == 0 && g_bf_draw == 0 && a == e && b == f && c == g && d == h;
#ifndef NO_PRESENT_DRAW
    if (!noop && g_bf_read != 0 && g_bf_draw == 0 && a == e && b == f && c == g && d == h && a == 0 && b == 0 && i == 0x4000 && present_prepare(g_bf_read, c, d, 0)) {
        g_pr_pending = 1;
        if (g_bf_seen < 8) { g_bf_seen++; TR("blitfix: blit #%d read_fbo=%ld draw_fbo=%ld src %ld,%ld..%ld,%ld mask=0x%lx -> REPLACED by a textured triangle at swap time", g_bf_seen, g_bf_read, g_bf_draw, a, b, c, d, i); }
        g_pr_replaced++;
        return 0;
    }
#endif
    if (g_bf_seen < 8) { g_bf_seen++; TR("blitfix: blit #%d read_fbo=%ld draw_fbo=%ld src %ld,%ld..%ld,%ld dst %ld,%ld..%ld,%ld mask=0x%lx filter=0x%lx -> %s", g_bf_seen, g_bf_read, g_bf_draw, a, b, c, d, e, f, g, h, i, j, noop ? "SKIPPED (no-op)" : "forwarded"); }
    if (noop) {
        if (++g_bf_skipped % 1000 == 0) TR("blitfix: %lu no-op blits skipped so far", g_bf_skipped);
        return 0;
    }
    return g_bf_real_blit(a, b, c, d, e, f, g, h, i, j);
}
// Minecraft 26.x presents with the DSA call glBlitNamedFramebuffer(read, draw, src x0,y0,x1,y1, dst x0,y0,x1,y1, mask, filter) - 12 arguments, the last six on the stack - and the profiler put 81 of 100
// samples in it (~125 ms of a 155 ms frame, every other GL call together ~12 ms). Same treatment as the glBlitFramebuffer above: read FBO != 0 -> screen, 1:1 colour copy becomes the swap-time triangle.
typedef long (*glfn12_t)(long, long, long, long, long, long, long, long, long, long, long, long);
static glfn12_t g_bf_real_blitn;
static long gw_bf_blitn(long r, long d, long a, long b, long c, long dd, long e, long f, long g, long h, long mask, long filt) {
    int tolog = g_bf_seen < 8;
#ifndef NO_PRESENT_DRAW
    if (d == 0 && r != 0 && a == e && b == f && c == g && dd == h && a == 0 && b == 0 && mask == 0x4000 && present_prepare(r, c, dd, 1)) {
        g_pr_pending = 1; g_pr_replaced++;
        if (tolog) { g_bf_seen++; TR("blitfix: named blit read_fbo=%ld draw_fbo=0 src 0,0..%ld,%ld mask=0x%lx -> REPLACED by a textured triangle at swap time", r, c, dd, mask); }
        return 0;
    }
#endif
    if (tolog) { g_bf_seen++; TR("blitfix: named blit read_fbo=%ld draw_fbo=%ld src %ld,%ld..%ld,%ld dst %ld,%ld..%ld,%ld mask=0x%lx filter=0x%lx (window %dx%d) -> forwarded", r, d, a, b, c, dd, e, f, g, h, mask, filt, g_w, g_h); }
    return g_bf_real_blitn(r, d, a, b, c, dd, e, f, g, h, mask, filt);
}
#ifdef PS5_MDLOOP
// MDLOOP=1 experiment: GLALL showed ~77 ms of GPU time per SUB-DRAW of Sodium's glMultiDrawElementsBaseVertex under Iris (144 indices cost as much as 6144), a fixed cost per sub-draw. Here the multi-draw is issued as
// one plain glDrawElementsBaseVertex per sub-draw (same result by definition of the call). If the per-sub-draw cost disappears, the SDK's multi-draw path was the problem; if not, the cost is in the draw state.
static glfn_t g_md_real;
static long gw_md_loop(long mode, long cnt, long type, long idx, long dc, long bv, long g, long h, long i, long j) {
    static void (*deb)(unsigned, int, unsigned, const void *, int);
    if (!deb) deb = (void (*)(unsigned, int, unsigned, const void *, int))eglGetProcAddress("glDrawElementsBaseVertex");
    if (!deb || !cnt || !idx || !bv || dc <= 0) return g_md_real(mode, cnt, type, idx, dc, bv, g, h, i, j);
    const int *c = (const int *)cnt, *b = (const int *)bv; const void *const *ix = (const void *const *)idx;
    for (long k = 0; k < dc; k++) if (c[k] > 0) deb((unsigned)mode, c[k], (unsigned)type, ix[k], b[k]);
    static unsigned long n; if (++n == 1 || n % 3000 == 0) TR("mdloop: %lu multi-draws issued as separate glDrawElementsBaseVertex calls (last one: %ld sub-draws)", n, dc);
    return 0;
}
#endif
// ---------------- shader build time probe ("shaders:" lines) ----------------
// Minecraft 26 builds ~60+ programs at start-up and the profiler shows the render thread parked in glGetProgramiv / glGetShaderiv for ~34 s of the ~45 s start (the SDK compiles lazily, at the first
// status query). These wrappers add up the time per call kind and print a line every 2 s while shaders are being built; the first link also reports whether the SDK offers program binaries
// (a cache of compiled programs would need glGetProgramBinary / glProgramBinary and a non-zero GL_NUM_PROGRAM_BINARY_FORMATS).
enum { SHP_COMPILE, SHP_LINK, SHP_PROGIV, SHP_SHIV, SHP_USE, SHP_UNILOC, SHP_UBLK, SHP_N };
static const char *const g_shp_name[SHP_N] = { "glCompileShader", "glLinkProgram", "glGetProgramiv", "glGetShaderiv", "glUseProgram", "glGetUniformLocation", "glGetUniformBlockIndex" };
static glfn_t g_shp_real[SHP_N]; static unsigned long g_shp_n[SHP_N]; static double g_shp_t[SHP_N], g_shp_max[SHP_N], g_shp_last;
// LAZYLINK=1 (build): glGetProgramiv(LINK_STATUS) / glGetShaderiv(COMPILE_STATUS) are answered "true" without asking the SDK (its answer is what blocks, ~0.16 s per program, ~177 programs at start-up);
// the real LINK_STATUS is asked the first time glUseProgram uses the program (a failure is logged there). Whether this saves time depends on whether the SDK compiles at the status query
// or at link time: the "shaders:" line shows where the time goes. Programs that are never used are never asked about.
enum { SHF_LINKED = 1, SHF_VERIFIED = 2, SHF_USED = 4 };
static unsigned char g_shp_prog[8192]; static unsigned long g_shp_nlinked, g_shp_nused, g_shp_skipped;
#ifdef PS5_LAZYLINK
#define SHP_LAZY 1
#else
#define SHP_LAZY 0
#endif
static long shp_call(int k, long a, long b, long c, long d, long e, long f, long g, long h, long i, long j) {
    unsigned prog = (unsigned)a; int pname = (int)b; unsigned char *pf = (k <= SHP_UBLK && prog < sizeof g_shp_prog && k != SHP_COMPILE && k != SHP_SHIV) ? &g_shp_prog[prog] : 0;
    if (k == SHP_LINK && pf) { *pf = SHF_LINKED; g_shp_nlinked++; }
    if (k == SHP_PROGIV && pname == 0x8B82 && c) {   // GL_LINK_STATUS
        if (SHP_LAZY && pf && (*pf & SHF_LINKED) && !(*pf & SHF_VERIFIED)) { *(int *)c = 1; g_shp_skipped++; return 0; }
    }
    if (SHP_LAZY && k == SHP_SHIV && pname == 0x8B81 && c) { *(int *)c = 1; g_shp_skipped++; return 0; }   // GL_COMPILE_STATUS
    if (k == SHP_USE && pf && prog) {
        if (!(*pf & SHF_USED)) { *pf |= SHF_USED; g_shp_nused++; }
        if (SHP_LAZY && (*pf & SHF_LINKED) && !(*pf & SHF_VERIFIED)) {
            int ok = 0; double tv = now_s(); g_shp_real[SHP_PROGIV](prog, 0x8B82, (long)&ok, 0, 0, 0, 0, 0, 0, 0); *pf |= SHF_VERIFIED;
            if (!ok) TR("shaders: program %u FAILED to link (found when it was first used, %.0f ms)", prog, 1e3 * (now_s() - tv));
            g_shp_t[SHP_PROGIV] += now_s() - tv;
        }
    }
    double t0 = now_s(); long r = g_shp_real[k](a, b, c, d, e, f, g, h, i, j); double t1 = now_s(), dt = t1 - t0;
    if (k == SHP_PROGIV && pname == 0x8B82 && pf) *pf |= SHF_VERIFIED;
    g_shp_n[k]++; g_shp_t[k] += dt; if (dt > g_shp_max[k]) g_shp_max[k] = dt;
    if (k == SHP_LINK && g_shp_n[k] == 1) {
        void *gb = (void *)eglGetProcAddress("glGetProgramBinary"), *pb = (void *)eglGetProcAddress("glProgramBinary"); int nf = -1;
        void (*gi)(unsigned, int *) = (void (*)(unsigned, int *))eglGetProcAddress("glGetIntegerv"); if (gi) gi(0x87FE, &nf);   // GL_NUM_PROGRAM_BINARY_FORMATS
        TR("shaders: program binary support: glGetProgramBinary=%p glProgramBinary=%p formats=%d (lazy link check: %s)", gb, pb, nf, SHP_LAZY ? "ON" : "off");
    }
    if (g_shp_last == 0) g_shp_last = t0;
    if (t1 - g_shp_last > 2.0 && (k != SHP_USE || g_shp_n[SHP_LINK] > 0)) {
        g_shp_last = t1; char b2[640]; int len = snprintf(b2, sizeof b2, "shaders: programs linked=%lu used=%lu, status queries answered without the SDK=%lu |", g_shp_nlinked, g_shp_nused, g_shp_skipped);
        for (int m = 0; m < SHP_N; m++) len += snprintf(b2 + len, sizeof b2 - len, " %s n=%lu tot=%.1fs max=%.0fms;", g_shp_name[m] + 2, g_shp_n[m], g_shp_t[m], 1e3 * g_shp_max[m]);
        TR("%s", b2);
    }
    return r;
}
#define SHP_W(K) static long gw_shp##K(long a, long b, long c, long d, long e, long f, long g, long h, long i, long j) { return shp_call(K, a, b, c, d, e, f, g, h, i, j); }
SHP_W(0) SHP_W(1) SHP_W(2) SHP_W(3) SHP_W(4) SHP_W(5) SHP_W(6)
static const glfn_t g_shp_wrap[SHP_N] = { gw_shp0, gw_shp1, gw_shp2, gw_shp3, gw_shp4, gw_shp5, gw_shp6 };
#ifndef NO_FENCEFAKE
// ---------------- GL sync objects (Minecraft 26.x: GlCommandEncoder.submit makes one per submit) ----------------
// Measured in the 26.3 log: ~156 ms per frame (6 fps) with swap=0.1 ms, no sdkwait polls and no kernel stalls, and the profiler puts 89 of 100 samples in glFenceSync:
// the SDK's glFenceSync waits for the GPU (a round trip has a ~16.7 ms floor, see the glFinish probe below), and the renderer makes ~9 fences per frame.
// A fence here is only a stamp (the swap count when it was made). It counts as signaled once FENCE_AGE swaps have passed (the SDK's own frame pacing means the GPU is done with
// work that old); glClientWaitSync on a younger fence with timeout 0 reports TIMEOUT_EXPIRED (the caller polls again), with a timeout it does ONE real glFinish and marks the fence
// signaled, so nothing is ever reported done early. NOFENCEFAKE=1 (build) keeps the SDK's own sync objects.
#ifndef FENCE_AGE
#define FENCE_AGE 3
#endif
#define FS_MAGIC 0xFE17C0DEu
typedef struct { unsigned magic, cond, flags, signaled; unsigned long born; } FakeSync;
static unsigned long g_swap_n; static unsigned long g_fs_made, g_fs_forced, g_fs_polled_young; static double g_fs_forced_t;
static void fs_real_finish(void) { static void (*fin)(void); if (!fin) fin = (void (*)(void))eglGetProcAddress("glFinish"); if (fin) fin(); }
static FakeSync *fs_get(long h) { FakeSync *f = (FakeSync *)h; return (f && f->magic == FS_MAGIC) ? f : 0; }
static int fs_done(FakeSync *f) { if (!f->signaled && g_swap_n - f->born >= FENCE_AGE) f->signaled = 1; return f->signaled; }
static long gw_fence_sync(long cond, long flags, long c, long d, long e, long f, long g, long h, long i, long j) {
    (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; (void)j;
    FakeSync *s = (FakeSync *)calloc(1, sizeof *s); if (!s) return 0;
    s->magic = FS_MAGIC; s->cond = (unsigned)cond; s->flags = (unsigned)flags; s->born = g_swap_n; g_fs_made++; return (long)s;
}
static long gw_client_wait(long sync, long flags, long timeout, long d, long e, long f, long g, long h, long i, long j) {
    (void)flags; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; (void)j;
    FakeSync *s = fs_get(sync); if (!s) return 0x911D;                       // GL_WAIT_FAILED
    if (fs_done(s)) return 0x911A;                                           // GL_ALREADY_SIGNALED
    if (timeout == 0) { g_fs_polled_young++; return 0x911B; }                // GL_TIMEOUT_EXPIRED: not known to be done yet
    double t0 = now_s(); fs_real_finish(); g_fs_forced_t += now_s() - t0; g_fs_forced++; s->signaled = 1;
    return 0x911C;                                                           // GL_CONDITION_SATISFIED
}
static long gw_wait_sync(long sync, long a, long b, long d, long e, long f, long g, long h, long i, long j) { (void)sync; (void)a; (void)b; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; (void)j; return 0; }   // GPU-side wait: one queue, nothing to do
static long gw_delete_sync(long sync, long a, long b, long d, long e, long f, long g, long h, long i, long j) { (void)a; (void)b; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; (void)j; FakeSync *s = fs_get(sync); if (s) { s->magic = 0; free(s); } return 0; }
static long gw_is_sync(long sync, long a, long b, long d, long e, long f, long g, long h, long i, long j) { (void)a; (void)b; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; (void)j; return fs_get(sync) != 0; }
static long gw_get_synciv(long sync, long pname, long bufsize, long length, long values, long f, long g, long h, long i, long j) {
    (void)f; (void)g; (void)h; (void)i; (void)j;
    FakeSync *s = fs_get(sync); int *v = (int *)values; int *len = (int *)length; if (!s || !v || bufsize < 1) { if (len) *len = 0; return 0; }
    switch ((unsigned)pname) {
    case 0x9112: *v = 0x9116; break;                                         // SYNC_OBJECT_TYPE = SYNC_FENCE
    case 0x9113: *v = (int)s->cond; break;                                   // SYNC_CONDITION
    case 0x9114: *v = fs_done(s) ? 0x9119 : 0x9118; break;                   // SYNC_STATUS = SIGNALED / UNSIGNALED
    case 0x9115: *v = (int)s->flags; break;                                  // SYNC_FLAGS
    default: if (len) *len = 0; return 0;
    }
    if (len) *len = 1; return 0;
}
static void *fs_lookup(const char *n) {
    if (!strcmp(n, "glFenceSync")) return (void *)gw_fence_sync;
    if (!strcmp(n, "glClientWaitSync")) return (void *)gw_client_wait;
    if (!strcmp(n, "glWaitSync")) return (void *)gw_wait_sync;
    if (!strcmp(n, "glDeleteSync")) return (void *)gw_delete_sync;
    if (!strcmp(n, "glIsSync")) return (void *)gw_is_sync;
    if (!strcmp(n, "glGetSynciv")) return (void *)gw_get_synciv;
    return 0;
}
#endif
// ---- fixed-function mode (Minecraft <= 1.16.5 and older): every GL name is answered by gl4es (ps5gl4es.c / build_gl4es.sh) running on this core context ----
// McBoot switches it on (Hello.glCompat -> jvm_host.cpp) before the game loads LWJGL; ps5_glcompat_level is the GL version gl4es reports (21 = 2.1), 0 = off.
int ps5_glcompat_level;
static int g_lk_probe;   // GL lookups logged so far in this round (reset in ps5_gl_release)
extern void ps5g4_configure(void *(*)(const char *), int) __attribute__((weak));
extern void *ps5g4_lookup(const char *) __attribute__((weak));
extern void ps5g4_pre_swap(void) __attribute__((weak));
extern int ps5g4_ready(void) __attribute__((weak));
extern void ps5g4_resync(void) __attribute__((weak));
#ifdef PS5_GLCPU
static void *ga_wrap(const char *n, void *p);
static void *g4_core(const char *n) {   // GLCPU: the SDK calls gl4es makes are timed too, under "@Name" (the game's own calls keep "Name")
    void *p = (void *)eglGetProcAddress(n);
    if (p && n[0] == 'g' && n[1] == 'l') { char b[96]; snprintf(b, sizeof b, "gl@%s", n + 2); return ga_wrap(b, p); }
    return p;
}
#else
static void *g4_core(const char *n) { return (void *)eglGetProcAddress(n); }
#endif
static void *gl_proc_base(const char *n) {
    if (ps5_glcompat_level && n[0] == 'g' && n[1] == 'l') {
        static int cfg;
        if (!cfg) {
            cfg = 1;
            if (ps5g4_lookup && ps5g4_configure) { ps5g4_configure(g4_core, ps5_glcompat_level); TR("g4: fixed-function mode on: gl4es answers the GL lookups (GL %d.%d)", ps5_glcompat_level / 10, ps5_glcompat_level % 10); }
            else { TR("g4: fixed-function mode requested, but this build has no gl4es (build_gl4es.sh / GL4ES=0?)"); ps5_glcompat_level = 0; }
        }
        if (ps5_glcompat_level) {
            if (g_lk_probe < 3) { g_lk_probe++; TR("glproc: round lookup #%d %s", g_lk_probe, n); }   // probe: where does a second game stop?
            void *g = ps5g4_lookup(n);
            if (g) return g;
            /* a name gl4es does not know: fall through to the console's own function */
        }
    }
    void *p = gl_proc_core(n);
    if (g_zk) return p;   // Zink/Mesa is a complete GL: none of the SDK workarounds (fence stamps, shader wrappers, blit replacement) apply
#ifndef NO_FENCEFAKE
    if (p && n[0] == 'g' && n[1] == 'l') { void *w = fs_lookup(n); if (w) { TR("fence: %s replaced by the stamp-based emulation (age %d swaps)", n, FENCE_AGE); return w; } }
#endif
    if (p && n[0] == 'g' && n[1] == 'l') for (int k = 0; k < SHP_N; k++) if (!strcmp(n, g_shp_name[k])) { g_shp_real[k] = (glfn_t)p; return (void *)g_shp_wrap[k]; }
#ifndef NO_BLITFIX
    if (p && n[0] == 'g' && n[1] == 'l') {
        if (!strcmp(n, "glBindFramebuffer")) { g_bf_real_bind = (glfn_t)p; TR("blitfix: hooked glBindFramebuffer"); return (void *)gw_bf_bind; }
        if (!strcmp(n, "glBlitFramebuffer")) { g_bf_real_blit = (glfn_t)p; TR("blitfix: hooked glBlitFramebuffer"); return (void *)gw_bf_blit; }
        if (!strcmp(n, "glBlitNamedFramebuffer")) { g_bf_real_blitn = (glfn12_t)p; TR("blitfix: hooked glBlitNamedFramebuffer"); return (void *)gw_bf_blitn; }
#ifdef PS5_MDLOOP
        if (!strcmp(n, "glMultiDrawElementsBaseVertex")) { g_md_real = (glfn_t)p; TR("mdloop: hooked glMultiDrawElementsBaseVertex"); return (void *)gw_md_loop; }
#endif
#ifndef NO_PRESENT_DRAW
        for (int k = 0; g_pr_inv[k].name; k++) if (!strcmp(n, g_pr_inv[k].name)) { *g_pr_inv[k].real = (glfn_t)p; return (void *)g_pr_inv[k].wrap; }
#endif
    }
#endif
    return p;
}
#ifdef PS5_GLALL
// ---------------- GLALL=1: GPU time per GL function (diagnostic build) ----------------
// The finish probe showed the GPU busy ~205 ms per frame (Sodium) while every CPU-side GL call is cheap, and the wrapped calls above are only a handful of functions.
// Here EVERY GL function LWJGL looks up gets a libffi-closure wrapper: it forwards the 10 integer/pointer argument registers, and a glFinish() right after the call turns
// the GPU work of THAT call into call time. Every perf window logs the functions with the most total time / the longest single call ("gall: ..."). The frame rate collapses
// (a finish per call): read the numbers, not the fps. Functions with float/double arguments (XMM registers) cannot pass through an integer-only wrapper and are left alone.
#include <ffi.h>
#define GA_MAX 800
typedef struct { char name[44]; glfn_t real; void *code; unsigned long n, nerr; double t, tmax; int dolog, logn; } GaEnt;
static unsigned (*g_ga_err)(void); static int g_ga_errn;   // glGetError as the application sees it (gl4es' in fixed-function mode): asked before and after every wrapped call, so the call that raises an error is named ("gallerr:")
static GaEnt g_ga[GA_MAX]; static int g_ga_n, g_ga_init; static ffi_cif g_ga_cif; static ffi_type *g_ga_at[10];
static int ga_skip(const char *n) {
    size_t l = strlen(n); if (l < 3) return 1;
    char last = n[l - 1]; if (last == 'f' || last == 'd') return 1;   // glUniform1f, glTexParameterf, glClearDepthf ...
    static const char *const bad[] = { "ClearColor", "ClearDepth", "BlendColor", "PolygonOffset", "LineWidth", "PointSize", "DepthRange", "SampleCoverage", "MinSampleShading", "Clearfi", "BufferfI",
        "Ortho", "Frustum", "AlphaFunc", "ClearAccum", "Accum", "PixelZoom", "Bitmap", 0 };   // float / double arguments in other names too (fixed-function GL: glOrtho / glFrustum / glAlphaFunc lost their arguments here and drew a garbled 1.12.2 title screen)
    for (int i = 0; bad[i]; i++) if (strstr(n, bad[i])) return 1;
    return !strcmp(n, "glFinish") || !strcmp(n, "glGetError") || !strcmp(n, "glGetString") || !strcmp(n, "glGetStringi");   // glFinish is the probe itself; the others are called constantly and must stay cheap
}
static int ga_wants_args(const char *n) {   // calls whose first arguments are logged (the vertex layout and the draw/buffer set-up are what matters when geometry is wrong)
    static const char *const k[] = { "VertexAttrib", "VertexArrayAttrib", "VertexArrayVertexBuffer", "VertexArrayElementBuffer", "BindVertexBuffer", "MultiDraw", "DrawElements", "DrawArrays", "DrawRange", "BindBufferRange", "BindBufferBase",
        "MemoryBarrier", "BufferStorage", "NamedBufferStorage", "TexBuffer", "DispatchCompute", "BindImageTexture", "ClearNamedBuffer", "ClearBuffer", "CopyBufferSubData", "CopyNamedBufferSubData", "ProvokingVertex", "PrimitiveRestart", 0 };
    for (int i = 0; k[i]; i++) if (strstr(n, k[i])) return 1;
    return 0;
}
static void ga_handler(ffi_cif *cif, void *ret, void **args, void *user) {
    GaEnt *e = (GaEnt *)user; long a[10]; (void)cif;
    for (int i = 0; i < 10; i++) a[i] = *(long *)args[i];
    if (e->dolog && e->logn < 6) { e->logn++; TR("gallargs: gl%s(0x%lx, 0x%lx, 0x%lx, 0x%lx, 0x%lx, 0x%lx)", e->name, a[0], a[1], a[2], a[3], a[4], a[5]); }
    if (g_ga_err) { unsigned er = g_ga_err(); if (er && g_ga_errn < 80) { g_ga_errn++; TR("gallerr: error 0x%x was already pending BEFORE gl%s (raised by an earlier call that GLALL does not wrap: float / double arguments, glGetError)", er, e->name); } }
    double t0 = now_s(); long r = e->real(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]);
    if (g_ga_fin) g_ga_fin();
    if (g_ga_err) { unsigned er = g_ga_err(); if (er) { e->nerr++; if (g_ga_errn < 80) { g_ga_errn++; TR("gallerr: gl%s(0x%lx, 0x%lx, 0x%lx, 0x%lx, 0x%lx, 0x%lx) raised error 0x%x (call #%lu of this function)", e->name, a[0], a[1], a[2], a[3], a[4], a[5], er, e->n + 1); } } }
    double dt = now_s() - t0; e->n++; e->t += dt; if (dt > e->tmax) e->tmax = dt;
    { static int slow_logged;   // slow multi-draws (Sodium's terrain): how many sub-draws and indices, and under which program / framebuffer / viewport (the state comes from the GLPROF trackers)
      if (dt > 0.050 && slow_logged < 80 && !strncmp(e->name, "MultiDrawElements", 17)) {
          slow_logged++; long dc = a[4], total = 0; const int *cnt = (const int *)a[1];
          if (cnt && dc > 0 && dc < 100000) for (long k = 0; k < dc; k++) total += cnt[k];
          TR("gallslow: %s %.1fms mode=0x%lx type=0x%lx subdraws=%ld indices=%ld prog=%ld fbo=%ld vp=%ldx%ld blend=%d depth=%d cull=%d cmask=%x", e->name, dt * 1e3, a[0], a[2], dc, total, g_cur_prog, g_cur_fbo, g_vp_w, g_vp_h, g_blend, g_depth_test, g_cull, g_cmask); } }
    *(long *)ret = r;
}
static void *ga_wrap(const char *n, void *p) {
    if (!g_ga_init) {
        for (int i = 0; i < 10; i++) g_ga_at[i] = &ffi_type_slong;
        if (ffi_prep_cif(&g_ga_cif, FFI_DEFAULT_ABI, 10, &ffi_type_slong, g_ga_at) != FFI_OK) { TR("gall: ffi_prep_cif failed"); g_ga_init = -1; }
#ifdef PS5_GLCPU
        else { g_ga_fin = 0; g_ga_init = 1; g_ga_err = 0; TR("gall: GLCPU: CPU time per GL function (no glFinish, no error polling), numbers are what the render thread spends in each call"); }
#else
        else { g_ga_fin = (void (*)(void))eglGetProcAddress("glFinish"); g_ga_init = 1; g_ga_err = (unsigned (*)(void))gl_proc_base("glGetError"); TR("gall: wrapping every GL function with a per-call glFinish (diagnostic build: fps is meaningless)"); }
#endif
    }
    if (g_ga_init != 1 || ga_skip(n)) return p;
    for (int i = 0; i < g_ga_n; i++) if (!strcmp(g_ga[i].name, n + 2)) return g_ga[i].code;
    if (g_ga_n >= GA_MAX) return p;
    GaEnt *e = &g_ga[g_ga_n]; snprintf(e->name, sizeof e->name, "%s", n + 2); e->real = (glfn_t)p;
    e->dolog = ga_wants_args(n + 2); e->logn = 0;
    void *code = 0; ffi_closure *cl = (ffi_closure *)ffi_closure_alloc(sizeof(ffi_closure), &code);
    if (!cl || ffi_prep_closure_loc(cl, &g_ga_cif, ga_handler, e, code) != FFI_OK) return p;
    e->code = code; g_ga_n++; return code;
}
static void ga_report(int frames) {
    int idx[GA_MAX], k, m, c = 0;
    for (k = 0; k < g_ga_n; k++) if (g_ga[k].n) idx[c++] = k;
    for (k = 0; k < c; k++) for (m = k + 1; m < c; m++) if (g_ga[idx[m]].t > g_ga[idx[k]].t) { int t = idx[k]; idx[k] = idx[m]; idx[m] = t; }
#ifdef PS5_GLCPU
    { double tt = 0, nn = 0, ts = 0, ns = 0; for (k = 0; k < c; k++) { GaEnt *e = &g_ga[idx[k]]; if (e->name[0] == '@') { ts += e->t; ns += e->n; } else { tt += e->t; nn += e->n; } }
      double fr = frames > 0 ? frames : 1;
      TR("gall: TOTAL game calls %.1f ms/frame over %.0f calls/frame (%.2f us per call); SDK calls made by gl4es (@) %.1f ms/frame over %.0f calls/frame (%.2f us per call)", 1e3 * tt / fr, nn / fr, nn > 0 ? 1e6 * tt / nn : 0.0, 1e3 * ts / fr, ns / fr, ns > 0 ? 1e6 * ts / ns : 0.0); }
#endif
    for (int line = 0; line < 5 && line * 3 < c; line++) {
        char b[330]; int len = snprintf(b, sizeof b, "gall:");
        for (k = line * 3; k < c && k < line * 3 + 3; k++) { GaEnt *e = &g_ga[idx[k]]; len += snprintf(b + len, sizeof b - len, " %s n/f=%.1f tot/f=%.1fms max=%.1fms;", e->name, (double)e->n / (frames > 0 ? frames : 1), 1e3 * e->t / (frames > 0 ? frames : 1), 1e3 * e->tmax); }
        TR("%s", b);
    }
    {   // census by call count: every function the application calls, so the set it relies on is visible (per frame)
        int cn[GA_MAX], cc = 0; for (k = 0; k < g_ga_n; k++) if (g_ga[k].n) cn[cc++] = k;
        for (k = 0; k < cc; k++) for (m = k + 1; m < cc; m++) if (g_ga[cn[m]].n > g_ga[cn[k]].n) { int t = cn[k]; cn[k] = cn[m]; cn[m] = t; }
        for (int line = 0; line < 8 && line * 6 < cc; line++) {
            char b[300]; int len = snprintf(b, sizeof b, "gcount:");
            for (k = line * 6; k < cc && k < line * 6 + 6; k++) len += snprintf(b + len, sizeof b - len, " %s=%.1f", g_ga[cn[k]].name, (double)g_ga[cn[k]].n / (frames > 0 ? frames : 1));
            TR("%s", b);
        }
    }
    { char b[300]; int len = 0; for (k = 0; k < g_ga_n; k++) if (g_ga[k].nerr && len < 250) len += snprintf(b + len, sizeof b - len, " %s=%lu", g_ga[k].name, g_ga[k].nerr); if (len > 0) TR("gallerr: calls that raised a GL error in this window:%s", b); }
    for (k = 0; k < g_ga_n; k++) { g_ga[k].n = 0; g_ga[k].nerr = 0; g_ga[k].t = g_ga[k].tmax = 0; }
}
static void *gl_proc(const char *n) { void *p = gl_proc_base(n); return (p && n[0] == 'g' && n[1] == 'l') ? ga_wrap(n, p) : p; }
#else
static void *gl_proc(const char *n) { return gl_proc_base(n); }
#endif
static unsigned long g_wait_starts, g_spin_polls, g_real_sleeps;   // filled by __wrap_sceKernelUsleep: GPU-completion waits seen by the SDK
enum { K_ALLOC, K_MAP, K_MUNMAP, K_RELEASE, K_SUBMIT, K_N };
static const char *const g_k_name[K_N] = { "AllocDirectMem", "MapDirectMem", "munmap", "ReleaseDirectMem", "SubmitDcb" };
static unsigned long g_k_n[K_N]; static double g_k_t[K_N];
static void gl_prof_report(int frames) {
    int idx[G_N], k, m;
    for (k = 0; k < G_N; k++) idx[k] = k;
    for (k = 0; k < G_N; k++) for (m = k + 1; m < G_N; m++) if (g_gl_t[idx[m]] > g_gl_t[idx[k]]) { int t = idx[k]; idx[k] = idx[m]; idx[m] = t; }
    for (int line = 0; line < 4; line++) {
        char b[200]; int len = snprintf(b, sizeof b, "gl:");
        for (k = line * 3; k < line * 3 + 3 && k < G_N; k++) {
            int i = idx[k]; if (!g_gl_n[i] || frames <= 0) break;
            len += snprintf(b + len, sizeof b - len, " %s n/f=%.1f avg=%.2fms tot/f=%.1fms max=%.1fms;", g_gl_name[i] + 2, (double)g_gl_n[i] / frames,
                            1e3 * g_gl_t[i] / g_gl_n[i], 1e3 * g_gl_t[i] / frames, 1e3 * g_gl_max[i]);
        }
        if (len > 3) TR("%s", b);
    }
    if (frames > 0) TR("sdkwait: GPU waits/f=%.1f spin-polls/f=%.1f real-1ms-sleeps/f=%.1f", (double)g_wait_starts / frames, (double)g_spin_polls / frames, (double)g_real_sleeps / frames);
    g_wait_starts = g_spin_polls = g_real_sleeps = 0;
    for (int part = 0; part < 2; part++) {   // kernel calls behind each flush/draw (SDK work-memory alloc/map/unmap/release, command submit)
        char b[200]; int len = snprintf(b, sizeof b, "kern:");
        for (k = part ? 3 : 0; k < (part ? K_N : 3); k++)
            len += snprintf(b + len, sizeof b - len, " %s n/f=%.1f avg=%.3fms tot/f=%.1fms;", g_k_name[k], frames > 0 ? (double)g_k_n[k] / frames : 0.0,
                            g_k_n[k] ? 1e3 * g_k_t[k] / g_k_n[k] : 0.0, frames > 0 ? 1e3 * g_k_t[k] / frames : 0.0);
        TR("%s", b);
    }
    memset(g_k_n, 0, sizeof g_k_n); memset(g_k_t, 0, sizeof g_k_t);
    memset(g_gl_n, 0, sizeof g_gl_n); memset(g_gl_t, 0, sizeof g_gl_t); memset(g_gl_max, 0, sizeof g_gl_max); g_slow_budget = SLOW_BUDGET;
}


// ---------------- stdout/stderr -> trace channel ----------------
// fd 1 and 2 cannot be redirected on the console, so messages the SDK (and everything else linked into the title) prints with
// printf/fprintf(stderr)/puts would vanish. Forward them to the host's UDP log instead.
#ifndef PS5_SDK_LOG_ALL
// The SDK prints several lines per draw (an index list, one printf per index, batch summaries, clears ...). Forwarding each one costs a UDP datagram plus the
// host's 300 us throttle sleep, which made every indexed draw cost ~2.5 ms (8 prints for a 6-index quad) and every clear ~3 ms. Keep the one-off setup lines and
// anything that looks like a problem; build with the "sdklog" option to forward everything again.
static int sdk_line_wanted(const char *t) {
    static const char *const keep[] = { "[ps5-egl]", "[ps5-agc]", "[ps5-output", "[ps5-prepare", "[klog]", 0 };
    static const char *const bad[] = { "fail", "Fail", "FAIL", "error", "Error", "ERROR", "terminat", "nsupported", "nvalid", "dropped", "fallback", "abort", "result=-", "status=-", 0 };
    for (int i = 0; keep[i]; i++) if (!strncmp(t, keep[i], strlen(keep[i]))) return 1;
    for (int i = 0; bad[i]; i++) if (strstr(t, bad[i])) return 1;
    return 0;
}
#endif
static void emit_line(const char *b) {
    char t[300]; size_t n = strlen(b); if (n >= sizeof t) n = sizeof t - 1;
    memcpy(t, b, n); t[n] = 0;
    while (n && (t[n - 1] == '\n' || t[n - 1] == '\r')) t[--n] = 0;
#ifndef PS5_SDK_LOG_ALL
    if (!sdk_line_wanted(t)) return;
#endif
    if (n) { static char tag[] = "[out] "; char o[320]; snprintf(o, sizeof o, "%s%s", tag, t); ps5_trace(o); }
}
int printf(const char *__restrict fmt, ...) {
    char b[400]; va_list a; va_start(a, fmt); int n = vsnprintf(b, sizeof b, fmt, a); va_end(a); emit_line(b); return n;
}
int fprintf(FILE *__restrict f, const char *__restrict fmt, ...) {
    va_list a; va_start(a, fmt);
    int n;
    if (f == stdout || f == stderr) { char b[400]; n = vsnprintf(b, sizeof b, fmt, a); emit_line(b); }
    else n = vfprintf(f, fmt, a);
    va_end(a); return n;
}
int puts(const char *s) { emit_line(s); return 1; }
int fputs(const char *__restrict s, FILE *__restrict f) {
    if (f == stdout || f == stderr) { emit_line(s); return 1; }
    return fprintf(f, "%s", s) < 0 ? -1 : 1;
}
// The SDK reports driver errors (draw dropped, present failed, ...) through the kernel debug output; route them to the UDP log too.
int sceKernelDebugOutText(int channel, const char *text) { (void)channel; if (text) { char b[300]; snprintf(b, sizeof b, "[klog] %s", text); emit_line(b); } return 0; }

// ---------------- overlay: software cursor + on-screen keyboard ----------------
// Minecraft shows no cursor of its own (it relies on the OS pointer) and GLFW has no text input here, so the shim draws both: an arrow at (g_cx, g_cy) while the
// window is not in GLFW_CURSOR_DISABLED mode (= menus), and an on-screen keyboard (open with R3 or the touchpad click in a menu/chat) that feeds MC's char and key
// callbacks. Everything is built on the CPU as coloured triangles (5x7 bitmap font, row-run merged) and drawn right before eglSwapBuffers into the default
// framebuffer with a private program/VAO/VBO; every piece of GL state touched is put back (MC caches GL state on the Java side). Real GL entry points are used.
static double g_cur_t;
static int g_polled, g_cur_vis;   // g_cur_vis: the cursor is drawn only after the controller moved/clicked it in the current menu (not on the startup screen)
static double g_ovt_s; static int g_cur_n;   // overlay time stat (was g_cur_t, which the pad code reuses as the last-activity time: absurd ms/frame)
static void *glr(const char *n) { return (void *)eglGetProcAddress(n); }
#define GLCALL(ret, name, ...) ({ static void *p_; if (!p_) p_ = glr(name); (ret (*)(__VA_ARGS__))p_; })   // one lookup per call site
static const unsigned char font5x7[95][5] = {   // ASCII 32..126, column bytes, bit0 = top row (classic 5x7 font, public domain)
    {0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x5F,0x00,0x00},
    {0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},
    {0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x56,0x20,0x50},
    {0x00,0x08,0x07,0x03,0x00},
    {0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00},
    {0x2A,0x1C,0x7F,0x1C,0x2A},
    {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x80,0x70,0x30,0x00},
    {0x08,0x08,0x08,0x08,0x08},
    {0x00,0x00,0x60,0x60,0x00},
    {0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},
    {0x00,0x42,0x7F,0x40,0x00},
    {0x72,0x49,0x49,0x49,0x46},
    {0x21,0x41,0x49,0x4D,0x33},
    {0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x31},
    {0x41,0x21,0x11,0x09,0x07},
    {0x36,0x49,0x49,0x49,0x36},
    {0x46,0x49,0x49,0x29,0x1E},
    {0x00,0x00,0x14,0x00,0x00},
    {0x00,0x40,0x34,0x00,0x00},
    {0x00,0x08,0x14,0x22,0x41},
    {0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08},
    {0x02,0x01,0x59,0x09,0x06},
    {0x3E,0x41,0x5D,0x59,0x4E},
    {0x7C,0x12,0x11,0x12,0x7C},
    {0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x41,0x3E},
    {0x7F,0x49,0x49,0x49,0x41},
    {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x41,0x51,0x73},
    {0x7F,0x08,0x08,0x08,0x7F},
    {0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},
    {0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x1C,0x02,0x7F},
    {0x7F,0x04,0x08,0x10,0x7F},
    {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},
    {0x3E,0x41,0x51,0x21,0x5E},
    {0x7F,0x09,0x19,0x29,0x46},
    {0x26,0x49,0x49,0x49,0x32},
    {0x03,0x01,0x7F,0x01,0x03},
    {0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},
    {0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63},
    {0x03,0x04,0x78,0x04,0x03},
    {0x61,0x59,0x49,0x4D,0x43},
    {0x00,0x7F,0x41,0x41,0x41},
    {0x02,0x04,0x08,0x10,0x20},
    {0x00,0x41,0x41,0x41,0x7F},
    {0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40},
    {0x00,0x03,0x07,0x08,0x00},
    {0x20,0x54,0x54,0x78,0x40},
    {0x7F,0x28,0x44,0x44,0x38},
    {0x38,0x44,0x44,0x44,0x28},
    {0x38,0x44,0x44,0x28,0x7F},
    {0x38,0x54,0x54,0x54,0x18},
    {0x00,0x08,0x7E,0x09,0x02},
    {0x18,0xA4,0xA4,0x9C,0x78},
    {0x7F,0x08,0x04,0x04,0x78},
    {0x00,0x44,0x7D,0x40,0x00},
    {0x20,0x40,0x40,0x3D,0x00},
    {0x7F,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7F,0x40,0x00},
    {0x7C,0x04,0x78,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78},
    {0x38,0x44,0x44,0x44,0x38},
    {0xFC,0x18,0x24,0x24,0x18},
    {0x18,0x24,0x24,0x18,0xFC},
    {0x7C,0x08,0x04,0x04,0x08},
    {0x48,0x54,0x54,0x54,0x24},
    {0x04,0x04,0x3F,0x44,0x24},
    {0x3C,0x40,0x40,0x20,0x7C},
    {0x1C,0x20,0x40,0x20,0x1C},
    {0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44},
    {0x4C,0x90,0x90,0x90,0x7C},
    {0x44,0x64,0x54,0x4C,0x44},
    {0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x77,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00},
    {0x02,0x01,0x02,0x04,0x02}
};
typedef struct { float x, y, r, g, b, a; } OvV;
#define OV_MAX 48000   /* the keyboard (dirt grain, 50 buttons, 3 lines of hints, every text twice for its shadow) needs about 30000 */
static OvV g_ov[OV_MAX]; static int g_ov_n;
static void ov_tri(float x0, float y0, float x1, float y1, float x2, float y2, float r, float g, float b, float a) {
    if (g_ov_n + 3 > OV_MAX) return;
    OvV *v = g_ov + g_ov_n; g_ov_n += 3;
    v[0] = (OvV){ x0, y0, r, g, b, a }; v[1] = (OvV){ x1, y1, r, g, b, a }; v[2] = (OvV){ x2, y2, r, g, b, a };
}
static void ov_rect(float x, float y, float w, float h, float r, float g, float b, float a) {
    ov_tri(x, y, x + w, y, x, y + h, r, g, b, a); ov_tri(x + w, y, x + w, y + h, x, y + h, r, g, b, a);
}
static const unsigned char glyph_sect[5] = {0x0A,0x55,0x55,0x55,0x28};   /* the section sign, 5x7 (not in the ASCII font) */
static float ov_text_w(const char *s, int scale) { return (float)strlen(s) * 6 * scale - scale; }
static void ov_text(float x, float y, int scale, const char *s, float r, float g, float b, float a) {
    for (; *s; s++) {
        unsigned c = (unsigned char)*s; if (c != 0xA7 && (c < 32 || c > 126)) c = '?';
        const unsigned char *gl = c == 0xA7 ? glyph_sect : font5x7[c - 32];
        for (int row = 0; row < 8; row++) {
            int run = -1;
            for (int col = 0; col <= 5; col++) {
                int on = col < 5 && ((gl[col] >> row) & 1);
                if (on && run < 0) run = col;
                if (!on && run >= 0) { ov_rect(x + run * scale, y + row * scale, (float)((col - run) * scale), (float)scale, r, g, b, a); run = -1; }
            }
        }
        x += 6 * scale;
    }
}
// ---- on-screen keyboard: layout and state (Minecraft look: a dirt panel with stone buttons) ----
typedef struct { char c, sc; int sp, w; const char *lab; } OskK;   // sp: 0 character, 1 shift, 2 backspace, 3 enter, 4 space, 5 letters / symbols page; w = width in key units
#define KC(c, sc) { c, sc, 0, 1, 0 }
#define KS(sp, w, lab) { 0, 0, sp, w, lab }
#define SECT '\xA7'   /* the section sign (Minecraft's colour / format code): drawn by a glyph of its own below */
static const OskK osk_rows[2][5][11] = {
  { /* letters */
    { KC('1','!'), KC('2','@'), KC('3','#'), KC('4','$'), KC('5','%'), KC('6','^'), KC('7','&'), KC('8','*'), KC('9','('), KC('0',')'), {0,0,0,0,0} },
    { KC('q','Q'), KC('w','W'), KC('e','E'), KC('r','R'), KC('t','T'), KC('y','Y'), KC('u','U'), KC('i','I'), KC('o','O'), KC('p','P'), {0,0,0,0,0} },
    { KC('a','A'), KC('s','S'), KC('d','D'), KC('f','F'), KC('g','G'), KC('h','H'), KC('j','J'), KC('k','K'), KC('l','L'), KC('~','`'), {0,0,0,0,0} },
    { KC('z','Z'), KC('x','X'), KC('c','C'), KC('v','V'), KC('b','B'), KC('n','N'), KC('m','M'), KC(',','"'), KC('.','\''), KC('/','?'), {0,0,0,0,0} },
    { KS(1,2,"SHIFT"), KS(5,2,"#+="), KC('@','@'), KS(4,2,"SPACE"), KS(2,1,"BKSP"), KS(3,2,"ENTER"), {0,0,0,0,0} },
  },
  { /* symbols: every punctuation mark of the keyboard plus the section sign (§1 .. §f colours, §l bold, §r reset) */
    { KC('1','!'), KC('2','@'), KC('3','#'), KC('4','$'), KC('5','%'), KC('6','^'), KC('7','&'), KC('8','*'), KC('9','('), KC('0',')'), {0,0,0,0,0} },
    { KC('-','-'), KC('_','_'), KC('=','='), KC('+','+'), KC('[','['), KC(']',']'), KC('{','{'), KC('}','}'), KC('<','<'), KC('>','>'), {0,0,0,0,0} },
    { KC('/','/'), KC('\\','\\'), KC('|','|'), KC(':',':'), KC(';',';'), KC('"','"'), KC('\'','\''), KC('~','~'), KC('`','`'), KC('?','?'), {0,0,0,0,0} },
    { KC(SECT,SECT), KC('&','&'), KC('@','@'), KC('#','#'), KC('%','%'), KC('^','^'), KC('*','*'), KC('(','('), KC(')',')'), KC('!','!'), {0,0,0,0,0} },
    { KS(1,2,"SHIFT"), KS(5,2,"ABC"), KC('@','@'), KS(4,2,"SPACE"), KS(2,1,"BKSP"), KS(3,2,"ENTER"), {0,0,0,0,0} },
  },
};
#define OSKR(r) (osk_rows[g_osk_page][r])
static float g_osk_ox, g_osk_oy;   // panel offset from its docked spot, moved with the right stick
static int g_osk_open, g_osk_shift, g_osk_lock, g_osk_l2, g_osk_row = 1, g_osk_k = 0, g_osk_top, g_osk_page;   // shift = one-shot (SHIFT key), lock = caps lock (double-tap L2), l2 = L2 held, page 0 letters / 1 symbols
#define OSK_UP() (g_osk_shift || g_osk_lock || g_osk_l2)
static int osk_nkeys(int row) { int n = 0; while (OSKR(row)[n].c || OSKR(row)[n].sp) n++; return n; }
static float osk_center(int row, int k) { float x = 0; for (int i = 0; i < k; i++) x += OSKR(row)[i].w; return x + OSKR(row)[k].w / 2.0f; }
#define UIK (g_w / 1920.0f)   /* the overlay is drawn in screen pixels: 1440p and 4K scale it so it is as big on the screen as at 1080p */
#define OSK_KW (70.0f * UIK)
#define OSK_KH (56.0f * UIK)
#define OSK_GAP (6.0f * UIK)
static int osk_sc(int sc) { return (int)(sc * UIK + 0.5f); }
#define OSK_HINTS 3
static void osk_text_sh(float x, float y, int sc, const char *t, float r, float g, float b) { sc = osk_sc(sc); ov_text(x + sc, y + sc, sc, t, r * 0.18f, g * 0.18f, b * 0.18f, 1); ov_text(x, y, sc, t, r, g, b, 1); }   // Minecraft text: a dark drop shadow under it
static void osk_button(float x, float y, float w, float h, float r, float g, float b) {   // a Minecraft button: black outline, light top-left edge, dark bottom-right edge
    ov_rect(x, y, w, h, 0.03f, 0.03f, 0.03f, 1);
    ov_rect(x + 2, y + 2, w - 4, h - 4, r, g, b, 1);
    ov_rect(x + 2, y + 2, w - 4, 3, r + 0.22f, g + 0.22f, b + 0.22f, 1); ov_rect(x + 2, y + 2, 3, h - 4, r + 0.22f, g + 0.22f, b + 0.22f, 1);
    ov_rect(x + 2, y + h - 6, w - 4, 4, r * 0.55f, g * 0.55f, b * 0.55f, 1); ov_rect(x + w - 6, y + 2, 4, h - 4, r * 0.55f, g * 0.55f, b * 0.55f, 1);
}
static const char *const osk_hint[OSK_HINTS] = {   /* each line must fit the panel (the test measures them) */
    "CROSS TYPE  SQUARE BKSP  TRIANGLE SPACE  OPTIONS ENTER",
    "L2 SHIFT (TWICE: CAPS)  R2 SYMBOLS  L1 R1 CURSOR",
    "R STICK MOVE  L3 DOCK TOP BOTTOM  R3 RESET  CIRCLE CLOSE" };
static void osk_build(void) {
    float pw = 10 * OSK_KW + 11 * OSK_GAP + 24 * UIK, ph = (40 + 10 + OSK_HINTS * 20 + 8 + OSK_GAP / UIK) * UIK + 5 * (OSK_KH + OSK_GAP);
    float px = (g_w - pw) / 2, py = g_osk_top ? 24.0f * UIK : g_h - ph - 24.0f * UIK;
    if (px + g_osk_ox < 0) g_osk_ox = -px; if (px + g_osk_ox + pw > g_w) g_osk_ox = g_w - pw - px;
    if (py + g_osk_oy < 0) g_osk_oy = -py; if (py + g_osk_oy + ph > g_h) g_osk_oy = g_h - ph - py;
    px += g_osk_ox; py += g_osk_oy;
    // the panel: dirt, with the grain of the block (a fixed pattern, so the picture is still when nothing moves)
    ov_rect(px - 3, py - 3, pw + 6, ph + 6, 0.02f, 0.02f, 0.02f, 1);
    ov_rect(px, py, pw, ph, 0.29f, 0.20f, 0.12f, 1);
    for (int gy = 0; gy * 16 + 18 < (int)ph - 4; gy++) for (int gx = 0; gx * 16 + 18 < (int)pw - 4; gx++) {
        unsigned h = (unsigned)(gx * 73856093u) ^ (unsigned)(gy * 19349663u); h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
        if (h % 5) continue;
        float d = (h >> 9) & 1 ? 0.70f : 1.28f, sz = 6.0f + (float)((h >> 12) & 3) * 2.0f;
        ov_rect(px + 4 + gx * 16 + (float)((h >> 16) & 7), py + 4 + gy * 16 + (float)((h >> 20) & 7), sz, sz, 0.29f * d, 0.20f * d, 0.12f * d, 1);
    }
    ov_rect(px, py, pw, 3, 0.45f, 0.33f, 0.20f, 1); ov_rect(px, py, 3, ph, 0.45f, 0.33f, 0.20f, 1);
    ov_rect(px, py + ph - 3, pw, 3, 0.14f, 0.09f, 0.05f, 1); ov_rect(px + pw - 3, py, 3, ph, 0.14f, 0.09f, 0.05f, 1);
    osk_text_sh(px + 14 * UIK, py + 12 * UIK, 2, "ON-SCREEN KEYBOARD", 1.0f, 1.0f, 1.0f);
    { const char *mode = g_osk_page ? "SYMBOLS" : g_osk_lock ? "CAPS LOCK" : (g_osk_shift || g_osk_l2) ? "SHIFT" : "ABC";
      osk_text_sh(px + pw - 14 * UIK - ov_text_w(mode, osk_sc(2)), py + 12 * UIK, 2, mode, 1.0f, 0.85f, 0.25f); }
    for (int r = 0; r < 5; r++) {
        float x = px + 12 * UIK + OSK_GAP, y = py + 40 * UIK + r * (OSK_KH + OSK_GAP);
        for (int k = 0; OSKR(r)[k].c || OSKR(r)[k].sp; k++) {
            const OskK *K = &OSKR(r)[k]; float w = K->w * OSK_KW + (K->w - 1) * OSK_GAP;
            int focus = (r == g_osk_row && k == g_osk_k);
            float cr = 0.45f, cg = 0.45f, cb = 0.45f;                 // stone grey, like Minecraft's buttons
            if (K->sp == 1 && OSK_UP() && !g_osk_page) { cr = 0.62f; cg = 0.50f; cb = 0.16f; }   // shift is on: gold
            if (K->sp == 5) { cr = 0.36f; cg = 0.40f; cb = 0.50f; }
            if (K->sp == 3) { cr = 0.28f; cg = 0.50f; cb = 0.24f; }    // ENTER: green
            if (focus) { cr = 0.30f; cg = 0.58f; cb = 0.20f; }        // the key under the cursor: grass green, white frame
            osk_button(x, y, w, OSK_KH, cr, cg, cb);
            if (focus) { ov_rect(x - 3, y - 3, w + 6, 3, 1, 1, 1, 1); ov_rect(x - 3, y + OSK_KH, w + 6, 3, 1, 1, 1, 1); ov_rect(x - 3, y, 3, OSK_KH, 1, 1, 1, 1); ov_rect(x + w, y, 3, OSK_KH, 1, 1, 1, 1); }
            char lab[2] = { 0, 0 }; const char *t; int sc = 3;
            if (K->sp) { t = K->lab; sc = 2; } else { lab[0] = OSK_UP() ? K->sc : K->c; t = lab; }
            osk_text_sh(x + (w - ov_text_w(t, osk_sc(sc))) / 2, y + (OSK_KH - 7 * osk_sc(sc)) / 2, sc, t, focus ? 1.0f : 0.92f, focus ? 1.0f : 0.92f, focus ? 0.63f : 0.92f);   // hovered text is yellow, like Minecraft
            x += w + OSK_GAP;
        }
    }
    float fy = py + 40 * UIK + 5 * (OSK_KH + OSK_GAP) + 10 * UIK;
    for (int i = 0; i < OSK_HINTS; i++) osk_text_sh(px + 14 * UIK, fy + i * 20 * UIK, 2, osk_hint[i], 0.85f, 0.85f, 0.85f);
}
// Minecraft Bedrock's pointer: the 16x16 sprite (only the top-left 9x12 is used), hotspot = top-left pixel. Each letter is a palette entry, '.' is transparent.
static const char *const cur_art[] = {
    "ab.......",
    "acb......",
    "adcb.....",
    "aedcb....",
    "abeddb...",
    "abedfdb..",
    "abbedfdb.",
    "abbedfeb.",
    "abbbaaag.",
    "abbagggg.",
    "aaagg....",
    ".ggg....." };
static void cur_color(char k, float *r, float *g, float *b) {
    static const unsigned char pal[7][3] = { { 68, 48, 40 }, { 92, 75, 66 }, { 206, 188, 165 }, { 166, 148, 129 }, { 126, 107, 93 }, { 146, 128, 111 }, { 40, 23, 16 } };   // a..g
    const unsigned char *p = pal[(k >= 'a' && k <= 'g') ? k - 'a' : 0];
    *r = p[0] / 255.0f; *g = p[1] / 255.0f; *b = p[2] / 255.0f;
}
static int g_cur_scale = 3;   // launcher.cfg cursor=2..6 (Settings > Mouse > Cursor size): screen pixels per sprite pixel at 1080p; 3 is the game's own size
static void cursor_build(void) {
    if (!g_cur_vis) return;
    const int S = osk_sc(g_cur_scale), rows = (int)(sizeof cur_art / sizeof cur_art[0]);   // S = screen pixels per sprite pixel (3 = about 36 px tall at 1080p)
    for (int r = 0; r < rows; r++) {
        const char *row = cur_art[r]; int w = (int)strlen(row), c = 0;
        while (c < w) {
            if (row[c] == '.') { c++; continue; }
            int c0 = c; char k = row[c]; while (c < w && row[c] == k) c++;
            float cr, cg, cb; cur_color(k, &cr, &cg, &cb);
            ov_rect((float)g_cx + c0 * S, (float)g_cy + r * S, (float)((c - c0) * S), (float)S, cr, cg, cb, 1.0f);
        }
    }
}
static int g_ov_ok, g_ov_failed, g_ov_same, g_ov_off_frames; static double g_ov_query_ms; static unsigned g_ov_prog, g_ov_vao, g_ov_vbo; static int g_ov_loc_res = -1;
static unsigned ov_shader(int type, const char *src) {
    unsigned sh = GLCALL(unsigned, "glCreateShader", int)(type); const char *p[1] = { src };
    GLCALL(void, "glShaderSource", unsigned, int, const char *const *, const int *)(sh, 1, p, 0);
    GLCALL(void, "glCompileShader", unsigned)(sh);
    int ok = 0; GLCALL(void, "glGetShaderiv", unsigned, int, int *)(sh, 0x8B81, &ok);
    if (!ok) { char log[200] = {0}; GLCALL(void, "glGetShaderInfoLog", unsigned, int, int *, char *)(sh, sizeof log - 1, 0, log); TR("overlay: shader compile failed: %.150s", log); return 0; }
    return sh;
}
static void overlay_init(void) {
    g_ov_failed = 1;
    static const char *vs = "#version 150\nin vec2 aPos;\nin vec4 aCol;\nuniform vec2 uRes;\nout vec4 vC;\nvoid main() { vec2 p = aPos / uRes * 2.0 - 1.0; gl_Position = vec4(p.x, -p.y, 0.0, 1.0); vC = aCol; }\n";
    static const char *fs = "#version 150\nin vec4 vC;\nout vec4 o;\nvoid main() { o = vC; }\n";
    unsigned v = ov_shader(0x8B31, vs), f = ov_shader(0x8B30, fs); if (!v || !f) return;
    g_ov_prog = GLCALL(unsigned, "glCreateProgram", void)();
    GLCALL(void, "glAttachShader", unsigned, unsigned)(g_ov_prog, v); GLCALL(void, "glAttachShader", unsigned, unsigned)(g_ov_prog, f);
    GLCALL(void, "glBindAttribLocation", unsigned, unsigned, const char *)(g_ov_prog, 0, "aPos");
    GLCALL(void, "glBindAttribLocation", unsigned, unsigned, const char *)(g_ov_prog, 1, "aCol");
    GLCALL(void, "glLinkProgram", unsigned)(g_ov_prog);
    int ok = 0; GLCALL(void, "glGetProgramiv", unsigned, int, int *)(g_ov_prog, 0x8B82, &ok);
    if (!ok) { TR("overlay: program link failed"); return; }
    g_ov_loc_res = GLCALL(int, "glGetUniformLocation", unsigned, const char *)(g_ov_prog, "uRes");
    int prev_vao = 0, prev_abuf = 0;
    GLCALL(void, "glGetIntegerv", int, int *)(0x85B5, &prev_vao); GLCALL(void, "glGetIntegerv", int, int *)(0x8894, &prev_abuf);
    GLCALL(void, "glGenVertexArrays", int, unsigned *)(1, &g_ov_vao); GLCALL(void, "glGenBuffers", int, unsigned *)(1, &g_ov_vbo);
    GLCALL(void, "glBindVertexArray", unsigned)(g_ov_vao); GLCALL(void, "glBindBuffer", int, unsigned)(0x8892, g_ov_vbo);
    GLCALL(void, "glBufferData", int, long, const void *, int)(0x8892, (long)sizeof g_ov, 0, 0x88E0);   // allocate once, rewrite per frame
    GLCALL(void, "glEnableVertexAttribArray", unsigned)(0); GLCALL(void, "glEnableVertexAttribArray", unsigned)(1);
    GLCALL(void, "glVertexAttribPointer", unsigned, int, int, int, int, const void *)(0, 2, 0x1406, 0, 24, (const void *)0);
    GLCALL(void, "glVertexAttribPointer", unsigned, int, int, int, int, const void *)(1, 4, 0x1406, 0, 24, (const void *)8);
    GLCALL(void, "glBindVertexArray", unsigned)((unsigned)prev_vao); GLCALL(void, "glBindBuffer", int, unsigned)(0x8892, (unsigned)prev_abuf);
    g_ov_ok = 1; g_ov_failed = 0;
    TR("overlay: ready (prog %u, buffer %zu KB, uRes=%d)", g_ov_prog, sizeof g_ov / 1024, g_ov_loc_res);
}
// seed (and occasionally re-check) the shadow from the driver; this is the expensive path, so it runs once and then very rarely
static void sh_sync(int verify) {
    void (*gi)(int, int *) = GLCALL(void, "glGetIntegerv", int, int *);
    unsigned char (*ie)(int) = GLCALL(unsigned char, "glIsEnabled", int);
    int dfbo = 0, rfbo = 0, vp[4] = { 0, 0, 0, 0 }, prog = 0, vao = 0, abuf = 0, bf[4] = { 0, 0, 0, 0 }; unsigned char cm[4] = { 1, 1, 1, 1 };
    gi(0x8CA6, &dfbo); gi(0x8CAA, &rfbo); gi(0x0BA2, vp); gi(0x8B8D, &prog); gi(0x85B5, &vao); gi(0x8894, &abuf);
    gi(0x80C9, &bf[0]); gi(0x80C8, &bf[1]); gi(0x80CB, &bf[2]); gi(0x80CA, &bf[3]);
    GLCALL(void, "glGetBooleanv", int, unsigned char *)(0x0C23, cm);
    int blend = ie(0x0BE2), depth = ie(0x0B71), cull = ie(0x0B44), scis = ie(0x0C11);
    if (verify && g_sh.seeded) {
        int bad = (g_sh.dfbo != dfbo) + (g_sh.prog != prog) + (g_sh.vao != vao) + (g_sh.abuf != abuf) + (g_sh.blend != blend) + (g_sh.depth != depth) + (g_sh.cull != cull) + (g_sh.scis != scis)
                + (g_sh.vp[2] != vp[2]) + (g_sh.vp[3] != vp[3]);
        if (bad) TR("overlay: shadow state differed from the driver in %d of 10 places (fbo %d/%d prog %d/%d vao %d/%d blend %d/%d depth %d/%d): corrected", bad, g_sh.dfbo, dfbo, g_sh.prog, prog, g_sh.vao, vao, g_sh.blend, blend, g_sh.depth, depth);
    }
    g_sh.dfbo = dfbo; g_sh.rfbo = rfbo; memcpy(g_sh.vp, vp, sizeof vp); g_sh.prog = prog; g_sh.vao = vao; g_sh.abuf = abuf; memcpy(g_sh.bf, bf, sizeof bf); memcpy(g_sh.cm, cm, sizeof cm);
    g_sh.blend = blend; g_sh.depth = depth; g_sh.cull = cull; g_sh.scis = scis; g_sh.seeded = 1;
}

// ---- stats overlay (launcher.cfg hud=on, hudpos=tl|tc|tr|bl|bc|br): FPS with the VSync state, the resolution with the refresh rate, the renderer. Drawn with the same triangles as the cursor, on top of every frame ----
static volatile int g_kit_ui;   /* the native launcher is on screen: it draws its own stats overlay, the shim's must stay hidden */
void ps5_hud_kit(int on) { g_kit_ui = on; }
int ps5_hud_hz(void);
static int g_hud_bg = 4;   /* launcher.cfg hudbg=0..5: background opacity of the stats panel */
static int g_ov_menu = 1, g_hud_on, g_hud_pos = 0, g_vsync = 1, g_use_gl;   /* g_use_gl: tentative here, set by ps5_gl_probe further down */
static void hud_build(void) {
    static double t0; static int frames; static char l1[64], l2[64], l3[48]; static float fr, fg, fb;
    double t = now_s(); frames++;
    if (t0 == 0) t0 = t;
    if (t - t0 >= 0.5 || !l1[0]) {
        int hz = ps5_hud_hz();
        double fps = t - t0 > 0 ? frames / (t - t0) : 0; if (!l1[0] && fps == 0) fps = hz;
        snprintf(l1, sizeof l1, "FPS: %d (VSync %s)", (int)(fps + 0.5), g_vsync ? "On" : "Off");
        snprintf(l2, sizeof l2, "Resolution: %dx%d %dHz", g_w, g_h, hz);
        snprintf(l3, sizeof l3, "Renderer: %s", g_zk ? "Zink" : g_use_gl ? "OpenGL" : "Vulkan");
        double rel = fps / hz; if (g_vsync == 0) rel = fps >= 55 ? 1 : fps / 55;
        if (rel >= 0.9) { fr = 0.45f; fg = 1.0f; fb = 0.45f; } else if (rel >= 0.5) { fr = 1.0f; fg = 0.85f; fb = 0.25f; } else { fr = 1.0f; fg = 0.35f; fb = 0.35f; }
        t0 = t; frames = 0;
    }
    int sc = osk_sc(2); float pad = 10.0f * UIK, lh = 11.0f * sc, m = 16.0f * UIK;
    const char *ls[3] = { l1, l2, l3 };
    float w = 0; for (int i = 0; i < 3; i++) { float tw = ov_text_w(ls[i], sc); if (tw > w) w = tw; }
    float pw = w + 2 * pad, ph = 3 * lh + 2 * pad - (lh - 8.0f * sc);
    int col = g_hud_pos % 3, bottom = g_hud_pos >= 3;   // 0 left, 1 centre, 2 right
    float x = col == 0 ? m : col == 1 ? (g_w - pw) / 2 : g_w - pw - m, y = bottom ? g_h - ph - m : m;
    { static const float bga[6] = { 0.0f, 0.2f, 0.4f, 0.55f, 0.72f, 0.92f }; float ba = bga[g_hud_bg < 0 ? 0 : g_hud_bg > 5 ? 5 : g_hud_bg];
      if (ba > 0) { ov_rect(x - 2, y - 2, pw + 4, ph + 4, 0, 0, 0, ba * 0.5f); ov_rect(x, y, pw, ph, 0.05f, 0.06f, 0.08f, ba); } }
    ov_rect(x, y, 3.0f * UIK + 1, ph, fr, fg, fb, 0.95f);   /* the bar on the left takes the colour of the frame rate: green steady, yellow below 90 %, red below half */
    for (int i = 0; i < 3; i++) {
        float tx = x + pad + 3.0f * UIK, ty = y + pad + i * lh;
        ov_text(tx + sc * 0.5f, ty + sc * 0.5f, sc, ls[i], 0, 0, 0, 0.8f);
        if (i == 0) ov_text(tx, ty, sc, ls[i], fr, fg, fb, 1); else ov_text(tx, ty, sc, ls[i], 0.92f, 0.94f, 0.97f, 1);
    }
}
static void cursor_draw(void) {   // name kept: called from glfwSwapBuffers; draws the cursor and the keyboard when open
    if (g_ov_failed) return;
    const int hud = g_hud_on && !g_kit_ui;
    const int menu = g_ov_menu;   // cursor and keyboard only in menus (glfwSwapBuffers decides); the stats overlay in every mode
    if (!(menu && (g_cur_vis || g_osk_open)) && !hud) { g_ov_off_frames++; return; }
    if (!g_ov_ok) { overlay_init(); if (!g_ov_ok) return; }
    double t0 = now_s();
    g_ov_n = 0;
    g_ov_off_frames = 0;
    if (menu && g_osk_open) osk_build();
    if (menu) cursor_build();
    if (hud) hud_build();
    if (g_ov_n == 0) return;
    {   // the overlay costs 6-9 ms per frame on this SDK (state queries, upload, draw): when its geometry is identical to the last frame and the screen has not changed, skip the upload
        static unsigned long last_hash; static int last_n;
        unsigned long hsh = 1469598103934665603UL;
        for (int i = 0; i < g_ov_n; i++) { const unsigned char *b = (const unsigned char *)&g_ov[i]; for (size_t k = 0; k < sizeof(OvV); k++) { hsh ^= b[k]; hsh *= 1099511628211UL; } }
        g_ov_same = (last_n == g_ov_n && last_hash == hsh); last_n = g_ov_n; last_hash = hsh;
    }
    // No state is read back here any more: every value below is kept up to date by the shadow wrappers above as Minecraft sets it (the 16 queries this used to make cost 16-58 ms per frame,
    // because each one syncs with the SDK's GL thread). The shadow is seeded once and re-checked every SH_VERIFY_EVERY overlay frames.
    static int ov_frames;
    double tq = now_s();
    if (!g_sh.seeded || ++ov_frames % SH_VERIFY_EVERY == 0) sh_sync(g_sh.seeded);
    g_ov_query_ms += 1e3 * (now_s() - tq);
    int dfbo = g_sh.dfbo, rfbo = g_sh.rfbo, prog = g_sh.prog, vao = g_sh.vao, abuf = g_sh.abuf; int vp[4] = { g_sh.vp[0], g_sh.vp[1], g_sh.vp[2], g_sh.vp[3] };
    int bf[4] = { g_sh.bf[0], g_sh.bf[1], g_sh.bf[2], g_sh.bf[3] }; unsigned char cm[4] = { g_sh.cm[0], g_sh.cm[1], g_sh.cm[2], g_sh.cm[3] };
    int e_blend = g_sh.blend, e_depth = g_sh.depth, e_cull = g_sh.cull, e_scis = g_sh.scis;
    void (*en)(int) = GLCALL(void, "glEnable", int), (*dis)(int) = GLCALL(void, "glDisable", int);
    (void)rfbo;
    GLCALL(void, "glBindFramebuffer", int, unsigned)(0x8D40, 0);
    GLCALL(void, "glViewport", int, int, int, int)(0, 0, g_w, g_h);
    if (!e_blend) en(0x0BE2); if (e_depth) dis(0x0B71); if (e_cull) dis(0x0B44); if (e_scis) dis(0x0C11);
    GLCALL(void, "glBlendFuncSeparate", int, int, int, int)(0x0302, 0x0303, 1, 0x0303);   // SRC_ALPHA, ONE_MINUS_SRC_ALPHA, ONE, ONE_MINUS_SRC_ALPHA
    GLCALL(void, "glColorMask", int, int, int, int)(1, 1, 1, 1);
    GLCALL(void, "glUseProgram", unsigned)(g_ov_prog);
    GLCALL(void, "glUniform2f", int, float, float)(g_ov_loc_res, (float)g_w, (float)g_h);
    GLCALL(void, "glBindVertexArray", unsigned)(g_ov_vao); GLCALL(void, "glBindBuffer", int, unsigned)(0x8892, g_ov_vbo);
    if (!g_ov_same) GLCALL(void, "glBufferSubData", int, long, long, const void *)(0x8892, 0, (long)g_ov_n * (long)sizeof(OvV), g_ov);
    GLCALL(void, "glDrawArrays", int, int, int)(4, 0, g_ov_n);
    // restore
    GLCALL(void, "glBindBuffer", int, unsigned)(0x8892, (unsigned)abuf); GLCALL(void, "glBindVertexArray", unsigned)((unsigned)vao); GLCALL(void, "glUseProgram", unsigned)((unsigned)prog);
    GLCALL(void, "glColorMask", int, int, int, int)(cm[0], cm[1], cm[2], cm[3]);
    GLCALL(void, "glBlendFuncSeparate", int, int, int, int)(bf[0], bf[1], bf[2], bf[3]);
    if (!e_blend) dis(0x0BE2); if (e_depth) en(0x0B71); if (e_cull) en(0x0B44); if (e_scis) en(0x0C11);
    GLCALL(void, "glViewport", int, int, int, int)(vp[0], vp[1], vp[2], vp[3]);
    GLCALL(void, "glBindFramebuffer", int, unsigned)(0x8CA9, (unsigned)dfbo); GLCALL(void, "glBindFramebuffer", int, unsigned)(0x8CA8, (unsigned)rfbo);
    g_ovt_s += now_s() - t0; g_cur_n++;
}
#ifndef NO_PRESENT_DRAW
// ---------------- present: main target -> screen as a draw (replaces the slow glBlitFramebuffer, see gw_bf_blit) ----------------
// The shader fetches texel (gl_FragCoord) of the main target's colour texture: no filtering, wrapping or mip state matters, orientation equals the blit's. The texture is bound with the DSA call
// glBindTextureUnit on unit 15 (not the active unit, so Minecraft's cached texture bindings stay valid) and unbound again. All other state is taken from the shadow and put back, as the overlay does.
static struct { unsigned prog, vao, tex; long fbo; int ok, failed; unsigned long frames, drawn; } g_pr;
static int present_init(void) {
    static const char *vs = "#version 150\nvoid main() { vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
    static const char *fs = "#version 150\nuniform sampler2D uTex;\nout vec4 o;\nvoid main() { o = texelFetch(uTex, ivec2(gl_FragCoord.xy), 0); }\n";
    if (!glr("glBindTextureUnit") || !glr("glGetFramebufferAttachmentParameteriv")) { TR("blitfix: glBindTextureUnit / glGetFramebufferAttachmentParameteriv missing: keeping the real blit"); return 0; }
    unsigned v = ov_shader(0x8B31, vs), f = ov_shader(0x8B30, fs); if (!v || !f) return 0;
    g_pr.prog = GLCALL(unsigned, "glCreateProgram", void)();
    GLCALL(void, "glAttachShader", unsigned, unsigned)(g_pr.prog, v); GLCALL(void, "glAttachShader", unsigned, unsigned)(g_pr.prog, f);
    GLCALL(void, "glLinkProgram", unsigned)(g_pr.prog);
    int ok = 0; GLCALL(void, "glGetProgramiv", unsigned, int, int *)(g_pr.prog, 0x8B82, &ok);
    if (!ok) { TR("blitfix: present program link failed: keeping the real blit"); return 0; }
    int loc = GLCALL(int, "glGetUniformLocation", unsigned, const char *)(g_pr.prog, "uTex");
    if (!g_sh.seeded) sh_sync(0);
    GLCALL(void, "glUseProgram", unsigned)(g_pr.prog); GLCALL(void, "glUniform1i", int, int)(loc, 15); GLCALL(void, "glUseProgram", unsigned)((unsigned)g_sh.prog);
    GLCALL(void, "glGenVertexArrays", int, unsigned *)(1, &g_pr.vao);
    g_pr.ok = 1; TR("blitfix: present program ready (prog %u, vao %u, texture unit 15)", g_pr.prog, g_pr.vao);
    return 1;
}
static int present_prepare(long fbo, long w, long h, int named) {   // named: the caller never bound fbo (glBlitNamedFramebuffer), so ask for its attachment by name
    if (g_pr.failed || w != g_w || h != g_h) return 0;
    if (!g_pr.ok && !present_init()) { g_pr.failed = 1; return 0; }
    if (!g_pr_valid || g_pr.fbo != fbo || ++g_pr.frames % 600 == 0) {   // one driver query (it syncs with the SDK's GL thread): on the first blit, after an attachment change, and rarely otherwise
        double tq0 = now_s(); int type = 0, name = 0; void (*ga)(int, int, int, int *) = GLCALL(void, "glGetFramebufferAttachmentParameteriv", int, int, int, int *);
        if (named) { void (*gn)(unsigned, int, int, int *) = GLCALL(void, "glGetNamedFramebufferAttachmentParameteriv", unsigned, int, int, int *); gn((unsigned)fbo, 0x8CE0, 0x8CD0, &type); gn((unsigned)fbo, 0x8CE0, 0x8CD1, &name); }
        else { ga(0x8CA8, 0x8CE0, 0x8CD0, &type); ga(0x8CA8, 0x8CE0, 0x8CD1, &name); }
        if (type != 0x1702 || name <= 0) { TR("blitfix: FBO %ld colour attachment is not a texture (type 0x%x, name %d): keeping the real blit", fbo, type, name); g_pr.failed = 1; return 0; }
        if ((unsigned)name != g_pr.tex || g_pr.fbo != fbo) TR("blitfix: present source = FBO %ld, colour texture %d", fbo, name);
        g_pr.fbo = fbo; g_pr.tex = (unsigned)name; g_pr_valid = 1; g_pr_cfbo = fbo; g_pr_ctex = (unsigned)name;
        { static unsigned long nq; static double tq; nq++; tq += now_s() - tq0; if (nq <= 3 || nq % 500 == 0) TR("blitfix: %lu attachment queries so far, avg %.2f ms each (%lu presents)", nq, 1e3 * tq / nq, g_pr.drawn); }
    }
    return 1;
}
static void present_draw(void) {
    if (!g_pr_pending) return;
    g_pr_pending = 0;
    if (!g_sh.seeded) sh_sync(0);
    int dfbo = g_sh.dfbo, rfbo = g_sh.rfbo, prog = g_sh.prog, vao = g_sh.vao; int vp[4] = { g_sh.vp[0], g_sh.vp[1], g_sh.vp[2], g_sh.vp[3] }; unsigned char cm[4] = { g_sh.cm[0], g_sh.cm[1], g_sh.cm[2], g_sh.cm[3] };
    int e_blend = g_sh.blend, e_depth = g_sh.depth, e_cull = g_sh.cull, e_scis = g_sh.scis;
    void (*en)(int) = GLCALL(void, "glEnable", int), (*dis)(int) = GLCALL(void, "glDisable", int);
    GLCALL(void, "glBindFramebuffer", int, unsigned)(0x8D40, 0);
    GLCALL(void, "glViewport", int, int, int, int)(0, 0, g_w, g_h);
    if (e_blend) dis(0x0BE2); if (e_depth) dis(0x0B71); if (e_cull) dis(0x0B44); if (e_scis) dis(0x0C11);
    GLCALL(void, "glColorMask", int, int, int, int)(1, 1, 1, 0);   // RGB only, like vanilla's own present (blitToScreen uses colorMask(true,true,true,false)): the main target's alpha is arbitrary after blending and must not reach the screen buffer
    GLCALL(void, "glBindTextureUnit", unsigned, unsigned)(15, g_pr_ctex);
    GLCALL(void, "glUseProgram", unsigned)(g_pr.prog); GLCALL(void, "glBindVertexArray", unsigned)(g_pr.vao);
    GLCALL(void, "glDrawArrays", int, int, int)(4, 0, 3);
    GLCALL(void, "glBindTextureUnit", unsigned, unsigned)(15, 0);
    GLCALL(void, "glBindVertexArray", unsigned)((unsigned)vao); GLCALL(void, "glUseProgram", unsigned)((unsigned)prog);
    GLCALL(void, "glColorMask", int, int, int, int)(cm[0], cm[1], cm[2], cm[3]);
    if (e_blend) en(0x0BE2); if (e_depth) en(0x0B71); if (e_cull) en(0x0B44); if (e_scis) en(0x0C11);
    GLCALL(void, "glViewport", int, int, int, int)(vp[0], vp[1], vp[2], vp[3]);
    GLCALL(void, "glBindFramebuffer", int, unsigned)(0x8CA9, (unsigned)dfbo); GLCALL(void, "glBindFramebuffer", int, unsigned)(0x8CA8, (unsigned)rfbo);
    if (++g_pr.drawn == 1 || g_pr.drawn % 2000 == 0) TR("blitfix: %lu presents drawn as a textured triangle instead of glBlitFramebuffer", g_pr.drawn);
}
#endif
// ---- capture probe (called by the GL profiler on a few draws of the captured frame) ----
static void cap_probe(int idx, long tex) {
    int vw = (int)(g_vp_w > 0 ? g_vp_w : g_w), vh = (int)(g_vp_h > 0 ? g_vp_h : g_h);
    int pts[5][2] = { { vw / 2, vh / 2 }, { vw / 4, vh / 2 }, { 3 * vw / 4, vh / 2 }, { vw / 2, vh / 4 }, { vw / 2, 3 * vh / 4 } };
    unsigned char px[5][4]; memset(px, 0, sizeof px);
    for (int i = 0; i < 5; i++) GLCALL(void, "glReadPixels", int, int, int, int, int, int, void *)(pts[i][0], pts[i][1], 1, 1, 0x1908, 0x1401, px[i]);
    { unsigned ge = GLCALL(unsigned, "glGetError", void)(); if (ge) TR("cap probe: glGetError after draw #%d = 0x%x", idx, ge); }
    TR("cap probe after draw #%d: fbo=%ld pixels centre=%u,%u,%u,%u  left=%u,%u,%u,%u  right=%u,%u,%u,%u  up=%u,%u,%u,%u  down=%u,%u,%u,%u", idx, g_cur_fbo,
       px[0][0], px[0][1], px[0][2], px[0][3], px[1][0], px[1][1], px[1][2], px[1][3], px[2][0], px[2][1], px[2][2], px[2][3], px[3][0], px[3][1], px[3][2], px[3][3], px[4][0], px[4][1], px[4][2], px[4][3]);
    if (tex > 0) {
        static long seen[16]; static int nseen; for (int i = 0; i < nseen; i++) if (seen[i] == tex) return;
        if (nseen < 16) seen[nseen++] = tex;
        int prev = 0, w = 0, h = 0, fmt = 0, minf = 0, magf = 0, wrap = 0, base = 0, maxl = 0;
        GLCALL(void, "glGetIntegerv", int, int *)(0x8069, &prev);
        GLCALL(void, "glBindTexture", int, unsigned)(0x0DE1, (unsigned)tex);
        void (*lv)(int, int, int, int *) = GLCALL(void, "glGetTexLevelParameteriv", int, int, int, int *); void (*tp)(int, int, int *) = GLCALL(void, "glGetTexParameteriv", int, int, int *);
        lv(0x0DE1, 0, 0x1000, &w); lv(0x0DE1, 0, 0x1001, &h); lv(0x0DE1, 0, 0x1003, &fmt);
        tp(0x0DE1, 0x2801, &minf); tp(0x0DE1, 0x2800, &magf); tp(0x0DE1, 0x2802, &wrap); tp(0x0DE1, 0x813C, &base); tp(0x0DE1, 0x813D, &maxl);
        GLCALL(void, "glBindTexture", int, unsigned)(0x0DE1, (unsigned)prev);
        TR("cap probe texture %ld: %dx%d internalformat=0x%x min=0x%x mag=0x%x wrapS=0x%x baseLevel=%d maxLevel=%d", tex, w, h, fmt, minf, magf, wrap, base, maxl);
        if (w * h <= 4 && w * h > 0) { unsigned char *(*dummy)(void) = 0; (void)dummy; void *gt = glr("glGetTexImage"); unsigned char px[16] = { 0 }; if (gt) { ((void (*)(int, int, int, int, void *))gt)(0x0DE1, 0, 0x1908, 0x1401, px); TR("cap probe texture %ld texel[0]=%u,%u,%u,%u", tex, px[0], px[1], px[2], px[3]); } else TR("cap probe: glGetTexImage not available"); }
    }
}

#ifdef PS5_GPUTIME
// ---------------- GPU timestamps (build with GPUTIME=1) ----------------
// Real GPU time instead of guesses: glQueryCounter(GL_TIMESTAMP) marks are inserted at the start and end of every 60th frame and around every merged chunk draw (ps5mega.inc), and their
// results are read 3 frames later (glGetQueryObject* waits for the GL thread, so this costs one stall per 60 frames). "frame timeline" is the GPU timeline from the first to the last mark of
// the frame (it includes the GPU waiting for the CPU to feed it); "merged draws" is pure GPU execution time of the merged chunk draws.
#define GT_MARKS 48
#define GT_SLOTS 4
static unsigned gt_ids[GT_SLOTS][GT_MARKS]; static int gt_tag[GT_SLOTS][GT_MARKS], gt_n[GT_SLOTS], gt_full[GT_SLOTS]; static unsigned long gt_rec[GT_SLOTS], gt_frame;
static int gt_cur = -1, gt_inited, gt_off; static double gt_acc_frame, gt_acc_draw; static int gt_acc_n, gt_acc_draws, gt_skipped;
static void gt_mark(int tag) {
    if (gt_off || gt_cur < 0 || gt_n[gt_cur] >= GT_MARKS) return;
    GLCALL(void, "glQueryCounter", unsigned, int)(gt_ids[gt_cur][gt_n[gt_cur]], 0x8E28); gt_tag[gt_cur][gt_n[gt_cur]++] = tag;
}
static void gt_harvest(int s) {
    int n = gt_n[s], avail = 1;
    for (int i = 0; i < n && avail; i++) { int a = 0; GLCALL(void, "glGetQueryObjectiv", unsigned, int, int *)(gt_ids[s][i], 0x8867, &a); avail = a; }
    if (!avail) { gt_skipped++; gt_full[s] = 0; return; }
    unsigned long long t[GT_MARKS]; for (int i = 0; i < n; i++) GLCALL(void, "glGetQueryObjectui64v", unsigned, int, unsigned long long *)(gt_ids[s][i], 0x8866, &t[i]);
    double frame = 0, draw = 0; int draws = 0, t0 = -1, t1 = -1;
    for (int i = 0; i < n; i++) { if (gt_tag[s][i] == 1) t0 = i; if (gt_tag[s][i] == 2) t1 = i; }
    if (t0 >= 0 && t1 > t0) frame = (double)(t[t1] - t[t0]) / 1e6;
    for (int i = 0; i + 1 < n; i++) if (gt_tag[s][i] >= 10 && (gt_tag[s][i] & 1) == 0 && gt_tag[s][i + 1] == gt_tag[s][i] + 1) { draw += (double)(t[i + 1] - t[i]) / 1e6; draws++; }
    gt_acc_frame += frame; gt_acc_draw += draw; gt_acc_draws += draws; gt_acc_n++; gt_full[s] = 0;
    if (gt_acc_n == 1 || gt_acc_n % 4 == 0) { TR("gputime: %d samples: GPU frame timeline %.2f ms, merged chunk draws %.2f ms in %.1f draws per frame (%d samples skipped, results not ready)", gt_acc_n, gt_acc_frame / gt_acc_n, gt_acc_draw / gt_acc_n, (double)gt_acc_draws / gt_acc_n, gt_skipped); }
}
static void gt_frame_begin(void) {   // right after the swap
    if (gt_off) return; gt_frame++;
    if (!gt_inited) {
        gt_inited = 1; void *qc = glr("glQueryCounter"), *gq = glr("glGenQueries"), *go = glr("glGetQueryObjectui64v");
        if (!qc || !gq || !go) { gt_off = 1; TR("gputime: timestamp queries are not available in this SDK"); return; }
        for (int s = 0; s < GT_SLOTS; s++) GLCALL(void, "glGenQueries", int, unsigned *)(GT_MARKS, gt_ids[s]);
        TR("gputime: ready (GPU timestamps sampled every 60th frame)");
    }
    for (int s = 0; s < GT_SLOTS; s++) if (gt_full[s] && gt_rec[s] + 3 <= gt_frame) gt_harvest(s);
    if (gt_frame % 60 == 0) { for (int s = 0; s < GT_SLOTS; s++) if (!gt_full[s]) { gt_cur = s; gt_n[s] = 0; gt_mark(1); break; } }
}
static void gt_frame_end(void) {   // just before the swap
    if (gt_off || gt_cur < 0) return; gt_mark(2); gt_full[gt_cur] = 1; gt_rec[gt_cur] = gt_frame; gt_cur = -1;
}
#endif

// ---------------- keyboard / mouse probe ----------------
// First step towards USB keyboard and mouse: find out what this sandboxed title can reach. sceKeyboard*/sceMouse* live in system modules; the audio probe showed that
// sceKernelDlsym() with a plain name failed for libSceAudioOut (0x80020003) although the module loaded, so this probe also calibrates against libkernel.
// It resolves the functions, opens the devices and, for ~25 s after start, prints the first bytes of every non-empty read so the data layout can be decoded
// (wiggle the mouse / press keys while it runs). Lines are tagged "inprobe:". No behaviour change.
// USB keyboard / mouse imports (used by ps5usbin.inc and by the probe below). In the Vulkan title they are strong references so that the linker pulls the SDK stub libSceKeyboard.so (and libSceMouse.so,
// built by vk_relink.sh) in: the runtime linker then resolves them. Weak ones would be dropped by --as-needed and resolve to 0. (A weak attribute on ANY declaration makes the symbol weak: declare them only here.)
#if defined(PS5_VK) && !defined(PS5_USB_NO_STRONG)
#define USB_IMPORT
#else
#define USB_IMPORT __attribute__((weak))
#endif
extern int sceSysmoduleLoadModule(unsigned short) USB_IMPORT;
extern int sceSysmoduleGetModuleHandleInternal(unsigned, int *) USB_IMPORT;
extern int sceKeyboardInit(void) USB_IMPORT;
extern int sceKeyboardOpen(int, int, int, const void *) USB_IMPORT;
extern int sceKeyboardReadState(int, void *) USB_IMPORT;
extern int sceMouseInit(void) USB_IMPORT;
extern int sceMouseOpen(int, int, int, const void *) USB_IMPORT;
extern int sceMouseRead(int, void *, int) USB_IMPORT;
#define sceKeyboardInit_w sceKeyboardInit
#define sceKeyboardOpen_w sceKeyboardOpen
#define sceKeyboardReadState_w sceKeyboardReadState
extern int sceKernelLoadStartModule(const char *name, size_t argc, const void *argv, unsigned flags, const void *opt, int *res);
extern int sceKernelDlsym(int handle, const char *symbol, void **addrp);
static void in_hex(const char *tag, const unsigned char *p, int n) { char b[200]; int l = snprintf(b, sizeof b, "inprobe: %s", tag); for (int i = 0; i < n && l < 190; i++) l += snprintf(b + l, sizeof b - l, " %02x", p[i]); TR("%s", b); }
static int in_nonzero(const unsigned char *p, int n) { for (int i = 0; i < n; i++) if (p[i]) return 1; return 0; }
// ---- name lookup that works on this console: sceKernelDlsym() wants the export's FULL stored name "<NID>#<lib id>#<module id>" (a plain name gives 0x80020003). NID = base64 (alphabet + -) of the first 8 bytes of
// SHA1(name + salt); the full name is found by scanning the module's own mapped segments for "<NID>#".
static void in_sha1(const unsigned char *d, size_t n, unsigned char out[20]) {   // n <= 200
    unsigned h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u }; unsigned char m[320]; memset(m, 0, sizeof m); memcpy(m, d, n); m[n] = 0x80;
    size_t total = ((n + 8) / 64 + 1) * 64; unsigned long long bits = (unsigned long long)n * 8; for (int i = 0; i < 8; i++) m[total - 1 - i] = (unsigned char)(bits >> (8 * i));
    #define ROL(x, k) (((x) << (k)) | ((x) >> (32 - (k))))
    for (size_t off = 0; off < total; off += 64) {
        const unsigned char *p = m + off; unsigned w[80]; for (int i = 0; i < 16; i++) w[i] = (unsigned)p[4*i] << 24 | (unsigned)p[4*i+1] << 16 | (unsigned)p[4*i+2] << 8 | p[4*i+3];
        for (int i = 16; i < 80; i++) w[i] = ROL(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        unsigned A = h[0], B = h[1], C = h[2], D = h[3], E = h[4];
        for (int i = 0; i < 80; i++) { unsigned f, k; if (i < 20) { f = (B & C) | (~B & D); k = 0x5A827999u; } else if (i < 40) { f = B ^ C ^ D; k = 0x6ED9EBA1u; } else if (i < 60) { f = (B & C) | (B & D) | (C & D); k = 0x8F1BBCDCu; } else { f = B ^ C ^ D; k = 0xCA62C1D6u; }
            unsigned t = ROL(A, 5) + f + E + k + w[i]; E = D; D = C; C = ROL(B, 30); B = A; A = t; }
        h[0] += A; h[1] += B; h[2] += C; h[3] += D; h[4] += E;
    }
    for (int i = 0; i < 5; i++) { out[4*i] = h[i] >> 24; out[4*i+1] = h[i] >> 16; out[4*i+2] = h[i] >> 8; out[4*i+3] = h[i]; }
    #undef ROL
}
static void in_nid(const char *name, char out[16]) {
    static const unsigned char salt[16] = { 0x51,0x8D,0x64,0xA6,0x35,0xDE,0xD8,0xC1,0xE6,0xB0,0x39,0xB1,0xC3,0xE5,0x52,0x30 };   // the 16-byte salt appended to the name (SHA1 of name+salt)
    unsigned char buf[200], h[20]; size_t n = strlen(name); if (n > 150) n = 150; memcpy(buf, name, n); memcpy(buf + n, salt, 16); in_sha1(buf, n + 16, h);
    static const char al[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"; int o = 0; unsigned long long v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | h[i];   /* the first 8 digest bytes as a little-endian u64 (checked against sceKernelLoadStartModule = wzvqT4UqKX8, sceKernelDlsym = LwG8g3niqwA: tests/nid_test.c); the first version read them big-endian and no NID lookup could ever match */
    for (int i = 0; i < 10; i++) out[o++] = al[(v >> (58 - 6 * i)) & 63]; out[o++] = al[(v << 2) & 63]; out[o] = 0;
}
extern int sceKernelGetModuleInfo(int handle, void *info) __attribute__((weak));
extern int sceKernelGetModuleList(int *handles, size_t num, size_t *actual) __attribute__((weak));
typedef struct { size_t size; char name[256]; struct { void *addr; unsigned size; int prot; } seg[4]; unsigned nseg; unsigned char fp[20]; } in_modinfo;
static void *in_sym(int h, const char *name, int quiet) {   // plain name first, then the NID, then the full stored name found in the module's memory
    void *a = 0; if (sceKernelDlsym(h, name, &a) == 0 && a) return a;
    char nid[16]; in_nid(name, nid); a = 0; if (sceKernelDlsym(h, nid, &a) == 0 && a) return a;
    {   // the export's full name is "<NID>#<library id>#<module id>", the ids one base64 digit each for the system modules: try every pair (4096 calls)
        static const char al2[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"; char full[24];
        for (int x = 0; x < 64; x++) for (int y = 0; y < 64; y++) { snprintf(full, sizeof full, "%s#%c#%c", nid, al2[x], al2[y]); a = 0; if (sceKernelDlsym(h, full, &a) == 0 && a) { if (!quiet) TR("inprobe: %s found as %s", name, full); return a; } }
    }
    static in_modinfo mi; memset(&mi, 0, sizeof mi); mi.size = sizeof mi;
    if (!sceKernelGetModuleInfo || sceKernelGetModuleInfo(h, &mi) != 0) { if (!quiet) TR("inprobe: no module info for handle %d", h); return 0; }
    char key[16]; int kl = snprintf(key, sizeof key, "%s#", nid);
    for (unsigned i = 0; i < mi.nseg && i < 4; i++) {
        char *base = (char *)mi.seg[i].addr; size_t sz = mi.seg[i].size; if (!base || sz < (size_t)kl || sz > (256u << 20)) continue;
        for (size_t off = 0; off < sz; off += 4096) {
            size_t pl = sz - off < 4096 + 64 ? sz - off : 4096 + 64; char vec[4];
            if (mincore(base + off, pl > 4096 ? 8192 : pl, vec) != 0) continue;
            for (size_t k = 0; k + kl <= pl; k++) if (!memcmp(base + off + k, key, kl)) {
                char full[64]; size_t n = 0; while (n < 40 && off + k + n < sz && base[off + k + n]) { full[n] = base[off + k + n]; n++; } full[n] = 0;
                a = 0; int r = sceKernelDlsym(h, full, &a); if (!quiet) TR("inprobe: %s -> stored name %s, dlsym 0x%x -> %p", name, full, r, a); return r == 0 ? a : 0;
            }
        }
    }
    if (!quiet) TR("inprobe: %s (NID %s) is not in module %d", name, nid, h);
    return 0;
}
static int in_load(const char *lib) {   // bare name first (works for libSceSystemService), then the system folders
    char p[160]; const char *dirs[3] = { "", "/system/common/lib/", "/system/priv/lib/" }; int res = 0;
    for (int d = 0; d < 3; d++) { snprintf(p, sizeof p, "%s%s", dirs[d], lib); int h = sceKernelLoadStartModule(p, 0, 0, 0, 0, &res); TR("inprobe: LoadStartModule(%s) = %d (res %d)", p, h, res); if (h >= 0) return h; }
    return -1;
}
static void in_dump(const char *tag, int h) {   // calibration: segments of the module and the first stored export names ("<NID>#<a>#<b>") found in them
    static in_modinfo mi; memset(&mi, 0, sizeof mi); mi.size = sizeof mi; if (!sceKernelGetModuleInfo || sceKernelGetModuleInfo(h, &mi) != 0) { TR("inprobe: %s: no module info", tag); return; }
    TR("inprobe: %s: module '%s', %u segments", tag, mi.name, mi.nseg); int shown = 0;
    for (unsigned i = 0; i < mi.nseg && i < 4; i++) {
        char *base = (char *)mi.seg[i].addr; size_t sz = mi.seg[i].size; TR("inprobe: %s: segment %u addr %p size %u prot %d", tag, i, (void *)base, mi.seg[i].size, mi.seg[i].prot);
        if (!base || sz < 16 || sz > (256u << 20)) continue;
        for (size_t off = 0; off < sz && shown < 6; off += 4096) {
            char vec[4]; size_t pl = sz - off < 4096 ? sz - off : 4096; if (mincore(base + off, pl, vec) != 0) continue;
            for (size_t k = 0; k + 15 < pl && shown < 6; k++) { const char *q = base + off + k; if (q[11] == '#' && q[13] == '#') { char nm[40]; memcpy(nm, q, 15); nm[15] = 0; int ok = 1; for (int j = 0; j < 15; j++) if (nm[j] < 33 || nm[j] > 126) ok = 0; if (ok) { TR("inprobe: %s: stored name sample: %s", tag, nm); shown++; k += 15; } } }
        }
    }
    if (!shown) TR("inprobe: %s: no stored names found in the segments", tag);
}
static int res_dummy;
static void *in_probe_thread(void *arg) {
    (void)arg; usleep(3000000);
    // 1) what is loaded already
    if (sceKernelGetModuleList && sceKernelGetModuleInfo) {
        static int hs[256]; size_t n = 0; int r = sceKernelGetModuleList(hs, 256, &n); TR("inprobe: module list rc 0x%x, %d modules", r, (int)n);
        for (size_t i = 0; i < n && i < 256; i++) { static in_modinfo mi; memset(&mi, 0, sizeof mi); mi.size = sizeof mi; if (sceKernelGetModuleInfo(hs[i], &mi) == 0) { TR("inprobe: module %d: %s", hs[i], mi.name); usleep(15000); } }
    }
    // 2) the libraries
    const char *libs[5] = { "libSceKeyboard.sprx", "libSceMouse.sprx", "libSceAudioIn.sprx", "libSceVoice.sprx", "libSceImeDialog.sprx" }; int mh[5];
    for (int i = 0; i < 5; i++) { mh[i] = in_load(libs[i]); usleep(20000); }
    if (mh[2] >= 0) in_dump("AudioIn", mh[2]);
    { int ss = sceKernelLoadStartModule("libSceSystemService.sprx", 0, 0, 0, 0, &res_dummy); TR("inprobe: calibration module libSceSystemService = %d", ss); if (ss >= 0) { in_dump("SystemService", ss); void *g = in_sym(ss, "sceSystemServiceGetAppIdOfRunningBigApp", 0); TR("inprobe: calibration GetAppIdOfRunningBigApp -> %p", g); } }
    void *kb[5] = {0}, *ms[5] = {0}; const char *kbn[5] = { "sceKeyboardInit", "sceKeyboardOpen", "sceKeyboardReadState", "sceKeyboardRead", "sceKeyboardClose" };
    const char *msn[5] = { "sceMouseInit", "sceMouseOpen", "sceMouseRead", "sceMouseClose", "sceMouseConnectPort" };
    for (int i = 0; i < 5; i++) { if (mh[0] >= 0) kb[i] = in_sym(mh[0], kbn[i], 0); if (mh[1] >= 0) ms[i] = in_sym(mh[1], msn[i], 0); usleep(10000); }
    if (mh[2] >= 0) { const char *an[] = { "sceAudioInInit", "sceAudioInOpen", "sceAudioInInput", "sceAudioInClose", "sceAudioInGetSilenceLevel" }; for (int i = 0; i < 5; i++) { in_sym(mh[2], an[i], 0); usleep(10000); } }
    if (mh[3] >= 0) { const char *vn[] = { "sceVoiceInit", "sceVoiceCreatePort", "sceVoiceStart", "sceVoiceReadFromOPort", "sceVoiceConnectIPortToOPort" }; for (int i = 0; i < 5; i++) { in_sym(mh[3], vn[i], 0); usleep(10000); } }
    // linked stubs of the SDK (libSceKeyboard exists there): present when the title links them
    TR("inprobe: linked sceKeyboardInit=%p sceKeyboardOpen=%p sceKeyboardReadState=%p", (void *)sceKeyboardInit_w, (void *)sceKeyboardOpen_w, (void *)sceKeyboardReadState_w);
    if (!kb[0]) kb[0] = (void *)sceKeyboardInit_w; if (!kb[1]) kb[1] = (void *)sceKeyboardOpen_w; if (!kb[2]) kb[2] = (void *)sceKeyboardReadState_w;
    int uid = -1; sceUserServiceGetInitialUser(&uid); int users[3] = { uid, 0xFF, 0 };
    int kh = -1, mhdl = -1;
    if (kb[0] && kb[1]) {
        TR("inprobe: sceKeyboardInit rc=0x%x", ((int (*)(void))kb[0])());
        for (int u = 0; u < 3 && kh < 0; u++) { int r = ((int (*)(int, int, int, void *))kb[1])(users[u], 0, 0, 0); TR("inprobe: sceKeyboardOpen(user=%d) = 0x%x", users[u], r); if (r > 0) kh = r; }
    }
    if (ms[0] && ms[1]) {
        TR("inprobe: sceMouseInit rc=0x%x", ((int (*)(void))ms[0])());
        for (int u = 0; u < 3 && mhdl < 0; u++) { int r = ((int (*)(int, int, int, void *))ms[1])(users[u], 0, 0, 0); TR("inprobe: sceMouseOpen(user=%d) = 0x%x", users[u], r); if (r > 0) mhdl = r; }
    }
    static unsigned char buf[512]; int lines = 0;
    for (int tick = 0; tick < 500 && (kh > 0 || mhdl > 0); tick++) {   // 25 s at 50 ms
        usleep(50000);
        if (mhdl > 0 && ms[2]) { memset(buf, 0, sizeof buf); int n = ((int (*)(int, void *, int))ms[2])(mhdl, buf, 1); if ((n > 0 || in_nonzero(buf, 64)) && lines < 40) { lines++; TR("inprobe: sceMouseRead n=%d", n); in_hex("mouse data[0..47]:", buf, 48); } }
        if (kh > 0 && kb[2]) { memset(buf, 0, sizeof buf); int n = ((int (*)(int, void *))kb[2])(kh, buf); if (n >= 0 && in_nonzero(buf + 8, 56) && lines < 40) { lines++; TR("inprobe: sceKeyboardReadState rc=%d", n); in_hex("kbd data[0..63]:", buf, 64); } }
    }
    TR("inprobe: done (keyboard handle %d, mouse handle %d, %d data lines)", kh, mhdl, lines);
    return 0;
}
static void in_probe_start(void) { static int started; if (started) return; started = 1; pthread_t t; if (pthread_create(&t, 0, in_probe_thread, 0) == 0) pthread_detach(t); }

// Process / libc parameters as the running process sees them (SceProcParam -> SceLibcParam: sceLibcHeapSize, ...ExtendedAlloc, ...InitialSize): the libc's own heap holds only ~12 MB of free 256K pieces and drains during play.
// This prints the real values, so we know whether the limit we patch into the module (make_jvm_title.sh, HEAPLIM) reaches the process at all. Everything is read through mincore-checked pointers.
extern void *sceKernelGetProcParam(void) __attribute__((weak));
static int pp_ok(unsigned long a, unsigned long n) {   // readable? asked of the kernel (write() from the range into a pipe answers EFAULT instead of faulting)
    if (a < 0x10000UL || a > 0x7fffffffffffUL) return 0;
    static int pf[2] = { -1, -1 }; if (pf[0] < 0 && pipe(pf) != 0) { pf[0] = -1; return a >= 0x400000UL && a + n < 0x20000000UL; }   /* no pipe: trust the module image range */
    char drain[256]; unsigned long done = 0;
    while (done < n) { unsigned long c = n - done > 128 ? 128 : n - done; long r = write(pf[1], (const void *)(a + done), c); if (r <= 0) return 0; if (read(pf[0], drain, (size_t)r) < 0) return 0; done += (unsigned long)r; }
    return 1;
}
static void procparam_probe(void) {
    if (!sceKernelGetProcParam) { TR("procparam: sceKernelGetProcParam is not linked"); return; }
    unsigned long pp = (unsigned long)sceKernelGetProcParam();
    TR("procparam: SceProcParam at 0x%lx (readable %d)", pp, pp_ok(pp, 0x80));
    if (!pp_ok(pp, 0x80)) return;
    const unsigned long *w = (const unsigned long *)pp;
    TR("procparam: [0x00] %lx %lx %lx %lx | [0x20] %lx %lx %lx %lx | [0x40] %lx %lx %lx %lx", w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11]);
    for (int i = 3; i < 12; i++) { unsigned long q = w[i]; if (!pp_ok(q, 0x48)) continue; const unsigned long *x = (const unsigned long *)q;
        TR("procparam: [0x%x] -> 0x%lx: %lx %lx %lx %lx %lx %lx %lx %lx", i * 8, q, x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7]);
        for (int k = 1; k < 8; k++) if (x[0] >= 0x20 && x[0] < 0x200 && pp_ok(x[k], 8)) TR("procparam:   [0x%x].[0x%x] -> 0x%lx = %lx (%lu)", i * 8, k * 8, x[k], *(const unsigned long *)x[k], *(const unsigned long *)x[k]); }
}
// ---------------- GLFW subset ----------------
int glfwInit(void) {
    if (g_inited) return 1;
    g_inited = 1; g_t0 = now_s();
    int user = -1; int r1 = -1, r2 = -1, r3 = -1;
    TR("glfw: init begin");
    r1 = sceUserServiceInitialize(0);
    r2 = sceUserServiceGetInitialUser(&user);
    g_pad_user = r2 == 0 ? user : -1;
    if (r2 == 0) { r3 = scePadInit(); if (r3 >= 0) g_pad = scePadOpen(user, 0, 0, 0); }
    TR("glfw: init done user=%d pad=%d (userSvcInit=%x getUser=%x padInit=%x)", user, g_pad, r1, r2, r3);
    { double t0 = now_s(); for (int k = 0; k < 2000; k++) (void)now_s(); double t1 = now_s(); TR("clock: now_s() costs %.2f us per call (a timing wrapper pays two)", 1e6 * (t1 - t0) / 2000); }
    procparam_probe();
#ifdef PS5_INPROBE
    in_probe_start();   // keyboard/mouse/mic probe (log lines "inprobe:"; no behaviour change). Earlier result: LoadStartModule(libSceKeyboard/libSceMouse) = 0x80020002 and sceKernelDlsym() by plain name failing even for libkernel; re-enable when working on USB input
#endif
    return 1;
}
void glfwTerminate(void) {}
void glfwInitHint(int h, int v) { (void)h; (void)v; }
void glfwGetVersion(int *a, int *b, int *c) { if (a) *a = g_ver[0]; if (b) *b = g_ver[1]; if (c) *c = g_ver[2]; }
const char *glfwGetVersionString(void) { return "3.3.8 PS5 (EGL/OpenGL 4.6 Core)"; }
int glfwGetError(const char **d) { if (d) *d = 0; return 0; }
void *glfwSetErrorCallback(void *f) { void *o = cb_err; cb_err = f; return o; }
void glfwWindowHint(int h, int v) { (void)h; (void)v; }
void glfwDefaultWindowHints(void) {}

static void pad_emu_load(void);
#ifdef PS5_VK
// Zink: Mesa's own GL command thread (mesa_glthread): the GL calls are queued on the render thread and Zink's per-draw work runs on another core (the render thread was the bottleneck: GPU backlog
// 0.1-0.7 ms, 12-16 ms of CPU per frame). It is on whenever the launcher's graphics setting glthread is on (PS5_GLTHREAD), with a safety net: a marker file is written before the attempt and
// removed once 300 frames were presented; if the marker is still there at the next start, that start did not get going and glthread stays off until /app0/.zglt_bad is deleted.
static int g_zglt;
static void zk_glthread_setup(void) {
    if (!getenv("PS5_ZGLTHREAD")) { TR("zink: Mesa glthread off (default; launcher.cfg zglthread=on turns it on)"); return; }
    if (access("/app0/.zglt_bad", F_OK) == 0) { TR("zink: Mesa glthread stays off: an earlier start with it never presented 300 frames (delete /app0/.zglt_bad to try again)"); return; }
    if (access("/app0/.zglt_try", F_OK) == 0) { int f = open("/app0/.zglt_bad", O_WRONLY | O_CREAT, 0666); if (f >= 0) close(f); unlink("/app0/.zglt_try"); TR("zink: the last start with Mesa glthread did not present 300 frames: glthread is switched off from now on"); return; }
    { int f = open("/app0/.zglt_try", O_WRONLY | O_CREAT, 0666); if (f >= 0) close(f); }
    setenv("mesa_glthread", "true", 1); setenv("MESA_GLTHREAD", "true", 1); g_zglt = 1;
    TR("zink: Mesa glthread requested (mesa_glthread=true)");
}
static void zk_glthread_ok(void) { if (g_zglt) { unlink("/app0/.zglt_try"); g_zglt = 0; TR("zink: 300 frames presented with Mesa glthread: marker cleared"); } }
static void *ps5vk_create_window(int w, int h);
#endif
#if defined(PS5_VK) && defined(PS5_ZINK)
static volatile int g_vk_defer;   // 1: the game on Vulkan runs while the launcher's loading screen is still on Zink; the display is handed over in glfwCreateWindow (McBoot: Hello.quit("vkdefer"))
void ps5_vk_defer(int v) { g_vk_defer = v; }
int ps5_vk_switch(void);
extern void ps5_vk_defer_done(void) __attribute__((weak));
#endif
static int g_use_gl;   // 1: take the OpenGL path although the shim is built with PS5_VK (AIO title; set by ps5_gl_probe)
void *glfwCreateWindow(int w, int h, const char *title, void *mon, void *share) {
    (void)w; (void)h; (void)title; (void)mon; (void)share;
    if (kit_boot_stop) kit_boot_stop();   // the launcher's loading screen still owns the window and the context: it ends first (it destroys its GL objects and releases the context)
    pad_emu_load();   // the instance (launcher) is chosen before Minecraft creates its window
#if defined(PS5_VK) && defined(PS5_ZINK)
    if (g_zk && g_vk_defer) {   // the loading screen has been up all through the game's start; now the display goes to Vulkan (the game asks for its window with no GL context)
        g_vk_defer = 0; TR("glfw: createWindow with a deferred Vulkan hand-over: Zink gives the display up now");
        if (ps5_vk_switch()) { if (ps5_vk_defer_done) ps5_vk_defer_done(); }
        else { int f = open("/app0/mc/.kit/vk_once", O_WRONLY | O_CREAT, 0666); if (f >= 0) { write(f, "1", 1); close(f); } TR("glfw: the Vulkan hand-over failed: the next start of the app plays on Vulkan"); return 0; }
    }
#endif
    if (g_created) { TR("glfw: createWindow again: the existing window and context are reused (launcher menu -> game)"); return WIN; }   // one fixed window (the SDK's rule): a second request gets the first one
#ifdef PS5_VK
    if (!g_use_gl && !g_zk) return ps5vk_create_window(w, h);   // AIO builds can choose the OpenGL path at run time (g_use_gl)
    if (g_zk) ps5vk_create_window(w, h);   // Zink: the window is the display; its Vulkan surface is made by Mesa through ps5_zink_create_display_surface
#endif
    static const EGLint cfg[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_NONE };
    static const EGLint cfgz[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, 0x3026 /* EGL_STENCIL_SIZE */, 8, EGL_NONE };   // Zink picks the swapchain format from the config: RGBA8888 (the first config is RGB565)
    static const EGLint ctxa[] = { EGL_CONTEXT_MAJOR_VERSION_KHR, 4, EGL_CONTEXT_MINOR_VERSION_KHR, 6, EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_NONE };
    EGLConfig c = 0; EGLint major, minor, n = 0, ew = 0, eh = 0;
    TR("glfw: createWindow begin");
    if (g_zk) zk_glthread_setup();
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    TR("glfw: eglGetDisplay=%p", (void *)g_dpy);
#ifdef PS5_HZ120
    if (!g_zk) {   // (always 120 when the build supports it: the 60 Hz setting is a frame limiter, so it can change at run time) the SDK presents at 60 Hz by default; 120 Hz must be requested while EGL is still terminated (and the title must declare it in param.json: HZ120=1 does that)
        typedef unsigned (*set_refresh_t)(void *, int); set_refresh_t sr = (set_refresh_t)eglGetProcAddress("eglSetDisplayRefreshPS5");
        if (sr && g_dpy != EGL_NO_DISPLAY) TR("glfw: eglSetDisplayRefreshPS5(120) = %u", sr(g_dpy, 120)); else TR("glfw: eglSetDisplayRefreshPS5 is not available in this SDK (no runtime display modes): staying at 60 Hz");
    }
#endif
    if (g_zk) kmem_report();   // memory the GPU driver finds when it picks the device
    if (g_dpy == EGL_NO_DISPLAY || !eglInitialize(g_dpy, &major, &minor)) { TR("glfw: eglInitialize FAILED err=%x", eglGetError()); return 0; }
    TR("glfw: EGL %d.%d", major, minor);
    if (!eglBindAPI(EGL_OPENGL_API) || !eglChooseConfig(g_dpy, g_zk ? cfgz : cfg, &c, 1, &n) || n < 1) { TR("glfw: bindAPI/chooseConfig FAILED err=%x n=%d", eglGetError(), n); return 0; }
#ifdef PS5_ZINK
    if (g_zk) { g_zk_win[0] = g_w; g_zk_win[1] = g_h; }
#endif
    g_surf = eglCreateWindowSurface(g_dpy, c, (EGLNativeWindowType)0, 0);
    TR("glfw: surface=%p", (void *)g_surf);
    g_ctx = eglCreateContext(g_dpy, c, EGL_NO_CONTEXT, ctxa);
    TR("glfw: context=%p err=%x", (void *)g_ctx, eglGetError());
    if (g_surf == EGL_NO_SURFACE || g_ctx == EGL_NO_CONTEXT) return 0;
    if (eglQuerySurface(g_dpy, g_surf, EGL_WIDTH, &ew) && eglQuerySurface(g_dpy, g_surf, EGL_HEIGHT, &eh) && ew > 0 && eh > 0) { g_w = ew; g_h = eh; }
    g_cx = g_w / 2.0; g_cy = g_h / 2.0; g_created = 1;
    TR("glfw: window ready %dx%d, SDK heap live=%zu bytes", g_w, g_h, g_zk ? (size_t)0 : ps5_opengl_heap_live_bytes());
    if (!g_zk) ps5_opengl_heap_stats_print(0);
    return WIN;
}
void glfwDestroyWindow(void *w) { (void)w; }
#ifdef PS5_MDBENCH
// ---------------- startup GL microbenchmark: how cheap is multi-draw on this SDK? (build with MDBENCH=1) ----------------
// Draw count is what limits Minecraft here (about 0.04 ms per glDrawElements after VFIX, linear in the draws per frame). Sodium-style renderers cut that by storing many chunk
// meshes in one buffer and drawing them with ONE multi-draw call whose per-draw chunk offset comes from gl_DrawID. The SDK advertises indirect-count multi-draw with
// gl_DrawID, but whether it is CHEAP is unknown. This runs once, on the fresh context, offscreen, with chunk-like data (500 "chunks" x 500 quads, MC's BLOCK vertex layout
// with the VFIX light attribute, 16-bit sequential quad indices) and logs the cost per frame of: A) 500 x (bind VAO, set offset uniform, glDrawElements) = what MC does now,
// B) one glMultiDrawElementsBaseVertex over one big buffer, C) one glMultiDrawElementsIndirect. Lines are tagged "mdbench:".
static unsigned md_shader(int type, const char *src, const char *what) {
    unsigned sh = GLCALL(unsigned, "glCreateShader", int)(type); const char *p[1] = { src };
    GLCALL(void, "glShaderSource", unsigned, int, const char *const *, const int *)(sh, 1, p, 0);
    GLCALL(void, "glCompileShader", unsigned)(sh);
    int ok = 0; GLCALL(void, "glGetShaderiv", unsigned, int, int *)(sh, 0x8B81, &ok);
    if (!ok) { char log[240] = { 0 }; GLCALL(void, "glGetShaderInfoLog", unsigned, int, int *, char *)(sh, sizeof log - 1, 0, log); TR("mdbench: %s shader FAILED: %.200s", what, log); return 0; }
    return sh;
}
static unsigned md_program(const char *vs, const char *fs, const char *what) {
    unsigned v = md_shader(0x8B31, vs, what), f = md_shader(0x8B30, fs, what); if (!v || !f) return 0;
    unsigned pr = GLCALL(unsigned, "glCreateProgram", void)();
    GLCALL(void, "glAttachShader", unsigned, unsigned)(pr, v); GLCALL(void, "glAttachShader", unsigned, unsigned)(pr, f);
    GLCALL(void, "glLinkProgram", unsigned)(pr);
    int ok = 0; GLCALL(void, "glGetProgramiv", unsigned, int, int *)(pr, 0x8B82, &ok);
    if (!ok) { TR("mdbench: %s program link FAILED", what); return 0; }
    return pr;
}
static void md_attribs(unsigned vbo, long base) {   // MC BLOCK layout: pos f3 @0, colour u8x4 norm @12, uv f2 @16, light as ONE uint @24 (VFIX), normal i8x4 norm @28; stride 32
    GLCALL(void, "glBindBuffer", int, unsigned)(0x8892, vbo);
    for (int a = 0; a < 5; a++) GLCALL(void, "glEnableVertexAttribArray", unsigned)((unsigned)a);
    GLCALL(void, "glVertexAttribPointer", unsigned, int, int, int, int, const void *)(0, 3, 0x1406, 0, 32, (const void *)(base + 0));
    GLCALL(void, "glVertexAttribPointer", unsigned, int, int, int, int, const void *)(1, 4, 0x1401, 1, 32, (const void *)(base + 12));
    GLCALL(void, "glVertexAttribPointer", unsigned, int, int, int, int, const void *)(2, 2, 0x1406, 0, 32, (const void *)(base + 16));
    GLCALL(void, "glVertexAttribIPointer", unsigned, int, int, int, const void *)(3, 1, 0x1405, 32, (const void *)(base + 24));
    GLCALL(void, "glVertexAttribPointer", unsigned, int, int, int, int, const void *)(4, 4, 0x1400, 1, 32, (const void *)(base + 28));
}
static void md_bench(void) {
    enum { N = 500, Q = 500, VPC = Q * 4, IPC = Q * 6, FR = 24 };
    TR("mdbench: start (%d chunks x %d quads = %d indices per draw)", N, Q, IPC);
    static const char *vsA = "#version 330 core\nlayout(location=0) in vec3 Position; layout(location=1) in vec4 Color; layout(location=2) in vec2 UV0; layout(location=3) in uint UV2_raw_; layout(location=4) in vec4 Normal;\n"
        "uniform vec3 ChunkOffset; out vec4 vC; out vec2 vT;\nvoid main() { vec3 p = Position + ChunkOffset; gl_Position = vec4(p.xy, 0.5, 1.0); ivec2 l = ivec2((int(UV2_raw_) << 16) >> 16, int(UV2_raw_) >> 16);\n"
        " vC = Color * (0.5 + float(l.x) / 512.0) * (0.75 + 0.25 * Normal.x); vT = UV0; }\n";
    static const char *vsB = "#version 460 core\nlayout(location=0) in vec3 Position; layout(location=1) in vec4 Color; layout(location=2) in vec2 UV0; layout(location=3) in uint UV2_raw_; layout(location=4) in vec4 Normal;\n"
        "layout(std140, binding = 0) uniform Offs { vec4 offs[512]; }; out vec4 vC; out vec2 vT;\nvoid main() { vec3 p = Position + offs[gl_DrawID].xyz; gl_Position = vec4(p.xy, 0.5, 1.0); ivec2 l = ivec2((int(UV2_raw_) << 16) >> 16, int(UV2_raw_) >> 16);\n"
        " vC = Color * (0.5 + float(l.x) / 512.0) * (0.75 + 0.25 * Normal.x); vT = UV0; }\n";
    static const char *fs = "#version 330 core\nuniform sampler2D Sampler0; in vec4 vC; in vec2 vT; out vec4 o;\nvoid main() { o = texture(Sampler0, vT) * vC; }\n";
    unsigned progA = md_program(vsA, fs, "A"), progB = md_program(vsB, fs, "B");
    // offscreen target + a 16x16 texture
    unsigned fbo = 0, rb = 0, ctex = 0, tex = 0;
    GLCALL(void, "glGenFramebuffers", int, unsigned *)(1, &fbo); GLCALL(void, "glGenRenderbuffers", int, unsigned *)(1, &rb); GLCALL(void, "glGenTextures", int, unsigned *)(2, &ctex);
    tex = ctex + 1; if (tex == ctex) tex = ctex;   // second id from the same call is not guaranteed consecutive; fetch it properly below
    { unsigned two[2]; GLCALL(void, "glGenTextures", int, unsigned *)(2, two); ctex = two[0]; tex = two[1]; }
    GLCALL(void, "glBindTexture", int, unsigned)(0x0DE1, ctex);
    GLCALL(void, "glTexImage2D", int, int, int, int, int, int, int, int, const void *)(0x0DE1, 0, 0x8058, 256, 256, 0, 0x1908, 0x1401, 0);
    GLCALL(void, "glBindRenderbuffer", int, unsigned)(0x8D41, rb); GLCALL(void, "glRenderbufferStorage", int, int, int, int)(0x8D41, 0x81A6, 256, 256);
    GLCALL(void, "glBindFramebuffer", int, unsigned)(0x8D40, fbo);
    GLCALL(void, "glFramebufferTexture2D", int, int, int, unsigned, int)(0x8D40, 0x8CE0, 0x0DE1, ctex, 0);
    GLCALL(void, "glFramebufferRenderbuffer", int, int, int, unsigned)(0x8D40, 0x8D00, 0x8D41, rb);
    GLCALL(void, "glViewport", int, int, int, int)(0, 0, 256, 256);
    static unsigned char tp[16 * 16 * 4]; for (int i = 0; i < 16 * 16 * 4; i++) tp[i] = (unsigned char)(i * 7);
    GLCALL(void, "glBindTexture", int, unsigned)(0x0DE1, tex);
    GLCALL(void, "glTexImage2D", int, int, int, int, int, int, int, int, const void *)(0x0DE1, 0, 0x8058, 16, 16, 0, 0x1908, 0x1401, tp);
    GLCALL(void, "glTexParameteri", int, int, int)(0x0DE1, 0x2801, 0x2600); GLCALL(void, "glTexParameteri", int, int, int)(0x0DE1, 0x2800, 0x2600);
    // geometry: chunk-like vertex data (one block shared by every chunk; the offset differs per draw) and the sequential quad index buffer
    static unsigned char vtx[VPC * 32]; static unsigned short idx[IPC];
    for (int q = 0; q < Q; q++) {
        float x = -0.9f + 1.8f * (float)(q % 25) / 25.0f, y = -0.9f + 1.8f * (float)(q / 25) / 20.0f, d = 0.004f;
        const float cx[4] = { 0, d, d, 0 }, cy[4] = { 0, 0, d, d };
        for (int k = 0; k < 4; k++) {
            unsigned char *v = vtx + (q * 4 + k) * 32; float pos[3] = { x + cx[k], y + cy[k], 0 }, uv[2] = { cx[k] > 0, cy[k] > 0 };
            memcpy(v, pos, 12); v[12] = 200; v[13] = 180; v[14] = 160; v[15] = 255; memcpy(v + 16, uv, 8);
            unsigned lt = (unsigned)(q % 16 * 16) | ((unsigned)(q % 15 * 16) << 16); memcpy(v + 24, &lt, 4); v[28] = 0; v[29] = 127; v[30] = 0; v[31] = 0;
        }
        static const int pat[6] = { 0, 1, 2, 2, 3, 0 }; for (int k = 0; k < 6; k++) idx[q * 6 + k] = (unsigned short)(q * 4 + pat[k]);
    }
    static float offs[512 * 4]; for (int i = 0; i < N; i++) { offs[i * 4 + 0] = 0.0007f * (float)(i % 50) - 0.017f; offs[i * 4 + 1] = 0.0007f * (float)(i / 50) - 0.003f; offs[i * 4 + 2] = 0; offs[i * 4 + 3] = 0; }
    unsigned ebo = 0, vboA[N], vaoA[N], arena = 0, vaoM = 0, ubo = 0, ind = 0;
    GLCALL(void, "glGenBuffers", int, unsigned *)(1, &ebo); GLCALL(void, "glGenBuffers", int, unsigned *)(N, vboA); GLCALL(void, "glGenVertexArrays", int, unsigned *)(N, vaoA);
    GLCALL(void, "glGenBuffers", int, unsigned *)(1, &arena); GLCALL(void, "glGenVertexArrays", int, unsigned *)(1, &vaoM);
    GLCALL(void, "glGenBuffers", int, unsigned *)(1, &ubo); GLCALL(void, "glGenBuffers", int, unsigned *)(1, &ind);
    for (int i = 0; i < N; i++) {   // A: one VBO + VAO per chunk, all sharing the index buffer (what Minecraft does)
        GLCALL(void, "glBindVertexArray", unsigned)(vaoA[i]);
        GLCALL(void, "glBindBuffer", int, unsigned)(0x8892, vboA[i]); GLCALL(void, "glBufferData", int, long, const void *, int)(0x8892, (long)sizeof vtx, vtx, 0x88E4);
        md_attribs(vboA[i], 0);
        GLCALL(void, "glBindBuffer", int, unsigned)(0x8893, ebo); if (i == 0) GLCALL(void, "glBufferData", int, long, const void *, int)(0x8893, (long)sizeof idx, idx, 0x88E4);
    }
    GLCALL(void, "glBindVertexArray", unsigned)(vaoM);   // B/C: ONE arena holding all chunks back to back
    GLCALL(void, "glBindBuffer", int, unsigned)(0x8892, arena); GLCALL(void, "glBufferData", int, long, const void *, int)(0x8892, (long)sizeof vtx * 1500, 0, 0x88E4);
    for (int i = 0; i < 1500; i++) GLCALL(void, "glBufferSubData", int, long, long, const void *)(0x8892, (long)i * (long)sizeof vtx, (long)sizeof vtx, vtx);
    md_attribs(arena, 0); GLCALL(void, "glBindBuffer", int, unsigned)(0x8893, ebo);
    GLCALL(void, "glBindBuffer", int, unsigned)(0x8A11, ubo); GLCALL(void, "glBufferData", int, long, const void *, int)(0x8A11, (long)sizeof offs, offs, 0x88E8);
    int counts[N]; const void *iptr[N]; int bv[N]; struct { unsigned count, inst, first; int base; unsigned binst; } cmd[N];
    for (int i = 0; i < N; i++) { counts[i] = IPC; iptr[i] = 0; bv[i] = i * VPC; cmd[i].count = IPC; cmd[i].inst = 1; cmd[i].first = 0; cmd[i].base = i * VPC; cmd[i].binst = 0; }
    GLCALL(void, "glBindBuffer", int, unsigned)(0x8F3F, ind); GLCALL(void, "glBufferData", int, long, const void *, int)(0x8F3F, (long)sizeof cmd, cmd, 0x88E4);
    unsigned er = GLCALL(unsigned, "glGetError", void)(); TR("mdbench: setup done, glGetError=0x%x", er);
    // Result of the first run: A, B and C all measured 16.5-17.2 ms for 500 draws, i.e. one 60 Hz period -- the glFinish round trip has a ~16.7 ms floor, and calls return
    // in microseconds (draws are queued). So this run varies the draw count (100 / 500 / 1500 per frame) to see what scales, and adds two controls:
    // D = same VAO, no state change between draws (is it the VAO/uniform changes that cost?), E = ONE instanced draw of the same geometry (what the GPU needs for the triangles).
    static const int sizes[3] = { 100, 500, 1500 }; const char *nm[5] = { "A bind VAO+uniform+draw", "D draws only (no state change)", "B multidraw basevertex", "C multidraw indirect", "E 1 instanced draw" };
    for (int si = 0; si < 3; si++) {
        const int S = sizes[si]; double base = 0;
        for (int t = 0; t < 5; t++) {
            unsigned pr = (t == 2 || t == 3) ? progB : progA; if (!pr) { TR("mdbench: S=%d %s skipped (shader failed)", S, nm[t]); continue; }
            GLCALL(void, "glUseProgram", unsigned)(pr); int loc = GLCALL(int, "glGetUniformLocation", unsigned, const char *)(pr, "ChunkOffset");
            int sl = GLCALL(int, "glGetUniformLocation", unsigned, const char *)(pr, "Sampler0"); GLCALL(void, "glUniform1i", int, int)(sl, 0);
            if (t == 2 || t == 3) GLCALL(void, "glBindBufferBase", int, unsigned, unsigned)(0x8A11, 0, ubo);
            GLCALL(void, "glActiveTexture", int)(0x84C0); GLCALL(void, "glBindTexture", int, unsigned)(0x0DE1, tex);
            double cpu = 0, tot = 0; int n = 0;
            for (int f = 0; f < 14; f++) {
                GLCALL(void, "glClear", int)(0x4100);
                double t0 = now_s();
                if (t == 0) { for (int i = 0; i < S; i++) { GLCALL(void, "glBindVertexArray", unsigned)(vaoA[i % N]); GLCALL(void, "glUniform3fv", int, int, const float *)(loc, 1, offs + (i % N) * 4); GLCALL(void, "glDrawElements", int, int, int, const void *)(4, IPC, 0x1403, 0); } }
                else if (t == 1) { GLCALL(void, "glBindVertexArray", unsigned)(vaoA[0]); GLCALL(void, "glUniform3fv", int, int, const float *)(loc, 1, offs); for (int i = 0; i < S; i++) GLCALL(void, "glDrawElements", int, int, int, const void *)(4, IPC, 0x1403, 0); }
                else if (t == 4) { GLCALL(void, "glBindVertexArray", unsigned)(vaoA[0]); GLCALL(void, "glUniform3fv", int, int, const float *)(loc, 1, offs); GLCALL(void, "glDrawElementsInstanced", int, int, int, const void *, int)(4, IPC, 0x1403, 0, S); }
                else { GLCALL(void, "glBindVertexArray", unsigned)(vaoM); GLCALL(void, "glBindBuffer", int, unsigned)(0x8A11, ubo); GLCALL(void, "glBufferSubData", int, long, long, const void *)(0x8A11, 0, (long)sizeof offs, offs);
                    if (t == 3) GLCALL(void, "glBindBuffer", int, unsigned)(0x8F3F, ind);
                    for (int left = S; left > 0; left -= N) { int k = left < N ? left : N;
                        if (t == 2) GLCALL(void, "glMultiDrawElementsBaseVertex", int, const int *, int, const void *const *, int, const int *)(4, counts, 0x1403, iptr, k, bv);
                        else GLCALL(void, "glMultiDrawElementsIndirect", int, int, const void *, int, int)(4, 0x1403, (const void *)0, k, 0); } }
                double t1 = now_s(); GLCALL(void, "glFinish", void)(); double t2 = now_s();
                if (f >= 4) { cpu += t1 - t0; tot += t2 - t0; n++; }
            }
            double c = 1e3 * cpu / n, tt = 1e3 * tot / n; unsigned e2 = GLCALL(unsigned, "glGetError", void)(); if (t == 0) base = tt;
            TR("mdbench: S=%4d %-30s CPU %7.2f ms  total %7.2f ms  = %6.1f us/draw  (%.2fx of A) err=0x%x", S, nm[t], c, tt, 1e3 * tt / S, base > 0 ? base / tt : 0.0, e2);
        }
    }
    // Results of the second run (500 draws of 3000 16-bit indices): A 34 us/draw, D (no state change) 29.5, B/C multi-draw 33, but E (one instanced draw, same triangles) 0.4 us per draw-equivalent.
    // => the cost is per draw COMMAND (multi-draw is lowered to individual draws), not state changes and not the GPU. Extras: does the cost depend on the index count or type (the SDK scans
    // 16-bit indices with a scalar loop and 32-bit ones with SSE4.1), and what does ONE draw over many chunks (the "mega-draw" a batching layer could issue) cost?
    {
        unsigned *i32 = (unsigned *)malloc(sizeof(unsigned) * 1500 * IPC); unsigned ebo32 = 0, vaoI = 0, vaoG = 0;
        if (i32) {
            for (int q = 0; q < 1500 * Q; q++) { static const int pat[6] = { 0, 1, 2, 2, 3, 0 }; for (int k = 0; k < 6; k++) i32[q * 6 + k] = (unsigned)(q * 4 + pat[k]); }
            GLCALL(void, "glGenBuffers", int, unsigned *)(1, &ebo32); GLCALL(void, "glGenVertexArrays", int, unsigned *)(1, &vaoI); GLCALL(void, "glGenVertexArrays", int, unsigned *)(1, &vaoG);
            GLCALL(void, "glBindVertexArray", unsigned)(vaoI); GLCALL(void, "glBindBuffer", int, unsigned)(0x8892, vboA[0]); md_attribs(vboA[0], 0);
            GLCALL(void, "glBindBuffer", int, unsigned)(0x8893, ebo32); GLCALL(void, "glBufferData", int, long, const void *, int)(0x8893, (long)(sizeof(unsigned) * 1500 * IPC), i32, 0x88E4);
            GLCALL(void, "glBindVertexArray", unsigned)(vaoG); md_attribs(arena, 0); GLCALL(void, "glBindBuffer", int, unsigned)(0x8893, ebo32);
            free(i32);
            GLCALL(void, "glUseProgram", unsigned)(progA); int loc = GLCALL(int, "glGetUniformLocation", unsigned, const char *)(progA, "ChunkOffset");
            int sl = GLCALL(int, "glGetUniformLocation", unsigned, const char *)(progA, "Sampler0"); GLCALL(void, "glUniform1i", int, int)(sl, 0);
            GLCALL(void, "glActiveTexture", int)(0x84C0); GLCALL(void, "glBindTexture", int, unsigned)(0x0DE1, tex);
            struct { const char *name; int mode; int count; int draws; unsigned type; } X[6] = {
                { "500 draws x 3000 idx, 16-bit", 0, IPC, 500, 0x1403 }, { "500 draws x  300 idx, 16-bit", 0, 300, 500, 0x1403 }, { "500 draws x   60 idx, 16-bit", 0, 60, 500, 0x1403 },
                { "500 draws x 3000 idx, 32-bit", 1, IPC, 500, 0x1405 }, { "ONE draw, 500 chunks (1.5M idx, 32-bit)", 2, 500 * IPC, 1, 0x1405 }, { "ONE draw, 1500 chunks (4.5M idx, 32-bit)", 2, 1500 * IPC, 1, 0x1405 } };
            for (int x = 0; x < 6; x++) {
                double tot = 0, cpu = 0; int n = 0;
                for (int f = 0; f < 14; f++) {
                    GLCALL(void, "glClear", int)(0x4100); double t0 = now_s();
                    GLCALL(void, "glBindVertexArray", unsigned)(X[x].mode == 0 ? vaoA[0] : X[x].mode == 1 ? vaoI : vaoG); GLCALL(void, "glUniform3fv", int, int, const float *)(loc, 1, offs);
                    for (int i = 0; i < X[x].draws; i++) GLCALL(void, "glDrawElements", int, int, int, const void *)(4, X[x].count, (int)X[x].type, 0);
                    double t1 = now_s(); GLCALL(void, "glFinish", void)(); double t2 = now_s(); if (f >= 4) { cpu += t1 - t0; tot += t2 - t0; n++; }
                }
                unsigned e2 = GLCALL(unsigned, "glGetError", void)();
                TR("mdbench: extra %-42s CPU %7.2f ms  total %7.2f ms  = %6.1f us per draw  err=0x%x", X[x].name, 1e3 * cpu / n, 1e3 * tot / n, 1e3 * tot / n / X[x].draws, e2);
            }
            GLCALL(void, "glBindVertexArray", unsigned)(0); GLCALL(void, "glDeleteVertexArrays", int, const unsigned *)(1, &vaoI); GLCALL(void, "glDeleteVertexArrays", int, const unsigned *)(1, &vaoG); GLCALL(void, "glDeleteBuffers", int, const unsigned *)(1, &ebo32);
        }
    }
    // Results of the third run: draw cost is a fixed ~30 us per draw (60 vs 3000 indices: 14.6 vs 16.0 ms per 500 draws; 16-bit vs 32-bit makes no difference), while ONE draw over
    // 500 / 1500 chunks costs 1.5 / 4.1 ms. A merging layer must generate the index list of the visible chunks every frame (Minecraft decides visibility) and upload it, plus a page table
    // that gives each 128-vertex page of the shared vertex buffer its chunk's camera-relative offset (read with texelFetch from gl_VertexID, instead of the ChunkOffset uniform).
    // This measures that whole per-frame pipeline: generate indices, upload them, upload the page table, ONE draw.
    {
        static const char *vsP = "#version 330 core\nlayout(location=0) in vec3 Position; layout(location=1) in vec4 Color; layout(location=2) in vec2 UV0; layout(location=3) in uint UV2_raw_; layout(location=4) in vec4 Normal;\n"
            "uniform sampler2D Pages; out vec4 vC; out vec2 vT;\nvoid main() { vec3 off = texelFetch(Pages, ivec2((gl_VertexID >> 7) & 255, gl_VertexID >> 15), 0).xyz; vec3 p = Position + off; gl_Position = vec4(p.xy, 0.5, 1.0);\n"
            " ivec2 l = ivec2((int(UV2_raw_) << 16) >> 16, int(UV2_raw_) >> 16); vC = Color * (0.5 + float(l.x) / 512.0) * (0.75 + 0.25 * Normal.x); vT = UV0; }\n";
        unsigned progP = md_program(vsP, fs, "P");
        unsigned *idx32 = (unsigned *)malloc(sizeof(unsigned) * 1500 * IPC); static float pages[256 * 128 * 4]; unsigned ebod = 0, vaoP = 0, ptex = 0;
        if (progP && idx32) {
            GLCALL(void, "glGenBuffers", int, unsigned *)(1, &ebod); GLCALL(void, "glGenVertexArrays", int, unsigned *)(1, &vaoP); GLCALL(void, "glGenTextures", int, unsigned *)(1, &ptex);
            GLCALL(void, "glBindVertexArray", unsigned)(vaoP); md_attribs(arena, 0);
            GLCALL(void, "glBindBuffer", int, unsigned)(0x8893, ebod); GLCALL(void, "glBufferData", int, long, const void *, int)(0x8893, (long)(sizeof(unsigned) * 1500 * IPC), 0, 0x88E0);
            GLCALL(void, "glActiveTexture", int)(0x84C7); GLCALL(void, "glBindTexture", int, unsigned)(0x0DE1, ptex);
            GLCALL(void, "glTexImage2D", int, int, int, int, int, int, int, int, const void *)(0x0DE1, 0, 0x8814, 256, 128, 0, 0x1908, 0x1406, 0);
            GLCALL(void, "glTexParameteri", int, int, int)(0x0DE1, 0x2801, 0x2600); GLCALL(void, "glTexParameteri", int, int, int)(0x0DE1, 0x2800, 0x2600);
            GLCALL(void, "glActiveTexture", int)(0x84C0); GLCALL(void, "glBindTexture", int, unsigned)(0x0DE1, tex);
            GLCALL(void, "glUseProgram", unsigned)(progP);
            GLCALL(void, "glUniform1i", int, int)(GLCALL(int, "glGetUniformLocation", unsigned, const char *)(progP, "Sampler0"), 0);
            GLCALL(void, "glUniform1i", int, int)(GLCALL(int, "glGetUniformLocation", unsigned, const char *)(progP, "Pages"), 7);
            for (int S = 500; S <= 1500; S += 1000) {
                double gen = 0, upl = 0, pg = 0, cpu = 0, tot = 0; int n = 0;
                for (int f = 0; f < 14; f++) {
                    GLCALL(void, "glClear", int)(0x4100); double t0 = now_s();
                    for (int c = 0; c < S; c++) {   // 1. index list of the visible chunks (here: all, in order): 500 quads each, vertices of chunk c start at c * 2000
                        unsigned *o = idx32 + (size_t)c * IPC; unsigned b = (unsigned)c * VPC;
                        for (int q = 0; q < Q; q++) { unsigned v = b + (unsigned)q * 4; o[0] = v; o[1] = v + 1; o[2] = v + 2; o[3] = v + 2; o[4] = v + 3; o[5] = v; o += 6; }
                    }
                    for (int c = 0; c < S; c++) for (int p = 0; p < 16; p++) { float *w = pages + (((c * VPC) >> 7) + p) * 4; w[0] = offs[(c % N) * 4]; w[1] = offs[(c % N) * 4 + 1]; w[2] = 0; w[3] = 0; }   // page table (CPU side)
                    double t1 = now_s();
                    GLCALL(void, "glBindVertexArray", unsigned)(vaoP); GLCALL(void, "glBindBuffer", int, unsigned)(0x8893, ebod);
                    GLCALL(void, "glBufferSubData", int, long, long, const void *)(0x8893, 0, (long)sizeof(unsigned) * S * IPC, idx32);   // 2. upload the index list
                    GLCALL(void, "glActiveTexture", int)(0x84C7); GLCALL(void, "glTexSubImage2D", int, int, int, int, int, int, int, int, const void *)(0x0DE1, 0, 0, 0, 256, 128, 0x1908, 0x1406, pages);   // 3. page table
                    GLCALL(void, "glActiveTexture", int)(0x84C0);
                    double t2 = now_s();
                    GLCALL(void, "glDrawElements", int, int, int, const void *)(4, S * IPC, 0x1405, 0);   // 4. ONE draw
                    double t3 = now_s(); GLCALL(void, "glFinish", void)(); double t4 = now_s();
                    if (f >= 4) { gen += t1 - t0; upl += t2 - t1; pg += t3 - t2; cpu += t3 - t0; tot += t4 - t0; n++; }
                }
                unsigned e2 = GLCALL(unsigned, "glGetError", void)();
                TR("mdbench: PIPELINE %4d chunks (%.1fM indices): generate %.2f ms + upload(index+page table) %.2f ms + draw call %.2f ms; CPU %.2f ms, total with GPU %.2f ms  [vs %.1f ms as separate draws] err=0x%x",
                   S, S * IPC / 1e6, 1e3 * gen / n, 1e3 * upl / n, 1e3 * pg / n, 1e3 * cpu / n, 1e3 * tot / n, S * 0.031, e2);
            }
            GLCALL(void, "glBindVertexArray", unsigned)(0); GLCALL(void, "glDeleteVertexArrays", int, const unsigned *)(1, &vaoP); GLCALL(void, "glDeleteBuffers", int, const unsigned *)(1, &ebod); GLCALL(void, "glDeleteTextures", int, const unsigned *)(1, &ptex);
        } else TR("mdbench: PIPELINE skipped (shader failed or out of memory)");
        free(idx32);
    }
    // clean up: back to the state of a fresh context
    GLCALL(void, "glBindVertexArray", unsigned)(0); GLCALL(void, "glBindBuffer", int, unsigned)(0x8892, 0); GLCALL(void, "glBindBuffer", int, unsigned)(0x8893, 0);
    GLCALL(void, "glBindBuffer", int, unsigned)(0x8F3F, 0); GLCALL(void, "glBindBuffer", int, unsigned)(0x8A11, 0); GLCALL(void, "glUseProgram", unsigned)(0);
    GLCALL(void, "glBindFramebuffer", int, unsigned)(0x8D40, 0); GLCALL(void, "glBindTexture", int, unsigned)(0x0DE1, 0);
    GLCALL(void, "glDeleteVertexArrays", int, const unsigned *)(N, vaoA); GLCALL(void, "glDeleteBuffers", int, const unsigned *)(N, vboA);
    GLCALL(void, "glDeleteVertexArrays", int, const unsigned *)(1, &vaoM); GLCALL(void, "glDeleteBuffers", int, const unsigned *)(1, &arena); GLCALL(void, "glDeleteBuffers", int, const unsigned *)(1, &ebo);
    GLCALL(void, "glDeleteBuffers", int, const unsigned *)(1, &ubo); GLCALL(void, "glDeleteBuffers", int, const unsigned *)(1, &ind);
    GLCALL(void, "glDeleteFramebuffers", int, const unsigned *)(1, &fbo); GLCALL(void, "glDeleteRenderbuffers", int, const unsigned *)(1, &rb);
    { unsigned two[2] = { ctex, tex }; GLCALL(void, "glDeleteTextures", int, const unsigned *)(2, two); }
    GLCALL(unsigned, "glGetError", void)();
    TR("mdbench: done");
}
#endif
// Pixel-transfer state is part of the GL context, and the one context is handed from the launcher to a game and back. A game leaves UNPACK_ROW_LENGTH / SKIP_* / ALIGNMENT / a bound pixel buffer behind; the next owner
// (the launcher's R8 font atlas, a second game's glyph atlas) uploads its textures with that state and gets sheared, striped text. Both ways are put back to the GL defaults, and the values found are logged.
static void gl_reset_transfer_state(const char *when) {
    if (!g_dpy || eglGetCurrentContext() == EGL_NO_CONTEXT) return;
    void (*gi)(unsigned, int *) = GLCALL(void, "glGetIntegerv", unsigned, int *);
    void (*ps)(unsigned, int) = GLCALL(void, "glPixelStorei", unsigned, int);
    void (*bb)(unsigned, unsigned) = GLCALL(void, "glBindBuffer", unsigned, unsigned);
    void (*at)(unsigned) = GLCALL(void, "glActiveTexture", unsigned);
    if (!gi || !ps || !bb || !at) return;
    static const unsigned q[] = { 0x0CF2, 0x0CF3, 0x0CF4, 0x0CF5, 0x806E, 0x806D, 0x0CF0, 0x0CF1, 0x0D02, 0x0D03, 0x0D04, 0x0D05, 0x88EF, 0x88ED, 0x84E0, 0x8CA6, 0x8069, 0x8B8D, 0x85B5 };
    int v[sizeof q / sizeof q[0]]; for (unsigned i = 0; i < sizeof q / sizeof q[0]; i++) { v[i] = -1; gi(q[i], &v[i]); }
    static int n; if (n++ < 12) TR("glstate: %s: unpack row=%d skipRows=%d skipPix=%d align=%d imgH=%d skipImg=%d swap=%d lsb=%d | pack row=%d skipRows=%d skipPix=%d align=%d | unpackPBO=%d packPBO=%d activeTex=0x%x fbo=%d tex2D=%d prog=%d vao=%d",
        when, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15], v[16], v[17], v[18]);
    // Fixed-function / core state a game assumes to be at the GL defaults: a leftover scissor box or viewport clips or shrinks the game's first frames (1.12.2: the Mojang splash drew in a small window with the
    // launcher still visible around it). Probe first, then restore.
    { unsigned char (*ie)(unsigned) = GLCALL(unsigned char, "glIsEnabled", unsigned);
      void (*gb)(unsigned, unsigned char *) = GLCALL(void, "glGetBooleanv", unsigned, unsigned char *);
      void (*sc)(int, int, int, int) = GLCALL(void, "glScissor", int, int, int, int);
      void (*vp)(int, int, int, int) = GLCALL(void, "glViewport", int, int, int, int);
      void (*dis)(unsigned) = GLCALL(void, "glDisable", unsigned);
      void (*cm)(unsigned char, unsigned char, unsigned char, unsigned char) = GLCALL(void, "glColorMask", unsigned char, unsigned char, unsigned char, unsigned char);
      void (*dm)(unsigned char) = GLCALL(void, "glDepthMask", unsigned char);
      if (ie && gb && sc && vp && dis && cm && dm) {
          int sbox[4] = { -1, -1, -1, -1 }, vbox[4] = { -1, -1, -1, -1 }; unsigned char cmask[4] = { 9, 9, 9, 9 }, dmask = 9;
          gi(0x0C10, sbox); gi(0x0BA2, vbox); gb(0x0C23, cmask); gb(0x0B72, &dmask);
          static int n2; if (n2++ < 12) TR("glstate: %s: scissor %s box %d,%d %dx%d | viewport %d,%d %dx%d | depth test %d blend %d cull %d stencil %d | colormask %d%d%d%d depthmask %d", when, ie(0x0C11) ? "ON" : "off", sbox[0], sbox[1], sbox[2], sbox[3],
              vbox[0], vbox[1], vbox[2], vbox[3], ie(0x0B71), ie(0x0BE2), ie(0x0B44), ie(0x0B90), cmask[0], cmask[1], cmask[2], cmask[3], dmask);
          dis(0x0C11); sc(0, 0, g_w, g_h); vp(0, 0, g_w, g_h); cm(1, 1, 1, 1); dm(1); } }
    // The shadow copy of program / VAO / buffer / FBO bindings (g_sh, kept by the wrappers of the lookup table) and the present-blit's FBO/texture cache describe the PREVIOUS owner of the context: after a 26.x game they
    // name deleted objects, the cursor overlay of the next (fixed-function) game "restored" them -> glUseProgram INVALID_VALUE / glBindVertexArray "non-gen name", the overlay's own program stayed bound while gl4es
    // believed its program was, and every glUniformMatrix hit "non-matrix uniform" (1.8.9 after 26.3: GL error flood). Forget them: the next user re-reads the driver.
    g_sh.seeded = 0; g_pr_valid = 0;
    ps(0x0CF2, 0); ps(0x0CF3, 0); ps(0x0CF4, 0); ps(0x0CF5, 4); ps(0x806E, 0); ps(0x806D, 0); ps(0x0CF0, 0); ps(0x0CF1, 0);
    ps(0x0D02, 0); ps(0x0D03, 0); ps(0x0D04, 0); ps(0x0D05, 4); ps(0x0D00, 0); ps(0x0D01, 0);
    bb(0x88EC, 0); bb(0x88EB, 0); at(0x84C0);
}
void glfwMakeContextCurrent(void *w) { if (w && g_created) { int was = eglGetCurrentContext() != EGL_NO_CONTEXT; int ok = eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx); unsigned er = eglGetError();
        for (int t = 0; !ok && t < 40; t++) { usleep(50000); ok = eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx); er = eglGetError(); }   // the previous owner thread may still be letting go
        TR("glfw: makeCurrent ok=%d err=%x thread=%p current=%p", ok, er, (void *)pthread_self(), (void *)eglGetCurrentContext());
        if (ok && !was) gl_reset_transfer_state("context made current");   // only when this thread did not own it yet (a game that calls makeCurrent again mid-game keeps its state)
        if (ok && ps5_glcompat_level && ps5g4_configure && ps5g4_lookup && !(ps5g4_ready && ps5g4_ready())) { ps5g4_configure(g4_core, ps5_glcompat_level); (void)ps5g4_lookup("glGetString"); }   // fixed-function mode: gl4es is set up here, while the context is certainly current
        else if (ok && ps5_glcompat_level && ps5g4_resync && ps5g4_ready && ps5g4_ready()) ps5g4_resync();   // a second game after the launcher used this context: gl4es' cached bindings are stale
#ifdef PS5_MDBENCH
        { static int done; if (ok && !done) { done = 1; md_bench(); } }
#endif
    } else if (g_dpy) eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); }
void *glfwGetCurrentContext(void) { return g_created && eglGetCurrentContext() != EGL_NO_CONTEXT ? WIN : 0; }
#if defined(PS5_NPROF) || defined(PS5_FINISH)
static double g_fin_sum, g_fin_max; static int g_fin_n;   // glFinish() before the swap: how long the GPU is still busy when the CPU has submitted the whole frame
#endif
#ifdef PS5_NPROF
// ---------------- native sampling profiler + pre-swap glFinish probe (build with NPROF=1) ----------------
// Question: why does the frame rate fall off with the draw count (about 0.09 ms of CPU per glDrawElements, 47 us even for 6 indices), and what is waiting for 23 ms inside
// eglSwapBuffers? (1) A sampler thread sends SIGPROF to the render thread every ~1 ms; the handler stores the interrupted RIP. Every perf window the samples are bucketed
// by 16 bytes and the top buckets are logged ("nprof ..."), to be resolved offline with resolve_prof.py against the unstripped ELF (code is execute-only, so no runtime
// symbolization). Run this build WITHOUT GLTHREAD so the draw work stays on the sampled thread. (2) glFinish() is timed right before eglSwapBuffers: it returns when the
// command stream AND the GPU are done, so "finish" is real work, while "swap" afterwards is only the flip/vblank wait.
#include <signal.h>
extern int main(void);
#ifndef NP_TEXT_END
#define NP_TEXT_END 0x2600000ULL   // end of .text (link address); make_jvm_title.sh passes the real value read from the previous link output
#endif
#define NP_MAX 4096
static unsigned long long g_np_rip[NP_MAX], g_np_ret[NP_MAX], g_np_lo, g_np_hi; static volatile unsigned g_np_n; static pthread_t g_np_target;
static void np_handler(int sig, siginfo_t *si, void *ucv) {
    (void)sig; (void)si; unsigned i = g_np_n;
    if (i < NP_MAX) {
        unsigned long long *w = (unsigned long long *)ucv, rip = w[28], rsp = w[31], ret = 0;   // rip = word 28, rsp = word 31 of the console's saved context (calibrated in jvm_host.cpp)
        if ((rip < g_np_lo || rip >= g_np_hi) && rsp > 0x10000 && !(rsp & 7)) {          // blocked in a system stub / libc / JIT: find the innermost caller inside the executable
            const unsigned long long *st = (const unsigned long long *)rsp;
            for (int j = 0; j < 24; j++) if (st[j] >= g_np_lo && st[j] < g_np_hi) { ret = st[j]; break; }
        }
        g_np_rip[i] = rip; g_np_ret[i] = ret; g_np_n = i + 1;
    }
}
static void *np_thread(void *arg) { (void)arg; for (;;) { usleep(1000); if (g_np_n < NP_MAX) pthread_kill(g_np_target, SIGPROF); } return 0; }
static void np_start(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = np_handler; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigemptyset(&sa.sa_mask);
    g_np_lo = ((unsigned long long)(uintptr_t)&main) & ~0xFFFFFULL; g_np_hi = g_np_lo + NP_TEXT_END;   // the title image starts at its load base (main sits in the first MB); code only, so data/bss pointers on the stack are not taken for return addresses
    int rc = sigaction(SIGPROF, &sa, 0); g_np_target = pthread_self();
    pthread_t t; int rc2 = pthread_create(&t, 0, np_thread, 0); if (rc2 == 0) pthread_detach(t);
    TR("nprof: started sigaction=%d thread=%d main=%p range=%llx-%llx", rc, rc2, (void *)&main, g_np_lo, g_np_hi);
}
static int np_cmp(const void *a, const void *b) { unsigned long long x = *(const unsigned long long *)a, y = *(const unsigned long long *)b; return x < y ? -1 : x > y; }
static void np_report(void) {
    unsigned n = g_np_n; g_np_n = 0; if (n > NP_MAX) n = NP_MAX; if (n == 0) return;
    static unsigned long long k[NP_MAX]; static unsigned long long ba[NP_MAX]; static unsigned bc[NP_MAX]; int nb = 0;
    for (unsigned i = 0; i < n; i++) k[i] = g_np_rip[i] & ~15ULL;
    qsort(k, n, sizeof k[0], np_cmp);
    for (unsigned i = 0; i < n;) { unsigned j = i; while (j < n && k[j] == k[i]) j++; ba[nb] = k[i]; bc[nb] = j - i; nb++; i = j; }
    char line[420]; int len = 0, shown = 0;
    for (int pick = 0; pick < 36 && pick < nb; pick++) {   // top buckets by count (selection, nb is small)
        int best = -1; for (int i = 0; i < nb; i++) if (bc[i] && (best < 0 || bc[i] > bc[best])) best = i;
        if (best < 0) break;
        if (len == 0) len = snprintf(line, sizeof line, "nprof n=%u top:", n);
        len += snprintf(line + len, sizeof line - len, " %llx:%u", ba[best], bc[best]); bc[best] = 0; shown++;
        if (shown % 9 == 0 || pick == 35) { TR("%s", line); len = 0; }
    }
    if (len) TR("%s", line);
    // callers (inside the executable) of the samples that were NOT in the executable: who is blocked or waiting, and where
    nb = 0; static unsigned long long kc[NP_MAX]; unsigned m = 0;
    for (unsigned i = 0; i < n; i++) if (g_np_ret[i]) kc[m++] = g_np_ret[i] & ~15ULL;
    if (m) {
        qsort(kc, m, sizeof kc[0], np_cmp);
        for (unsigned i = 0; i < m;) { unsigned j = i; while (j < m && kc[j] == kc[i]) j++; ba[nb] = kc[i]; bc[nb] = j - i; nb++; i = j; }
        len = 0; shown = 0;
        for (int pick = 0; pick < 27 && pick < nb; pick++) {
            int best = -1; for (int i = 0; i < nb; i++) if (bc[i] && (best < 0 || bc[i] > bc[best])) best = i;
            if (best < 0) break;
            if (len == 0) len = snprintf(line, sizeof line, "nprof-callers n=%u top:", m);
            len += snprintf(line + len, sizeof line - len, " %llx:%u", ba[best], bc[best]); bc[best] = 0; shown++;
            if (shown % 9 == 0) { TR("%s", line); len = 0; }
        }
        if (len) TR("%s", line);
    }
}
#endif
static double g_fl_req, g_fl_act; static int g_fl_n, g_fl_log;
volatile int g_gl_down;
static volatile unsigned long g_swap_cnt; static pthread_t g_render_th; static volatile int g_render_th_set;   // stall detector (pad_sampler)
void glfwSwapBuffers(void *w) {
    g_swap_cnt++; if (!g_render_th_set) { g_render_th = pthread_self(); g_render_th_set = 1; }
    (void)w; static int n; static double t_win, t_last, in_swap, in_frame; static int frames;
    if (!g_created) return;
    if (g_gl_down) { usleep(4000); return; }
#ifdef PS5_MEGA
    mg_sync();
#endif
#ifdef PS5_GPUTIME
    gt_frame_end();
#endif
#ifndef NO_PRESENT_DRAW
    present_draw();   // before the overlay: the cursor must end up on top of the presented frame
#endif
    g_ov_menu = g_polled && g_cursor_mode != GLFW_CURSOR_DISABLED;
    if (g_hud_on || g_ov_menu) cursor_draw();
#ifdef PS5_NPROF
    { static int np_init; if (!np_init) { np_init = 1; np_start(); } }
#endif
#if defined(PS5_NPROF) || defined(PS5_FINISH)
    { double tf0 = now_s(); GLCALL(void, "glFinish", void)(); double tf = now_s() - tf0; g_fin_sum += tf; g_fin_n++; if (tf > g_fin_max) g_fin_max = tf; }
#endif
    if (ps5_glcompat_level && ps5g4_pre_swap) ps5g4_pre_swap();   // gl4es: flush pending immediate-mode / display-list work
    double t0 = now_s();
    int ok = eglSwapBuffers(g_dpy, g_surf);
#ifdef PS5_GPUTIME
    gt_frame_begin();
#endif
    double t1 = now_s();
#ifdef PS5_ZINK
    {   // Zink: eglSwapBuffers returns when the frame is queued, not when it is drawn. Every 30th frame the GPU backlog is measured with a glFinish (this one frame is a little slower; the others are untouched)
        static double fin_sum, fin_max; static int fin_n;
        if (g_zk && n % 30 == 29) { double f0 = now_s(); GLCALL(void, "glFinish", void)(); double fd = now_s() - f0; fin_sum += fd; fin_n++; if (fd > fin_max) fin_max = fd; }
        if (g_zk && g_zk_report && fin_n) { TR("zink: GPU backlog at swap (glFinish every 30th frame): avg %.1f ms max %.1f ms over %d samples", 1e3 * fin_sum / fin_n, 1e3 * fin_max, fin_n); fin_sum = fin_max = 0; fin_n = 0; g_zk_report = 0; }
    }
#endif
    if (n < 3 || ok == 0) { TR("glfw: swap #%d ok=%d eglError=%x heapLive=%zu", n, ok, ok ? 0 : eglGetError(), g_zk ? (size_t)0 : ps5_opengl_heap_live_bytes()); }
#ifdef PS5_HZ120
    if (n == 30 && !g_zk) { typedef unsigned (*get_mode_t)(void *, int *, int *, int *); get_mode_t gm = (get_mode_t)eglGetProcAddress("eglGetDisplayModePS5"); int w = 0, h = 0, hz = 0;
        if (gm) { unsigned r = gm(g_dpy, &w, &h, &hz); if (hz > 0) g_hz_real = hz; TR("glfw: display mode after the first frames: %dx%d @ %d Hz (call returned %u; 60 means the display or title metadata did not allow 120)", w, h, hz, r); } }
#endif
    n++;
    if (n == 300 && g_zk) zk_glthread_ok();
#ifndef NO_FENCEFAKE
    g_swap_n = (unsigned long)n;
    if (n % 120 == 0 && g_fs_made) TR("fence: %lu made, %lu forced real glFinish waits (%.1f ms total), %lu young-fence polls answered 'not yet' (totals)", g_fs_made, g_fs_forced, g_fs_forced_t * 1e3, g_fs_polled_young);
#endif
    { static double t_cap; static int cap_frame; static double t_capstart;
      if (cap_frame) { TR("cap frame end: %d draws in the captured frame, %.1f ms", g_frame_draws, (t1 - t_capstart) * 1e3); cap_frame = 0; g_cap = 0; }
      g_frame_draws = 0;
#ifdef PS5_CAP   // periodic draw capture is off by default now (panorama solved; the probes use glReadPixels/glGet*, which would stall a glthread build)
      if (t1 - t_cap > 8.0) { t_cap = t1; t_capstart = t1; g_cap = 50; cap_frame = 1; TR("cap frame start (every ~8 s, first 50 draws listed in order)"); }
#endif
      }
    // perf line every ~2 s: fps, time inside eglSwapBuffers (GPU/vblank wait) vs the rest of the frame (Java + GL command building)
    if (t_last > 0) in_frame += t0 - t_last;
    in_swap += t1 - t0; frames++; t_last = t1;
    if (t_win == 0) t_win = t1;
    if (t1 - t_win >= 2.0 && frames > 0) {
        TR("perf: fps=%.1f swap=%.1fms other=%.1fms frame=%.1fms (%d frames)", frames / (t1 - t_win), 1e3 * in_swap / frames, 1e3 * in_frame / frames, 1e3 * (t1 - t_win) / frames, frames);
        TR("clock: fast=%lu slow=%lu ns/tick=%.5f", g_fc_fast, g_fc_slow, g_fc.nspt); g_fc_fast = g_fc_slow = 0;
        { static int slowrep; static long long cpu0; struct timespec ct; long long cpu = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ct) == 0 ? ct.tv_sec * 1000000000LL + ct.tv_nsec : -1;
          if (in_frame / frames > 0.1 && slowrep < 3 && cpu >= 0) {   // a frame over 100 ms of non-swap time: is this thread burning CPU (then the sample shows where) or waiting
              slowrep++; TR("slowframe: %.0f ms per frame outside the swap, of which this thread's own CPU time is %.0f ms per frame; native stacks of all threads follow", 1e3 * in_frame / frames, (cpu - cpu0) / 1e6 / frames);
              sample_all_threads(); }
          cpu0 = cpu; }
#ifdef PS5_ZINK
        if (g_zk) { g_zk_report = 1; static int zc; if (zc++ % 5 == 0) { long cb = zk_cache_bytes("/app0/radv-shader-cache", 0); TR("zink: shader cache on disk: %ld bytes, %d files, %d dirs in /app0/radv-shader-cache", cb, zk_nf, zk_nd); } }
#endif
        gl_prof_report(frames);
#ifdef PS5_GLALL
        ga_report(frames);
#endif
        if (g_fl_n) { TR("fpslimit: %.1f limiter waits per frame, %.2f ms requested, %.2f ms actually waited per frame (Minecraft is capping the frame rate itself)", (double)g_fl_n / frames, 1e3 * g_fl_req / frames, 1e3 * g_fl_act / frames); g_fl_req = g_fl_act = 0; g_fl_n = 0; }
        if (!g_zk) TR("sdkheap: %zu MB live in the GL layer", ps5_opengl_heap_live_bytes() >> 20);
        { static int nk; if (nk++ % 3 == 0) kmem_report(); }
#ifdef PS5_MEGA
        mg_report(frames);
#endif
#ifdef PS5_VFIX
        { static int last; if (g_vf_ptr + g_vf_nrm + g_vf_sh_ok + g_vf_sh_warn != last) { last = g_vf_ptr + g_vf_nrm + g_vf_sh_ok + g_vf_sh_warn; TR("vfix: pointers converted: %d integer (UV1/UV2), %d normal; shaders rewritten %d, warnings %d", g_vf_ptr, g_vf_nrm, g_vf_sh_ok, g_vf_sh_warn); } }
#endif
#if defined(PS5_NPROF) || defined(PS5_FINISH)
        if (g_fin_n) { TR("finish: glFinish before swap avg %.1f ms max %.1f ms (swap itself %.1f ms)", 1e3 * g_fin_sum / g_fin_n, 1e3 * g_fin_max, 1e3 * in_swap / frames); g_fin_sum = g_fin_max = 0; g_fin_n = 0; }
#endif
#ifdef PS5_NPROF
        np_report();
#endif
        if (g_cur_n) { TR("cursor: overlay %.2f ms/frame avg over %d frames, of which %.2f ms is reading GL state back (now only to seed/verify the shadow state)", 1e3 * g_ovt_s / g_cur_n, g_cur_n, g_ov_query_ms / g_cur_n); g_ovt_s = 0; g_cur_n = 0; g_ov_query_ms = 0; }
        t_win = t1; in_swap = in_frame = 0; frames = 0;
    }
}
static int g_uncap_fail;   // Minecraft's VSync option (glfwSwapInterval); with UNCAP=1 it also selects the console flip mode
void glfwSwapInterval(int i) { g_vsync = i != 0; TR("glfw: swap interval %d (VSync %s)", i, i ? "ON" : "OFF"); if (g_dpy) eglSwapInterval(g_dpy, i); }
void *glfwGetProcAddress(const char *n) { return gl_proc(n); }
int glfwExtensionSupported(const char *n) { (void)n; return 0; }
int glfwWindowShouldClose(void *w) { (void)w; return g_should_close; }
void glfwSetWindowShouldClose(void *w, int v) { (void)w; g_should_close = v; }
void glfwGetFramebufferSize(void *w, int *a, int *b) { (void)w; if (a) *a = g_w; if (b) *b = g_h; { static int n; if (n++ < 4) TR("glfw: framebuffer size asked -> %dx%d", g_w, g_h); } }
// hooks for the native launcher stage (kit_stage.cpp, runs before the JVM in this same window): the pad the shim opened, and a way to hand the EGL context back
int ps5_pad_handle(void) { return g_pad; }
// Start-up peek (launcher.cfg ui=off): is a button held while the title boots? Opens the pad the same way glfwInit does (idempotent) and returns every button seen within <ms> ms (returns early once Square, 0x8000, is seen).
unsigned ps5_pad_peek(int ms) {
    glfwInit();
    if (g_pad < 0) { TR("peek: no pad handle"); return 0; }
    unsigned seen = 0; double t0 = now_s();
    do { PadState s; memset(&s, 0, sizeof s); if (scePadReadState(g_pad, &s) >= 0 && s.connected) seen |= s.buttons & 0x7fffffffu; if (seen & 0x8000u) break; usleep(8000); } while (now_s() - t0 < ms / 1000.0);
    TR("peek: buttons seen in %d ms at start-up: 0x%x", ms, seen);
    return seen;
}
// the game is over (or its window closed): nothing of it may stay on the screen of the launcher - no arrow, no on-screen keyboard, no grabbed-camera state; the next game starts with the pointer in the middle
static void game_input_reset(void) { g_polled = 0; g_cur_vis = 0; g_osk_open = 0; g_osk_l2 = 0; g_osk_lock = 0; g_cursor_mode = 0; g_pad_rest = 0; g_cx = g_w / 2.0; g_cy = g_h / 2.0; }
// the game is over: real GLFW forgets its callbacks in glfwTerminate, this shim never terminates. The next game's glfwSet*Callback returns the PREVIOUS pointer and LWJGL turns it into a Java object
// (Callback.get): the old game freed that thunk (Window.close), so it read freed memory / another round's lambda: ClassCastException "dev$$Lambda cannot be cast to GLFWErrorCallbackI" at the 2nd 1.16.5 start.
static void callbacks_reset(void) { cb_key = 0; cb_char = 0; cb_mouse = 0; cb_pos = 0; cb_scroll = 0; cb_fb = 0; cb_size = 0; cb_focus = 0; /* cb_err stays: it is not tied to a window, and Minecraft 26.x sets it before glfwCreateWindow (GLFWErrorScope) - the loading screen ending inside that call must not wipe it */ cb_drop = cb_close = cb_enter = cb_charmods = cb_wpos = cb_icon = cb_max = 0; }
void ps5_gl_release(void) { game_input_reset(); callbacks_reset(); g_lk_probe = 0; gl_reset_transfer_state("context released"); if (g_dpy) { g_vsync = 1; eglSwapInterval(g_dpy, 1); eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); TR("glfw: context released (thread=%p)", (void *)pthread_self()); } }
void glfwGetWindowSize(void *w, int *a, int *b) { (void)w; if (a) *a = g_w; if (b) *b = g_h; { static int n; if (n++ < 4) TR("glfw: window size asked -> %dx%d", g_w, g_h); } }
void glfwGetWindowPos(void *w, int *a, int *b) { (void)w; if (a) *a = 0; if (b) *b = 0; }
/* The window IS the display: a size the game asks for (VulkanMod's own "Fullscreen Resolution" setting, 1920x1080, through glfwSetWindowMonitor) cannot be applied. Real GLFW tells the game the size it ended up with through the size callbacks;
   without that the game kept its own 1920x1080 while the pad cursor moved in 2560x1440 (log ac0eeea2: cursor off the real spot). Say the real size, so the game re-reads it. */
static void tell_real_size(void *w, int a, int b) {
    if (a == g_w && b == g_h) return;
    { static int n; if (n++ < 6) TR("glfw: the game asked for %dx%d, the window stays %dx%d: telling it through the size callbacks", a, b, g_w, g_h); }
    if (cb_size) cb_size(w, g_w, g_h);
    if (cb_fb) cb_fb(w, g_w, g_h);
}
void glfwSetWindowSize(void *w, int a, int b) { tell_real_size(w, a, b); }
void glfwSetWindowMonitor(void *w, void *mon, int x, int y, int a, int b, int hz) { (void)x; (void)y; (void)hz; { static int n; if (n++ < 6) TR("glfw: SetWindowMonitor(monitor %p, %dx%d)", mon, a, b); } if (mon) tell_real_size(w, a, b); }
void glfwSetWindowPos(void *w, int a, int b) { (void)w; (void)a; (void)b; }
void glfwSetWindowSizeLimits(void *w, int a, int b, int c, int d) { (void)w; (void)a; (void)b; (void)c; (void)d; }
void glfwSetWindowAttrib(void *w, int a, int b) { (void)w; (void)a; (void)b; }
static char g_win_title[256] = "Minecraft";
void glfwSetWindowTitle(void *w, const char *t) { (void)w; if (t) { size_t n = strlen(t); if (n >= sizeof g_win_title) n = sizeof g_win_title - 1; memcpy(g_win_title, t, n); g_win_title[n] = 0; } }
const char *glfwGetWindowTitle(void *w) { (void)w; return g_win_title; }   // GLFW 3.4: LWJGL 3.4's GLFW class fails to initialise without this symbol (Minecraft 26.x)

// GLFW 3.4 IME / preedit API: LWJGL 3.4's GLFW class resolves and calls these unconditionally (Minecraft 26.x NPE at nglfwSetPreeditCallback); no IME on the console
void *glfwSetPreeditCallback(void *w, void *cb) { (void)w; (void)cb; return 0; }
void *glfwSetIMEStatusCallback(void *w, void *cb) { (void)w; (void)cb; return 0; }
void *glfwSetPreeditCandidateCallback(void *w, void *cb) { (void)w; (void)cb; return 0; }
void glfwResetPreeditText(void *w) { (void)w; }
void glfwSetPreeditCursorRectangle(void *w, int x, int y, int ww, int h) { (void)w; (void)x; (void)y; (void)ww; (void)h; }
void glfwGetPreeditCursorRectangle(void *w, int *x, int *y, int *ww, int *h) { (void)w; if (x) *x = 0; if (y) *y = 0; if (ww) *ww = 0; if (h) *h = 0; }
const unsigned *glfwGetPreeditCandidate(void *w, int idx, int *textCount) { (void)w; (void)idx; if (textCount) *textCount = 0; return 0; }
void glfwSetWindowIcon(void *w, int n, const void *i) { (void)w; (void)n; (void)i; }
int glfwGetWindowAttrib(void *w, int a) {
    (void)w;
    switch (a) {
    case 0x20001: case 0x20004: return 1;                     // FOCUSED, VISIBLE
    case 0x30001: return 0x30001;                              // CLIENT_API = OpenGL
    case 0x22002: return 4; case 0x22003: return 6;            // context version
    case 0x22008: return 0x32001;                              // CORE profile
    default: return 0;
    }
}
float glfwGetWindowOpacity(void *w) { (void)w; return 1.0f; }
void glfwGetWindowContentScale(void *w, float *x, float *y) { (void)w; if (x) *x = 1; if (y) *y = 1; }
void *glfwGetPrimaryMonitor(void) { return MON; }
void **glfwGetMonitors(int *n) { static void *m[1] = { &g_mon_tag }; if (n) *n = 1; return m; }
static VidMode g_vm = { 1920, 1080, 8, 8, 8, 60 };
const VidMode *glfwGetVideoMode(void *m) { (void)m; g_vm.width = g_w; g_vm.height = g_h; g_vm.refreshRate = g_hz_real ? g_hz_real : (g_hz >= 120 ? 120 : 60); return &g_vm; }
const VidMode *glfwGetVideoModes(void *m, int *n) { (void)m; if (n) *n = 1; return glfwGetVideoMode(m); }
void glfwGetMonitorPos(void *m, int *a, int *b) { (void)m; if (a) *a = 0; if (b) *b = 0; }
void glfwGetMonitorWorkarea(void *m, int *a, int *b, int *c, int *d) { (void)m; if (a) *a = 0; if (b) *b = 0; if (c) *c = g_w; if (d) *d = g_h; }
void glfwGetMonitorPhysicalSize(void *m, int *a, int *b) { (void)m; if (a) *a = 600; if (b) *b = 340; }
void glfwGetMonitorContentScale(void *m, float *x, float *y) { (void)m; if (x) *x = 1; if (y) *y = 1; }
const char *glfwGetMonitorName(void *m) { (void)m; return "PS5 Display"; }
void *glfwGetWindowMonitor(void *w) { (void)w; return MON; }
void glfwSetInputMode(void *w, int mode, int v) { (void)w;
    if (mode == GLFW_CURSOR) {
        if (g_cursor_mode == GLFW_CURSOR_DISABLED && v != GLFW_CURSOR_DISABLED) { g_cx = g_w / 2.0; g_cy = g_h / 2.0; g_cur_vis = 0; }   // camera released (pause menu, inventory): the pointer is re-centred but stays hidden until the pad/touchpad is used
        g_cursor_mode = v;
    } }
int glfwGetInputMode(void *w, int mode) { (void)w; return mode == GLFW_CURSOR ? g_cursor_mode : 0; }
int glfwGetKey(void *w, int k) { (void)w; return k >= 0 && k <= KEY_MAX ? g_keys[k] : 0; }
int glfwGetMouseButton(void *w, int b) { (void)w; return b >= 0 && b < 8 ? g_mouse[b] : 0; }
void glfwGetCursorPos(void *w, double *x, double *y) { (void)w; if (x) *x = g_cx; if (y) *y = g_cy; }
void glfwSetCursorPos(void *w, double x, double y) { (void)w; g_cx = x; g_cy = y; }
const char *glfwGetKeyName(int k, int s) { (void)k; (void)s; return 0; }
int glfwGetKeyScancode(int k) { return k; }
int glfwRawMouseMotionSupported(void) { return 0; }
int glfwGetPlatform(void) { return 0x60005; }
int glfwPlatformSupported(int p) { return p == 0x60005; }
const char *glfwGetClipboardString(void *w) { (void)w; return ""; }
void glfwSetClipboardString(void *w, const char *s) { (void)w; (void)s; }
// GLFW joystick API for mods with their own controller support (Controlify and the like): joystick 0 is the DualSense. Gamepad layout = GLFW's (A cross, B circle, X square, Y triangle,
// back = touchpad, start = options); the raw joystick layout is the Linux DualSense one (buttons: cross circle triangle square L1 R1 L2 R2 share options PS L3 R3 touchpad).
// Controllers: joystick 0 = the primary pad (opened for the initial user at start-up, also drives the keyboard/mouse emulation); 1..3 = the controllers of the OTHER signed-in users, opened on hot-plug
// (pad_scan, once a second from glfwPollEvents). GLFW games (Legacy4J's split-screen, Controlify...) see them as joysticks 1..3 and get GLFW's connect / disconnect callback.
#define NPAD 4
static int g_xpad[NPAD] = { -1, -1, -1, -1 }, g_xuser[NPAD] = { -1, -1, -1, -1 }; static volatile int g_jpres[NPAD];   // [0] unused for the handle (g_pad)
static void *g_cb_joy;
static int pad_sample_j(int j, PadState *s) {
    memset(s, 0, sizeof *s);
    if (j < 0 || j >= NPAD) return 0;
    int h = j == 0 ? g_pad : g_xpad[j];
    if (h < 0) return 0;
    LPB(1); int pr_ = scePadReadState(h, s); LPA(1);
    if (pr_ < 0 || !s->connected) return 0;
    return 1;
}
static int pad_sample(PadState *s) { return pad_sample_j(0, s); }
static void pad_scan(double t) {
    static double last; static int ev_n; if (t - last < 1.0) return; last = t;
    if (sceUserServiceGetEvent) { UserEv ev; int rc, n = 0; while (n++ < 8 && (rc = sceUserServiceGetEvent(&ev)) == 0) { TR("user: event %s user 0x%x (primary 0x%x)", ev.type == 0 ? "LOGIN" : ev.type == 1 ? "LOGOUT" : "other", (unsigned)ev.user, (unsigned)g_pad_user); if (++ev_n > 200) break; } }
    int ids[4] = { -1, -1, -1, -1 };
    if (sceUserServiceGetLoginUserIdList && sceUserServiceGetLoginUserIdList(ids) == 0) {
        static int prev[4] = { -2, -2, -2, -2 }; if (memcmp(prev, ids, sizeof ids)) { memcpy(prev, ids, sizeof ids); TR("user: signed in 0x%x 0x%x 0x%x 0x%x (primary 0x%x)", (unsigned)ids[0], (unsigned)ids[1], (unsigned)ids[2], (unsigned)ids[3], (unsigned)g_pad_user); }
        for (int i = 0; i < 4; i++) {
            int u = ids[i]; if (u < 0 || u == g_pad_user) continue;
            int slot = -1; for (int j = 1; j < NPAD; j++) if (g_xuser[j] == u) slot = j;
            if (slot < 0) for (int j = 1; j < NPAD; j++) if (g_xuser[j] < 0) { slot = j; break; }
            if (slot < 0 || g_xpad[slot] >= 0) continue;
            static int fails[8]; if (fails[i & 7] >= 3) continue;   // a user whose pad cannot be opened is tried 3 times only
            TR("pad: opening the controller of user 0x%x as joystick %d ...", (unsigned)u, slot);
            int h = scePadOpen(u, 0, 0, 0);
            TR("pad: scePadOpen(user 0x%x) = 0x%x", (unsigned)u, (unsigned)h);
            if (h >= 0) { g_xpad[slot] = h; g_xuser[slot] = u; } else fails[i & 7]++;
        }
    }
    for (int j = 0; j < NPAD; j++) {   // connected / disconnected transitions -> GLFW joystick callback
        PadState s; int now = pad_sample_j(j, &s);
        if (now != g_jpres[j]) { g_jpres[j] = now; TR("pad: joystick %d %s%s", j, now ? "CONNECTED" : "DISCONNECTED", j ? "" : " (primary)"); if (g_cb_joy) ((void (*)(int, int))g_cb_joy)(j, now ? 0x00040001 : 0x00040002); }
    }
}
int glfwJoystickPresent(int j) { PadState s; return j >= 0 && j < NPAD && pad_sample_j(j, &s); }
int glfwJoystickIsGamepad(int j) { return glfwJoystickPresent(j); }
const char *glfwGetGamepadName(int j) { return j >= 0 && j < NPAD ? "PS5 DualSense Wireless Controller" : 0; }
const char *glfwGetJoystickName(int j) { return glfwGetGamepadName(j); }
const char *glfwGetJoystickGUID(int j) { static const char *g[NPAD] = { "030000004c050000e60c000000010000", "030000004c050000e60c000000010001", "030000004c050000e60c000000010002", "030000004c050000e60c000000010003" }; return j >= 0 && j < NPAD ? g[j] : 0; }   // SDL's GUID of a DualSense over USB
void glfwSetJoystickUserPointer(int j, void *p) { (void)j; (void)p; }
void *glfwGetJoystickUserPointer(int j) { (void)j; return 0; }
void *glfwSetJoystickCallback(void *f) { void *o = g_cb_joy; g_cb_joy = f; return o; }
int glfwUpdateGamepadMappings(const char *m) { (void)m; return 1; }
static float pad_axis(int v) { float f = (v - 128) / 127.0f; return f < -1 ? -1 : f; }
int glfwGetGamepadState(int j, void *out) {   // GLFWgamepadstate { unsigned char buttons[15]; float axes[6]; }
    PadState s; unsigned char *bt = (unsigned char *)out; float *ax = (float *)(bt + 16);
    if (!out || !pad_sample_j(j, &s)) return 0;
    uint32_t b = s.buttons; memset(bt, 0, 15);
    bt[0] = !!(b & 0x4000); bt[1] = !!(b & 0x2000); bt[2] = !!(b & 0x8000); bt[3] = !!(b & 0x1000); bt[4] = !!(b & 0x400); bt[5] = !!(b & 0x800);
    bt[6] = !!(b & 0x100000); bt[7] = !!(b & 0x8); bt[9] = !!(b & 0x2); bt[10] = !!(b & 0x4);
    bt[11] = !!(b & 0x10); bt[12] = !!(b & 0x20); bt[13] = !!(b & 0x40); bt[14] = !!(b & 0x80);
    ax[0] = pad_axis(s.lx); ax[1] = pad_axis(s.ly); ax[2] = pad_axis(s.rx); ax[3] = pad_axis(s.ry);
    ax[4] = s.l2 / 127.5f - 1.0f; ax[5] = s.r2 / 127.5f - 1.0f;
    return 1;
}
const float *glfwGetJoystickAxes(int j, int *count) {   // lx ly rx ry l2 r2 (triggers -1..1)
    static float axs[NPAD][6]; PadState s; float *ax = axs[j & 3];
    if (count) *count = 0;
    if (!pad_sample_j(j, &s)) return 0;
    ax[0] = pad_axis(s.lx); ax[1] = pad_axis(s.ly); ax[2] = pad_axis(s.rx); ax[3] = pad_axis(s.ry); ax[4] = s.l2 / 127.5f - 1.0f; ax[5] = s.r2 / 127.5f - 1.0f;
    if (count) *count = 6;
    return ax;
}
const unsigned char *glfwGetJoystickButtons(int j, int *count) {
    static unsigned char bts[NPAD][14]; PadState s; unsigned char *bt = bts[j & 3];
    if (count) *count = 0;
    if (!pad_sample_j(j, &s)) return 0;
    uint32_t b = s.buttons;
    bt[0] = !!(b & 0x4000); bt[1] = !!(b & 0x2000); bt[2] = !!(b & 0x1000); bt[3] = !!(b & 0x8000); bt[4] = !!(b & 0x400); bt[5] = !!(b & 0x800);
    bt[6] = s.l2 > 30; bt[7] = s.r2 > 30; bt[8] = !!(b & 0x1); bt[9] = !!(b & 0x8); bt[10] = 0; bt[11] = !!(b & 0x2); bt[12] = !!(b & 0x4); bt[13] = !!(b & 0x100000);
    if (count) *count = 14;
    return bt;
}
const unsigned char *glfwGetJoystickHats(int j, int *count) {   // one hat: the D-pad (bit 1 up, 2 right, 4 down, 8 left)
    static unsigned char hs[NPAD][1]; PadState s; unsigned char *h = hs[j & 3];
    if (count) *count = 0;
    if (!pad_sample_j(j, &s)) return 0;
    h[0] = (unsigned char)(((s.buttons & 0x10) ? 1 : 0) | ((s.buttons & 0x20) ? 2 : 0) | ((s.buttons & 0x40) ? 4 : 0) | ((s.buttons & 0x80) ? 8 : 0));
    if (count) *count = 1;
    return h;
}
void *glfwGetWindowUserPointer(void *w) { (void)w; static void *p; return p; }
void glfwSetWindowUserPointer(void *w, void *p) { (void)w; (void)p; }
double glfwGetTime(void) { return now_s() - g_t0; }
void glfwSetTime(double t) { g_t0 = now_s() - t; }
uint64_t glfwGetTimerValue(void) { return (uint64_t)(now_s() * 1e9); }
uint64_t glfwGetTimerFrequency(void) { return 1000000000ull; }

#define SETCB(fn, var, ty) void *fn(void *w, void *f) { (void)w; void *o = (void *)var; var = (ty)f; return o; }
SETCB(glfwSetKeyCallback, cb_key, KeyFn) SETCB(glfwSetCharCallback, cb_char, CharFn) SETCB(glfwSetMouseButtonCallback, cb_mouse, MouseFn)
SETCB(glfwSetCursorPosCallback, cb_pos, PosFn) SETCB(glfwSetScrollCallback, cb_scroll, PosFn) SETCB(glfwSetFramebufferSizeCallback, cb_fb, SizeFn)
SETCB(glfwSetWindowSizeCallback, cb_size, SizeFn) SETCB(glfwSetWindowFocusCallback, cb_focus, FocusFn)
SETCB(glfwSetDropCallback, cb_drop, void *) SETCB(glfwSetWindowCloseCallback, cb_close, void *) SETCB(glfwSetCursorEnterCallback, cb_enter, void *)
SETCB(glfwSetCharModsCallback, cb_charmods, void *) SETCB(glfwSetWindowPosCallback, cb_wpos, void *) SETCB(glfwSetWindowIconifyCallback, cb_icon, void *)
SETCB(glfwSetWindowMaximizeCallback, cb_max, void *)

// ---------------- pad -> keyboard / mouse ----------------
// A key / button is down when the pad OR a USB keyboard / mouse holds it (ps5usbin.inc): the pad code sets its own wish every poll, the USB code its own, and the game sees the merge.
static unsigned char g_padk[KEY_MAX + 1], g_usbk[KEY_MAX + 1], g_padm[8], g_usbm[8]; static unsigned g_usb_mods;
static void key_apply(int k) {
    int want = g_padk[k] | g_usbk[k];
    if (g_keys[k] == want) return;
    g_keys[k] = (unsigned char)want;
    if (cb_key) cb_key(WIN, k, k, want ? GLFW_PRESS : GLFW_RELEASE, (int)g_usb_mods);
}
static void set_key(int k, int down) { if (k < 0 || k > KEY_MAX) return; g_padk[k] = down ? 1 : 0; key_apply(k); }
static void usb_key_set(int k, int down) { if (k < 0 || k > KEY_MAX) return; g_usbk[k] = down ? 1 : 0; key_apply(k); }
static void mouse_apply(int b) {
    int want = g_padm[b] | g_usbm[b];
    if (g_mouse[b] == want) return;
    g_mouse[b] = (unsigned char)want;
    if (cb_mouse) cb_mouse(WIN, b, want ? GLFW_PRESS : GLFW_RELEASE, (int)g_usb_mods);
}
static void set_mouse(int b, int down) { if (b < 0 || b > 7) return; g_padm[b] = down ? 1 : 0; mouse_apply(b); }
static void usb_mouse_set(int b, int down) { if (b < 0 || b > 7) return; g_usbm[b] = down ? 1 : 0; mouse_apply(b); }
enum { K_SPACE = 32, K_A = 65, K_D = 68, K_E = 69, K_F = 70, K_Q = 81, K_S = 83, K_T = 84, K_W = 87, K_ESC = 256, K_ENTER = 257, K_RIGHT = 262, K_LEFT = 263, K_DOWN = 264, K_UP = 265, K_F3 = 292, K_F5 = 294, K_B = 66, K_G = 71, K_H = 72, K_KEYN = 78, K_BKSP = 259, K_LSHIFT = 340, K_LCTRL = 341, K_TAB = 258, K_F1 = 290 };
// ---- on-screen keyboard: input handling ----
static void osk_char(unsigned c) {
    if (cb_charmods) ((void (*)(void *, unsigned, int))cb_charmods)(WIN, c, 0);   // MC listens on the char-mods callback
    if (cb_char) cb_char(WIN, c);
}
static void osk_tap_key(int k) { if (cb_key) { cb_key(WIN, k, k, GLFW_PRESS, 0); cb_key(WIN, k, k, GLFW_RELEASE, 0); } }
static void osk_clampfocus(void) { int n = osk_nkeys(g_osk_row); if (g_osk_k >= n) g_osk_k = n - 1; if (g_osk_k < 0) g_osk_k = 0; }
static void osk_move(int dx, int dy) {
    if (dx) { int n = osk_nkeys(g_osk_row); g_osk_k = (g_osk_k + dx + n) % n; }
    if (dy) {
        float c = osk_center(g_osk_row, g_osk_k); int r = (g_osk_row + dy + 5) % 5, best = 0; float bd = 1e9f;
        for (int k = 0; k < osk_nkeys(r); k++) { float d = osk_center(r, k) - c; if (d < 0) d = -d; if (d < bd) { bd = d; best = k; } }
        g_osk_row = r; g_osk_k = best;
    }
    osk_clampfocus();
}
static void osk_press(void) {
    const OskK *k = &OSKR(g_osk_row)[g_osk_k];
    switch (k->sp) { case 1: if (g_osk_lock) g_osk_lock = 0; else g_osk_shift ^= 1; return; case 5: g_osk_page ^= 1; osk_clampfocus(); return; case 2: osk_tap_key(K_BKSP); return; case 3: osk_tap_key(K_ENTER); g_osk_open = 0; g_osk_l2 = 0; g_osk_lock = 0; return; case 4: osk_char(' '); g_osk_shift = 0; return; default: break; }
    osk_char((unsigned char)(OSK_UP() ? k->sc : k->c)); g_osk_shift = 0;
}
static int osk_rep(int slot, int held, double t) {   // edge + auto-repeat (0.35 s, then every 0.08 s)
    static double next[16]; static int was[16];
    if (!held) { was[slot] = 0; return 0; }
    if (!was[slot]) { was[slot] = 1; next[slot] = t + 0.35; return 1; }
    if (t >= next[slot]) { next[slot] = t + 0.08; return 1; }
    return 0;
}
// Instance setting padEmu (instance.cfg, written by the launcher): "off" = the pad is NOT turned into keyboard / mouse events, so a mod with its own controller support
// (it reads the pad through the GLFW joystick functions below) is the only thing that sees it. Read when the window is created: the launcher has chosen the instance by then.
static int g_pad_emu = 1;
static void pad_emu_load(void) {
    char inst[64] = "default", ln[256], path[160]; FILE *f = fopen("/app0/mc/launcher.cfg", "r");
    g_cur_scale = 3; g_hud_on = 0; g_hud_pos = 0; g_hud_bg = 4;
    if (f) { while (fgets(ln, sizeof ln, f)) { if (!strncmp(ln, "instance=", 9)) { snprintf(inst, sizeof inst, "%s", ln + 9); inst[strcspn(inst, "\r\n ")] = 0; } else if (!strncmp(ln, "sched=", 6)) { int n = atoi(ln + 6); if (n == 0 || (n >= 256 && n <= 767)) g_sched_prio = n; } else if (!strncmp(ln, "cursor=", 7)) { int n = atoi(ln + 7); if (n >= 1 && n <= 8) g_cur_scale = n; } else if (!strncmp(ln, "hud=", 4)) { g_hud_on = !strncmp(ln + 4, "on", 2); } else if (!strncmp(ln, "hudbg=", 6)) { int n = atoi(ln + 6); if (n >= 0 && n <= 5) g_hud_bg = n; } else if (!strncmp(ln, "hudpos=", 7)) { static const char *const hp[6] = { "tl", "tc", "tr", "bl", "bc", "br" }; for (int q = 0; q < 6; q++) if (!strncmp(ln + 7, hp[q], 2)) g_hud_pos = q; } } fclose(f); }
    TR("sched: launcher.cfg sched=%d (0 = off)", g_sched_prio); TR("cursor: size %d (launcher.cfg cursor=; 3 = normal)", g_cur_scale); TR("hud: stats overlay %s, position %d (launcher.cfg hud= / hudpos=)", g_hud_on ? "ON" : "off", g_hud_pos);
    if (!inst[0] || strchr(inst, '/') || inst[0] == '.') snprintf(inst, sizeof inst, "default");
    snprintf(path, sizeof path, "/app0/mc/instances/%s/instance.cfg", inst); g_pad_emu = 1;
    f = fopen(path, "r");
    if (f) { while (fgets(ln, sizeof ln, f)) if (!strncmp(ln, "padEmu=", 7)) { char *v = ln + 7; v[strcspn(v, "\r\n ")] = 0; if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_pad_emu = 0; } fclose(f); }
    TR("pad: instance %s: controller emulation (keyboard / mouse) %s", inst, g_pad_emu ? "on" : "OFF (native controller input only)");
}
// Touchpad: ScePadData.touchData (touchNum, then two {x, y: u16, id: u8}), 1920 x 943. PadState.mid[] starts at byte 12 of ScePadData, the touch data at byte 52.
static int pad_touch(const PadState *s, int *x, int *y, int *id) {
    int n = s->mid[40] > 2 ? 0 : s->mid[40];
    if (n < 1) return 0;
    *x = s->mid[48] | (s->mid[49] << 8); *y = s->mid[50] | (s->mid[51] << 8); *id = s->mid[52];
    { static int logged; if (logged < 6 && (logged++, 1)) TR("pad: touch n=%d x=%d y=%d id=%d", n, *x, *y, *id); }
    return 1;
}
// Pad sampler: at a low frame rate a quick tap starts and ends between two glfwPollEvents calls and is lost. This thread samples the pad every ~4 ms and
// latches every button seen pressed since the last poll, so a tap is always delivered as at least one frame of "pressed".
#include <pthread.h>
#define TRIG_ON 150    /* trigger pressed above this (of 255, about 60 %) and released below TRIG_OFF: a light touch no longer places / breaks, like the Bedrock editions */
#define TRIG_OFF 100

// Per-second timeline (one "tl:" line a second, from pad_sampler): everything that could explain a freeze on one line, so the lines before the first stall show what changed. Pieces are read every 3rd second only
// (the probe briefly takes the free 256K pieces of the libc heap).
#ifdef PS5_VK
extern int sceKernelAvailableFlexibleMemorySize(size_t *size) __attribute__((weak));
static void tl_second(unsigned long swaps) {
    static unsigned long prev_sw, prev_su[SU_N]; static unsigned sec; static int prev_pc = -1, pc = -1; static int prev_thr; static long prev_mx;
    sec++;
    int drained = 0;
    if ((sec % 3) == 0) { prev_pc = pc; pc = libc_free_pieces(); drained = (prev_pc >= 0 && pc >= 0) ? prev_pc - pc : 0; }
    char su[200]; size_t o = 0; su[0] = 0;
    for (int i = 0; i < SU_N; i++) { unsigned long v = g_su[i], d = v - prev_su[i]; prev_su[i] = v; if (d && o + 24 < sizeof su) o += snprintf(su + o, sizeof su - o, " %s+%lu", g_su_name[i], d); }
    char omst[170]; omst[0] = 0;
#ifndef PS5_NO_OWNMALLOC
    snprintf(omst, sizeof omst, " om=%luMB/%luMB %luch %lubig %luf %lunew", om_used >> 20, om_peak >> 20, om_chunks, om_dedic_n, om_fail, om_new_n);
#ifndef PS5_NO_RTLDHEAP
    { size_t l = strlen(omst); snprintf(omst + l, sizeof omst - l, " rtld=%lu/%lu/%lu", g_rt_malloc, g_rt_free, g_rt_pm); }
#endif
#endif
    size_t flex = 0; if (sceKernelAvailableFlexibleMemorySize) sceKernelAvailableFlexibleMemorySize(&flex);
    long mx = g_mx_n; int thr = g_reg_n;
    TR("tl: t=%us frames=%lu pieces=%d%s%s mutex+%ld alive=%d attr=%d thr=%d(%+d) flex=%zuMB suspects:%s", sec, swaps - prev_sw, pc, drained > 0 ? " DRAIN" : "", omst, mx - prev_mx, g_mx_real - g_mx_realdel, g_at_n, thr, thr - prev_thr, flex >> 20, su[0] ? su : " none");
    { static int dsamp; if (drained >= 3 && dsamp < 3) { dsamp++; TR("tl: drain #%d: sampling every thread now (who is running while the libc pool shrinks?)", dsamp); sample_all_threads(); } }
    if (drained > 0) TR("tl: libc pool lost %d x256K piece(s) in the last 3 s (suspects above are per second; look at the 3 lines before)", drained);
    prev_sw = swaps; prev_thr = thr; prev_mx = mx;
}
#endif
static volatile int g_froze_whole;
extern int ps5_cv_nudge(void) __attribute__((weak));   // jvm_host.cpp
static void *pad_sampler(void *arg) {
    (void)arg; PadState s; unsigned tick = 0; unsigned long last_cnt = 0; int still = 0, dumps = 0;
    for (;;) {
        usleep(4000);
        if ((++tick & 255) == 0) {   // about once a second: has the render thread stopped presenting? (a 1.20.1 world "froze" with nothing in the log)
            unsigned long c = g_swap_cnt;
#ifdef PS5_VK
            tl_second(c);
#endif
            if (c != 0 && c == last_cnt) {
                still++;
                if ((still == 6 || still == 20) && dumps < 3 && g_render_th_set) {
                    if (still == 6 && dumps == 0) {
#ifdef PS5_VK
                        enomem_probe("stall");
#endif
                        sample_all_threads(); }
                    dumps++; TR("stall: no frame presented for %d s (frame #%lu): sampling the render thread's stack (a CRASH sig=30 block follows; the game keeps running)", still, c);
                    pthread_kill(g_render_th, SIGUSR1);
                    if (ps5_request_java_dump) ps5_request_java_dump();   // and every Java thread's stack (a frozen game: which thread waits for what)
                }
                if (still == 7 || still == 10 || still == 14 || still == 20) { int k = ps5_cv_nudge ? ps5_cv_nudge() : -1; TR("stall: woke %d condition variables that threads were waiting on (a lost wake-up would resume the game now)", k); }
                if (still >= 25 && g_froze_whole && ps5_host_quit) { static int fq; if (!fq++) { TR("stall: the whole JVM has been stopped for %d s: restarting the app (the launcher says so)", still); ps5_host_quit("freeze"); } }
                if (still == 9 && ps5_java_dump_pending && ps5_java_dump_pending()) { g_froze_whole = 1; TR("stall: WHOLE JVM STOPPED - no Java thread answered the stack request for 3 s (some thread does not reach the VM safepoint; the render thread is just parked there)"); sample_all_threads(); }
            } else { if (still >= 6) TR("stall: frames resumed after about %d s", still); still = 0; }
            last_cnt = c;
        }
        if (g_pad < 0) continue;
        memset(&s, 0, sizeof s);
        if (scePadReadState(g_pad, &s) < 0 || !s.connected) continue;
        uint32_t b = s.buttons & 0x7fffffffu & ~0x300u;   // the pad's digital L2/R2 bits fire at the first millimetre of travel: only the analog value counts
        if (s.l2 > TRIG_ON) b |= 0x100;
        if (s.r2 > TRIG_ON) b |= 0x200;
        g_pad_latch |= b;
        // Emergency exit that also works when the game's render thread is frozen (glfwPollEvents is not called then): L1+R1+L3+R3 held 1.5 s asks the game to close, 5 s later the host returns to the launcher
        { static double qh0, qreq; const uint32_t QC = 0x400 | 0x800 | 0x2 | 0x4; double t = now_s();
          if ((b & QC) == QC) { if (!qh0) qh0 = t; else if (!qreq && t - qh0 > 1.5) { qreq = t; g_should_close = 1; TR("pad: quit combo (L1+R1+L3+R3 held 1.5 s): asking the game to close"); } } else qh0 = 0;
          if (qreq && g_should_close && t - qreq > 5.0) { static int hard; if (!hard++) { TR("pad: the game did not close within 5 s: returning to the launcher"); if (ps5_host_quit) ps5_host_quit("pad quit combo"); } } }
    }
    return 0;
}
// Console memory probe (the gallium layer failed "resource-allocate-failed rc=80020023 (ENOMEM) ... limit=0x2dc000000" after many game switches): total / largest free 2 MB-aligned direct block / flexible memory
extern long sceKernelGetDirectMemorySize(void) __attribute__((weak));
extern int sceKernelAvailableDirectMemorySize(long start, long end, size_t align, long *phys, size_t *size) __attribute__((weak));
extern int sceKernelAvailableFlexibleMemorySize(size_t *size) __attribute__((weak));
static void kmem_report(void) {
    long tot = sceKernelGetDirectMemorySize ? sceKernelGetDirectMemorySize() : -1, ph = 0; size_t big = 0, flex = 0; int r1 = -1, r2 = -1;
    if (sceKernelAvailableDirectMemorySize && tot > 0) r1 = sceKernelAvailableDirectMemorySize(0, tot, 0x200000, &ph, &big);
    if (sceKernelAvailableFlexibleMemorySize) r2 = sceKernelAvailableFlexibleMemorySize(&flex);
    TR("kmem: direct total %ld MB, largest free 2 MB-aligned block %zu MB (rc %d) | flexible free %zu MB (rc %d)", tot > 0 ? tot >> 20 : -1, big >> 20, r1, flex >> 20, r2);
#ifdef PS5_VK
    pool_probe();
    { static int calls; if (calls++ == 1) { int e = 0, n = map_headroom(30000, &e); TR("kmem: separate mappings still possible: %d (cap 30000, errno %d)", n, e); char fb[200]; fd_text(fb, sizeof fb, 0); TR("kmem: %s", fb); TR("kmem: jar fds: %d real, %d virtual open (%d with a real fd), %d virtualised in all, %d parked, %d reopened", g_fdv_jars, g_fdv_used, g_fdv_res, g_fdv_virt, g_fdv_ev, g_fdv_reopen); } }   // trend of the mapping headroom (ps5lwjgl.c rev ctl5)
#endif
}
static void start_pad_sampler(void) { static int started; if (started || g_pad < 0) return; started = 1; pthread_t t; if (pthread_create(&t, 0, pad_sampler, 0) == 0) pthread_detach(t); TR("glfw: pad sampler started"); }
#include "ps5usbin.inc"   // USB keyboard + mouse (and ps5_usb_pad_bits() for the launcher)
void glfwPollEvents(void) {
    g_polled = 1; int dbg_layer = 0;
    double t = now_s(), dt = t - g_last_poll, rawdt = dt; g_last_poll = t; if (g_last_poll > 0 && dt < 100 && dt > g_in_maxgap) g_in_maxgap = dt; if (dt > 0.1) { if (dt < 100) g_in_capped++; dt = 0.1; }
    pad_scan(t);
    PadState s; memset(&s, 0, sizeof s);
    int prc = g_pad < 0 ? -2 : scePadReadState(g_pad, &s);
    { static unsigned long n_ok, n_rc, n_disc, n_noh; static int last_rc; static double tl;   // what the pad really returns (GL mode on the AIO title had no input at all): one short line every 5 s
      if (g_pad < 0) n_noh++; else if (prc < 0) { n_rc++; last_rc = prc; } else if (!s.connected) n_disc++; else n_ok++;
      if (t - tl > 5.0) { if (tl > 0) TR("pad: 5 s reads ok %lu, rc<0 %lu (last 0x%x), disconnected %lu, no handle %lu; now btn 0x%x L %d,%d R %d,%d", n_ok, n_rc, (unsigned)last_rc, n_disc, n_noh, (unsigned)s.buttons, s.lx, s.ly, s.rx, s.ry);
        tl = t; n_ok = n_rc = n_disc = n_noh = 0; } }
    if (prc < 0 || !s.connected || (s.buttons & 0x80000000u)) memset(&s, 0, sizeof s), s.lx = s.ly = s.rx = s.ry = 128;
    start_pad_sampler();
    uint32_t b = (s.buttons & ~0x300u) | g_pad_latch; g_pad_latch = 0;   // latched taps count as pressed for this poll
    if (b || s.lx < 100 || s.lx > 156 || s.ly < 100 || s.ly > 156 || s.rx < 100 || s.rx > 156 || s.ry < 100 || s.ry > 156) { if (sdl_pad_armed) { if (!sdl_real_in) TR("pad: first real input (buttons 0x%x)", b); sdl_real_in = 1; } }
    else { sdl_pad_armed = 1; g_pad_rest = 1; }   // the pad has been seen at rest since this game's window was created: from now on a button / stick is input for the game (what was still held from the launcher's menu is not)
    int gui = g_cursor_mode != GLFW_CURSOR_DISABLED; g_rel_dx = g_rel_dy = 0;
    { static uint32_t seen = 0x3FFFFF; uint32_t nb = b & ~seen; if (nb) { seen |= nb; TR("pad: first press of button bit(s) 0x%x (full mask 0x%x)", nb, b); } }   // finds the real touchpad bit
    uint32_t edge = b & ~g_prev_buttons;
    // Emergency exit that does not depend on the game seeing the pad: hold L1 + R1 + L3 + R3 (both sticks pressed) for 1.5 s. The window close is requested first (the game saves and leaves through its own
    // path, back to the launcher); if it is still running 8 s later the host returns to the launcher directly. Works with padEmu off, in menus, in the on-screen keyboard and in gameplay.
    { static double qh0, qreq; const uint32_t QC = 0x400 | 0x800 | 0x2 | 0x4;   /* L1+R1+L3+R3: the Share button never reaches a game */
      if ((b & QC) == QC) { if (!qh0) qh0 = t; else if (!qreq && t - qh0 > 1.5) { qreq = t; TR("pad: quit combo (L1+R1+L3+R3 held 1.5 s): asking the game to close"); g_should_close = 1; if (cb_close) ((void (*)(void *))cb_close)(WIN); } }
      else qh0 = 0;
      if (qreq && t - qreq > 8.0) { static int hard; if (!hard++) { TR("pad: the game did not close within 8 s: returning to the launcher"); if (ps5_host_quit) ps5_host_quit("pad quit combo"); } } }
    if (!g_pad_emu) {   // instance setting: a mod handles the pad itself (a USB keyboard / mouse still works)
        g_prev_buttons = b;
        if (g_cursor_mode == GLFW_CURSOR_DISABLED) g_cur_vis = 0;
        else if (g_pad_rest && b) g_cur_vis = 0;   // the controller spoke (a button; the sticks below): the mouse pointer hides at once, as the emulator's own cursor does after 2 s
        else if (g_pad_rest && (s.lx < 100 || s.lx > 156 || s.ly < 100 || s.ly > 156 || s.rx < 100 || s.rx > 156 || s.ry < 100 || s.ry > 156)) g_cur_vis = 0;
        usb_game(t, dt, g_cursor_mode != GLFW_CURSOR_DISABLED);
        if (g_cur_vis && t - g_cur_t > 2.0) g_cur_vis = 0;   // same idle rule as with the emulator on
        static double qx = -1, qy = -1; if ((g_cx != qx || g_cy != qy) && cb_pos) cb_pos(WIN, g_cx, g_cy); qx = g_cx; qy = g_cy; return; }
    int tx = 0, ty = 0, tid = 0, tdown = pad_touch(&s, &tx, &ty, &tid);   // touchpad finger
    static int tp_has, tp_px, tp_py, tp_pid, tp_tap; static double tp_t0, tp_travel;
    double tdx = 0, tdy = 0;
    if (tdown && tp_has && tid == tp_pid) { tdx = tx - tp_px; tdy = ty - tp_py; tp_travel += sqrt(tdx * tdx + tdy * tdy); }   // finger moved since the last poll
    if (tdown && !tp_has) { tp_t0 = t; tp_travel = 0; }
    if (!tdown && tp_has && t - tp_t0 < 0.28 && tp_travel < 45 && !(b & 0x100000)) tp_tap = 3;   // a light tap (no click, hardly moved) = left click, like the Bedrock editions
    tp_has = tdown; tp_px = tx; tp_py = ty; tp_pid = tid;
    if (!gui) { if (g_osk_open) { g_osk_l2 = 0; g_osk_lock = 0; } g_osk_open = 0; g_cur_vis = 0; }
#ifdef PS5_MEGA
    if (!gui && (edge & 0x100000) && (b & 0x2) && (b & 0x4)) mg_toggle();   // L3 + R3 + touchpad click in gameplay: chunk merging on/off (A/B test of the draw merging; the plain touchpad click is the player list / debug now)
#endif
    if (gui && !g_osk_open && (edge & (0x4 | 0x100000))) { /* R3 or the touchpad CLICK opens the on-screen keyboard (a light tap on the touchpad is the mouse click) */ g_osk_open = 1; g_osk_page = 0; osk_clampfocus(); TR("osk: opened"); edge = 0; g_prev_buttons = b; }
    double lx = (s.lx - 128) / 128.0, ly = (s.ly - 128) / 128.0, rx = (s.rx - 128) / 128.0, ry = (s.ry - 128) / 128.0;
    if (lx > -0.15 && lx < 0.15) lx = 0;  if (ly > -0.15 && ly < 0.15) ly = 0;
    if (rx > -0.15 && rx < 0.15) rx = 0;  if (ry > -0.15 && ry < 0.15) ry = 0;
    static int l2s, r2s;   // trigger with hysteresis
    if (s.l2 > TRIG_ON) l2s = 1; else if (s.l2 < TRIG_OFF) l2s = 0;
    if (s.r2 > TRIG_ON) r2s = 1; else if (s.r2 < TRIG_OFF) r2s = 0;
    int l2 = l2s || (b & 0x100), r2 = r2s || (b & 0x200);
    /* A press that the on-screen keyboard used (open, Options = enter, Circle = close...) must not also reach the game: it stays swallowed until the button is released */
    static uint32_t swallow;
    { uint32_t held = b | (l2 ? 0x100u : 0) | (r2 ? 0x200u : 0); swallow &= held;
      static int pgui; if (gui != pgui) { pgui = gui; swallow |= held; }   // a button held while a screen opens / closes belongs to the old screen (Circle closing the inventory must not also drop the item)
      if (g_osk_open) swallow |= held;
      else { b &= ~swallow; if (swallow & 0x100) l2 = 0; if (swallow & 0x200) r2 = 0; } }
    #define B(m) ((b & (m)) != 0)
    static int prev_r2, prev_l2;
    if (g_osk_open) {                                        // on-screen keyboard owns the pad
        set_mouse(0, 0); set_mouse(1, 0); set_mouse(2, 0); set_key(K_TAB, 0); set_key(K_ESC, 0); set_key(K_LSHIFT, 0); set_key(K_UP, 0); set_key(K_DOWN, 0); set_key(K_LEFT, 0); set_key(K_RIGHT, 0);
        if (osk_rep(0, B(0x10) || ly < -0.6, t)) osk_move(0, -1);
        if (osk_rep(1, B(0x40) || ly > 0.6, t)) osk_move(0, 1);
        if (osk_rep(2, B(0x80) || lx < -0.6, t)) osk_move(-1, 0);
        if (osk_rep(3, B(0x20) || lx > 0.6, t)) osk_move(1, 0);
        if (edge & 0x4000) osk_press();                      // Cross
        if (osk_rep(4, B(0x8000), t)) osk_tap_key(K_BKSP);   // Square: backspace (repeats)
        g_osk_l2 = l2;                                       // L2 held = shift (like the PS5 keyboard); a second press within 0.4 s = caps lock on / off
        if (l2 && !prev_l2) { static double last_l2; if (t - last_l2 < 0.4) { g_osk_lock ^= 1; last_l2 = 0; } else last_l2 = t; }
        if (r2 && !prev_r2) { g_osk_page ^= 1; osk_clampfocus(); }   // R2: letters <-> symbols (the section sign is there)
        if (edge & 0x8) { osk_tap_key(K_ENTER); g_osk_open = 0; g_osk_l2 = 0; g_osk_lock = 0; }   // Options: enter, and the keyboard closes (like the console keyboards)
        if (edge & 0x1000) osk_char(' ');                    // Triangle: space
        if (osk_rep(5, B(0x400), t)) osk_tap_key(K_LEFT);    // L1 / R1: caret left / right
        if (osk_rep(6, B(0x800), t)) osk_tap_key(K_RIGHT);
        if (fabsf((float)rx) > 0.15f) g_osk_ox += (float)rx * 900.0f * UIK * (float)dt;   // right stick: move the panel anywhere on screen (like the PS3 keyboard)
        if (fabsf((float)ry) > 0.15f) g_osk_oy += (float)ry * 900.0f * UIK * (float)dt;
        if (edge & 0x4) { g_osk_ox = 0; g_osk_oy = 0; }      // R3: reset the panel to its docked spot
        if (edge & 0x2) { g_osk_ox = 0; g_osk_oy = 0; g_osk_top ^= 1; }   // L3: dock it at the top / the bottom of the screen
        if (edge & (0x2000 | 0x100000)) { g_osk_open = 0; g_osk_l2 = 0; g_osk_lock = 0; TR("osk: closed"); }
    } else if (gui) {                                        // menus: cursor + clicks
        set_key(K_W, 0); set_key(K_A, 0); set_key(K_S, 0); set_key(K_D, 0);
        g_cx += (rx + lx) * 1100.0 * (g_w / 1920.0) * dt; g_cy += (ry + ly) * 1100.0 * (g_w / 1920.0) * dt;   /* screen-relative speed: the same crossing time at 1080p, 1440p and 4K */
        if (tdx != 0 || tdy != 0) {   // touchpad = trackpad: relative, with acceleration (slow drags are precise, flicks cross the screen), jitter below 2 units ignored
            double v = sqrt(tdx * tdx + tdy * tdy) / (dt > 0.001 ? dt : 0.001), gain = (g_w / 1920.0) * (1.0 + (v / 2500.0 > 1.5 ? 1.5 : v / 2500.0));
            if (fabs(tdx) > 2 || fabs(tdy) > 2) { g_cx += tdx * gain; g_cy += tdy * gain; }
            g_cur_vis = 1; g_cur_t = t;
        }
        if (g_cx < 0) g_cx = 0; if (g_cx > g_w) g_cx = g_w; if (g_cy < 0) g_cy = 0; if (g_cy > g_h) g_cy = g_h;
        if (g_pad_rest && (rx * rx + ry * ry + lx * lx + ly * ly > 0.04 || B(0x4000) || B(0x8000) || B(0x100000) || r2 || l2)) { g_cur_vis = 1; g_cur_t = t; }   // controller activated the cursor (only after the pad was seen at rest in this game: log c05d54c8, the pointer appeared by itself right after launch)
        else if (g_cur_vis && t - g_cur_t > 2.0) g_cur_vis = 0;   // idle for 2 s: the pointer hides again (a USB mouse's too: usb_game keeps g_cur_t fresh while it moves)
        set_mouse(0, g_pad_rest && (B(0x4000) || tp_tap > 0 || r2)); set_mouse(1, g_pad_rest && (B(0x8000) || l2)); set_mouse(2, 0);   // touchpad tap = left click
        if (tp_tap > 0) tp_tap--;
        set_key(K_ESC, B(0x2000) || B(0x8)); set_key(K_LSHIFT, B(0x1000));
        set_key(K_UP, B(0x10)); set_key(K_DOWN, B(0x40)); set_key(K_LEFT, B(0x80)); set_key(K_RIGHT, B(0x20));
        set_key(K_SPACE, 0); set_key(K_E, 0); set_key(K_TAB, 0); set_key(K_T, 0); set_key(K_F5, 0); set_key(K_F3, 0); set_key(K_Q, 0); set_key(K_F, 0); set_key(K_LCTRL, 0);
    } else {                                                 // gameplay: stick = move, right stick = look
        // Layout of the legacy console edition on a PlayStation pad:
        //   Cross jump, Circle drop item, Triangle inventory, Square swap hands, L3 camera view (F5), R3 sneak, no auto-sprint,
        //   L1/R1 hotbar, L2 use, R2 attack, Options pause;  D-pad: up pick block, left debug (tap F3; held: face / shoulder buttons are the F3+key combos), right chat;  touchpad click: player list (Tab)
        int dbg = B(0x80); dbg_layer = dbg;
        set_key(K_F3, dbg); set_key(K_TAB, B(0x100000));
        set_key(K_W, ly < -0.45); set_key(K_S, ly > 0.45); set_key(K_A, lx < -0.45 || (dbg && B(0x1000))); set_key(K_D, lx > 0.45);
        g_rel_dx = rx * 1300.0 * dt; g_rel_dy = ry * 1000.0 * dt; g_cx += g_rel_dx; g_cy += g_rel_dy;
        if (rawdt > 0.1 && rawdt < 100) g_in_lostpx += (rawdt - 0.1) * (fabs(rx) * 1300.0 + fabs(ry) * 1000.0);
        set_key(K_SPACE, !dbg && B(0x4000)); set_key(K_LSHIFT, !dbg && B(0x4));
        set_key(K_F5, !dbg && B(0x2));
        set_key(K_Q, dbg ? B(0x400) : B(0x2000)); set_key(K_E, !dbg && B(0x1000)); set_key(K_F, dbg ? B(0x800) : B(0x8000));
        set_key(K_T, !dbg && B(0x20));
        set_key(K_B, dbg && B(0x4000)); set_key(K_G, dbg && B(0x2000)); set_key(K_H, dbg && B(0x8000)); set_key(K_KEYN, dbg && B(0x10));   // F3+B hitboxes, +G chunk borders, +H tooltips, +A reload chunks, +Q help, +F render distance, +N gamemode
        set_key(K_ESC, B(0x8));
        set_mouse(2, !dbg && B(0x10));   // D-pad up: pick block (middle button)
        set_mouse(0, !dbg && r2); set_mouse(1, !dbg && l2);
    }
    prev_r2 = r2; prev_l2 = l2;
    if (cb_scroll && !g_osk_open && !dbg_layer) {
        if (B(0x400) && !(g_prev_buttons & 0x400)) cb_scroll(WIN, 0.0, 1.0);     // L1: previous hotbar slot
        if (B(0x800) && !(g_prev_buttons & 0x800)) cb_scroll(WIN, 0.0, -1.0);    // R1: next hotbar slot
    }
    g_prev_buttons = b;
    usb_game(t, dt, gui);   // USB keyboard / mouse: merged with what the pad did above
    static double px = -1, py = -1;
    if ((g_cx != px || g_cy != py) && cb_pos) cb_pos(WIN, g_cx, g_cy);
    px = g_cx; py = g_cy;
}
void glfwWaitEvents(void) { usleep(2000); glfwPollEvents(); }
// Minecraft's frame limiter (Blaze3D.limitDisplayFPS) spins on glfwGetTime() and calls this with the time left until the next frame is due. Sleeping a flat 2 ms regardless, as this did,
// quantises every limited frame to a 2 ms grid: with a 60 fps limit that is harmless, but it also makes any higher limit snap to a lower rate. Now the requested time is actually waited.
void glfwWaitEventsTimeout(double t) {
    double t0 = now_s();
    if (t > 0) { if (t > 0.05) t = 0.05; usleep((unsigned)(t * 1e6)); }
    glfwPollEvents();
    g_fl_req += t; g_fl_act += now_s() - t0; g_fl_n++;
    if (g_fl_log < 8) { g_fl_log++; TR("fpslimit: Minecraft asked to wait %.2f ms (its frame limiter is active)", t * 1e3); }
}
void glfwPostEmptyEvent(void) {}

// ---------------- LWJGL's library loading ----------------
static const Sym glfw_syms[] = {
#include "realnames.inc"
#include "stubs.inc"
    {0, 0}
};

// System modules (libSceAgc, libSceVideoOut, ...): the OpenGL SDK finds its GPU and video functions with dlopen/dlsym at run time.
// Our replacements must not swallow those requests, so pass them to the console's own module loader.
extern int sceKernelLoadStartModule(const char *name, size_t argc, const void *argv, unsigned flags, const void *opt, int *res);
extern int sceKernelDlsym(int handle, const char *symbol, void **addrp);
typedef struct { int handle; } ModH;
static ModH g_mods[24]; static int g_nmods;
static void *module_open(const char *name) {
    const char *b = strrchr(name, '/'); b = b ? b + 1 : name;
    int res = 0, h = sceKernelLoadStartModule(b, 0, 0, 0, 0, &res);
    if (h < 0) { TR("module %s: load failed %x", b, h); return 0; }
    for (int i = 0; i < g_nmods; i++) if (g_mods[i].handle == h) return &g_mods[i];
    if (g_nmods >= 24) return 0;
    g_mods[g_nmods].handle = h; TR("module %s -> handle %d", b, h);
    return &g_mods[g_nmods++];
}
static int is_module(void *p) { return (char *)p >= (char *)g_mods && (char *)p < (char *)(g_mods + 24); }
#include "ps5al.inc"   // software OpenAL on sceAudioOut (replaces the earlier sndprobe tone test)
void *ps5_pal_lookup(const char *n) { return pal_lookup(n); }   // ps5lwjgl2.c (LWJGL 2): the same function table
void ps5_l2_log(const char *m) { TR("%s", m); }
#ifdef PS5_VK
#include "ps5vk.inc"   // Vulkan: SDL_Vulkan_* + the display surface on RADV, vkGetInstanceProcAddr for LWJGL's "vulkan" library
#endif
#include "ps5sdl.inc"  // SDL3 subset for Minecraft 26.x (LWJGL 3.4: org.lwjgl.sdl), on top of the GLFW window / EGL / pad code above
#ifdef PS5_FREETYPE
#include "ps5ft_syms.inc"   // generated by build_lwjgl.sh: FT_* function table of the statically linked FreeType (LWJGL 3.4 org.lwjgl.util.freetype)
#else
static int h_ft;
#endif
#ifdef PS5_SPIRV
#include "ps5spirv_syms.inc"   // generated by build_spirv.sh: shaderc_* and spvc_* tables (LWJGL 3.4 org.lwjgl.util.shaderc / org.lwjgl.util.spvc), Minecraft 26.x compiles its shaders at run time
#else
static int h_shaderc, h_spvc;
#endif

void *(*ps5_dll_handle_fn)(void) = 0;   // set by jvm_host.cpp to the ACTIVE JDK's ps5_dl.cpp (the title links several JDKs; each has its own handle and symbol table)
void *(*ps5_dlsym_fn)(const char *) = 0;
static int h_glfw, h_gl, h_self, h_al, h_zink;   // h_al: handle for "openal": dlsym() answers from ps5al.inc (software OpenAL on sceAudioOut)
static const char *dl_err = "";
volatile int ps5_gl_lookups; static int ps5_gl_misses;   // read by the host watchdog
#ifdef PS5_ZINK
// Zink + EGL (zink.o, make_zink_obj.sh). The EGL API is renamed zink_eglXxx (the GL SDK has its own eglXxx). The DRI driver symbols Mesa's loader looks up stay global in zink.o.
extern void *zink_eglGetPlatformDisplay(unsigned, void *, const long *);
extern void *zink_eglGetProcAddress(const char *);
extern unsigned zink_eglInitialize(void *, int *, int *);
extern unsigned zink_eglTerminate(void *);
extern unsigned zink_eglBindAPI(unsigned);
extern unsigned zink_eglChooseConfig(void *, const int *, void **, int, int *);
extern void *zink_eglCreateContext(void *, void *, void *, const int *);
extern unsigned zink_eglMakeCurrent(void *, void *, void *, void *);
extern void *zink_eglCreateWindowSurface(void *, void *, void *, const int *);
extern unsigned zink_eglGetConfigs(void *, void **, int, int *);
extern unsigned zink_eglGetConfigAttrib(void *, void *, int, int *);
extern unsigned zink_eglSwapBuffers(void *, void *);
extern unsigned zink_eglSwapInterval(void *, int);
extern int zink_eglGetError(void);
extern const char *zink_eglQueryString(void *, int);
extern const void **__driDriverGetExtensions_zink(void) __attribute__((weak));
extern const void *__DRI_DRIVER_EXTENSIONS[] __attribute__((weak));
static void *ps5_zink_sym(const char *sym) {
    if (!strcmp(sym, "__driDriverGetExtensions_zink")) return (void *)__driDriverGetExtensions_zink;
    if (!strcmp(sym, "__DRI_DRIVER_EXTENSIONS")) return (void *)__DRI_DRIVER_EXTENSIONS;
    return 0;
}
#endif
void *dlopen(const char *name, int flags) {
    (void)flags; TR("dlopen(%s)", name ? name : "(self)");
    if (!name) return &h_self;
    if (strstr(name, "glfw")) return &h_glfw;
    if (strstr(name, "SDL3")) return &h_sdl;
    if (strstr(name, "freetype")) return &h_ft;
    if (strstr(name, "shaderc")) return &h_shaderc;
    if (strstr(name, "spirv-cross")) return &h_spvc;
    if (strstr(name, "openal")) return &h_al;
#ifdef PS5_ZINK   // Mesa's loader opens the DRI driver by file name ("zink_dri.so" / "libgallium-*.so"): it is linked in, dlsym answers from the table below
    if (strstr(name, "zink_dri") || strstr(name, "libgallium") || strstr(name, "_dri.so")) return &h_zink;
#endif
#ifdef PS5_VK
    if (strstr(name, "vulkan")) return &h_vk;
#endif
    if (strstr(name, "libGL") || strstr(name, "libEGL") || strstr(name, "opengl")) return &h_gl;
    if (strstr(name, ".sprx") || strstr(name, "libSce") || strstr(name, "libkernel")) return module_open(name);
    if (strstr(name, "zip")) return &h_self;
    dl_err = "library is not available in this build"; return 0;
}
// Stand-in for SDL/FreeType functions that are not built in: returns 0, and logs the caller (first 20 calls) so a function Minecraft does call shows up in the log.
static long lib_unimpl(void) { static int n; if (n < 20) { n++; TR("lib: call to an unimplemented SDL/FreeType function from %p", __builtin_return_address(0)); } return 0; }

#ifdef PS5_FREETYPE
// FreeType diagnostics: Minecraft 1.21.x loading a TTF font (Legacy4J) failed with "FreeType error 0x14 (Loading glyph)" = FT_Err_Invalid_Outline. The two wrappers log what the font is (FT_New_Memory_Face) and, when FT_Load_Glyph
// fails, retry once with FT_LOAD_NO_HINTING (the bytecode interpreter / auto-hinter are the usual suspects; the build has no autofit module) and say whether that helped. A successful retry is returned as success.
static int (*ft_real_load)(void *, unsigned, int32_t);
static int (*ft_real_newmem)(void *, const unsigned char *, long, long, void **);
static int ft_newmem_wrap(void *lib, const unsigned char *base, long size, long idx, void **face) {
    int r = ft_real_newmem(lib, base, size, idx, face); static int n;
    if (n < 16) { n++; const char *fam = 0; long ng = -1, fl = -1;
        if (r == 0 && face && *face) { char *f = (char *)*face; memcpy(&fl, f + 16, 8); memcpy(&ng, f + 32, 8); memcpy(&fam, f + 40, sizeof fam); }
        TR("lwjgl: freetype: FT_New_Memory_Face(%ld bytes, magic %02x%02x%02x%02x, index %ld) = 0x%x family '%s' glyphs %ld flags 0x%lx", size, size > 3 ? base[0] : 0, size > 3 ? base[1] : 0, size > 3 ? base[2] : 0, size > 3 ? base[3] : 0, idx, r, fam ? fam : "?", ng, fl); }
    return r;
}
static int ft_load_wrap(void *face, unsigned idx, int32_t flags) {
    int r = ft_real_load(face, idx, flags);
    if (r) { static int n; int r2 = ft_real_load(face, idx, flags | 0x2);
        if (n < 12) { n++; const char *fam = 0; long ng = -1; if (face) { memcpy(&ng, (char *)face + 32, 8); memcpy(&fam, (char *)face + 40, sizeof fam); }
            TR("lwjgl: freetype: FT_Load_Glyph(glyph %u, flags 0x%x) = 0x%x; retry with FT_LOAD_NO_HINTING = 0x%x (face '%s', %ld glyphs)", idx, (unsigned)flags, r, r2, fam ? fam : "?", ng); }
        if (r2 == 0) return 0; }
    return r;
}
#endif
void *dlsym(void *h, const char *sym) {
    if (is_module(h)) {
        void *a = 0; static int miss;
        if (sceKernelDlsym(((ModH *)h)->handle, sym, &a) == 0) return a;
        if (miss++ < 20) TR("dlsym(module, %s): not found", sym);
        return 0;
    }
#ifdef PS5_VK
    if (h == &h_vk) { if (!strcmp(sym, "vkGetInstanceProcAddr")) return (void *)ps5vk_gipa; return (void *)ps5vk_gipa(0, sym); }   // global commands (through the same filter as vkGetInstanceProcAddr); instance / device commands go through vkGetInstanceProcAddr
#endif
#ifdef PS5_ZINK
    if (h == &h_zink) { void *f = ps5_zink_sym(sym); TR("zink: dlsym(driver, %s) -> %p", sym, f); if (!f) dl_err = "symbol not found"; return f; }
#endif
    if (h == &h_al) { void *f = pal_lookup(sym); static int miss; if (!f && miss++ < 12) TR("sndal: dlsym(openal, %s): not found", sym); return f; }
    if (h == &h_sdl) { for (const Sym *x = sdl_syms; x->name; x++) if (!strcmp(x->name, sym)) return x->fn; return (void *)lib_unimpl; }   // LWJGL throws "A required function is missing" for a whole class if ONE of its functions is 0, so functions Minecraft never calls get a stub
#ifdef PS5_FREETYPE
    if (h == &h_ft) { for (const Sym *x = ft_syms; x->name; x++) if (!strcmp(x->name, sym)) {
        if (!strcmp(sym, "FT_Load_Glyph")) { ft_real_load = (int (*)(void *, unsigned, int32_t))x->fn; return (void *)ft_load_wrap; }
        if (!strcmp(sym, "FT_New_Memory_Face")) { ft_real_newmem = (int (*)(void *, const unsigned char *, long, long, void **))x->fn; return (void *)ft_newmem_wrap; }
        return x->fn; } return (void *)lib_unimpl; }
#endif
#ifdef PS5_SPIRV
    if (h == &h_shaderc) { for (const Sym *x = shaderc_syms; x->name; x++) if (!strcmp(x->name, sym)) return x->fn; return (void *)lib_unimpl; }
    if (h == &h_spvc) { for (const Sym *x = spvc_syms; x->name; x++) if (!strcmp(x->name, sym)) return x->fn; return (void *)lib_unimpl; }
#endif
#ifdef PS5_VK
    if (h == &h_glfw && !strncmp(sym, "glfw", 4)) { void *vf = ps5vk_glfw_sym(sym); if (vf) return vf; }
#endif
    if (h == &h_glfw) { for (const Sym *s = glfw_syms; s->name; s++) if (!strcmp(s->name, sym)) return s->fn; TR("dlsym(glfw, %s): not found", sym); dl_err = "symbol not found"; return 0; }
    if (h == &h_gl) {
        if (!strcmp(sym, "glXGetProcAddress") || !strcmp(sym, "glXGetProcAddressARB") || !strcmp(sym, "eglGetProcAddress")) return (void *)gl_proc;
        int n = ++ps5_gl_lookups;
        if (n <= 6) TR("gl lookup #%d %s begin", n, sym);
        void *r = gl_proc(sym);
        if (n <= 6 || (!r && ps5_gl_misses++ < 12)) TR("gl lookup #%d %s -> %p", n, sym, r);
        return r;
    }
    if (ps5_dlsym_fn && (h == &h_self || (ps5_dll_handle_fn && h == ps5_dll_handle_fn()))) {   // JDK native libs are linked in: libjimage looks ZIP_InflateFully up on the zip handle (jdk25), os::dll_load hands out ps5_dll_handle()
        void *f = ps5_dlsym_fn(sym); if (f) return f;
    }
    TR("dlsym(other, %s): not found", sym); dl_err = "symbol not found"; return 0;
}
char *dlerror(void) { const char *e = dl_err; dl_err = ""; return (char *)e; }
int dlclose(void *h) { (void)h; return 0; }
int dlinfo(void *h, int req, void *arg) { (void)h; (void)req; (void)arg; dl_err = "dlinfo is not supported"; return -1; }

// ---------------- access(2) replacement ----------------
// On this console access() fails with EPERM for every path (/app0 and /download0 alike), while stat()/open() work. File.canRead/canWrite and
// Files.checkAccess ride on it: ZipFileSystem (MC opens its own client.jar), log4j config lookup, JNA temp dir, JCE policy files...
// Emulate it with stat(): an existing path is reported readable, writable and executable. (An earlier version refused W_OK under /app0 on the assumption
// that it is read-only; launches have since left logs/ and hs_err_pid*.log inside /app0, so it accepts writes.)
#include <sys/stat.h>
#include <errno.h>
int access(const char *path, int mode) {
    struct stat st;
    (void)mode;
    if (stat(path, &st) != 0) return -1;
    return 0;
}

// ---------------- lstat(2) replacement ----------------
// Hypothesis from the probes: C unlink/rmdir/rename all succeed (pre-jvm, post-jvm-create and in-java), yet Java's Files.delete/Files.move fail with EPERM.
// Both start with UnixFileAttributes.get(path, followLinks=false), i.e. lstat(), while my C probe only used stat(). Symlinks cannot be created here
// (readlink/symlink are stubs), so lstat() == stat(); answer it with stat() in case the sandbox refuses lstat.
#include <sys/stat.h>
int lstat(const char *path, struct stat *st) { return stat(path, st); }

// ---------------- executable memory for libffi closures ----------------
// libffi 3.4.6 on a non-Linux target gives out plain malloc() memory for closure trampolines; executing it faults (SEGV_ACCERR, seen in the MC
// crash at the first GLFW/GL callback). Anonymous RWX mmap works here (it is what HotSpot's code cache uses), so serve closures from that.
// build_lwjgl.sh no longer compiles libffi's closures.c, so these are the only ffi_closure_alloc/free.
#include <sys/mman.h>
#include <pthread.h>
static pthread_mutex_t ffi_m = PTHREAD_MUTEX_INITIALIZER; static char *ffi_cur, *ffi_end; static void *ffi_freel;
void *ffi_closure_alloc(size_t size, void **code) {
    
    void *p = 0;
    if (!code || size > 64) return 0;                       // ffi_closure is 56 bytes; one 64-byte slot size keeps the free list trivial
    pthread_mutex_lock(&ffi_m);
    if (ffi_freel) { p = ffi_freel; ffi_freel = *(void **)p; }
    else {
        if (ffi_cur + 64 > ffi_end) {
            // HotSpot's code cache does mmap(RWX) and then mprotect(RWX) on commit; the first alone left these pages non-executable (SEGV_ACCERR on
            // the first callback), so do both and say what happened.
            void *a = mmap(0, 256 * 1024, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
            if (a == MAP_FAILED) { pthread_mutex_unlock(&ffi_m); TR("ffi_closure_alloc: mmap failed errno=%d", errno); return 0; }
            int mp = mprotect(a, 256 * 1024, PROT_READ | PROT_WRITE | PROT_EXEC);
            TR("ffi_closure_alloc: arena %p mprotect(RWX)=%d errno=%d", a, mp, mp ? errno : 0);
            if (mp) { munmap(a, 256 * 1024); pthread_mutex_unlock(&ffi_m); return 0; }
            ffi_cur = (char *)a; ffi_end = ffi_cur + 256 * 1024;
        }
        p = ffi_cur; ffi_cur += 64;
    }
    pthread_mutex_unlock(&ffi_m);
    *code = p;                                              // writable and executable at the same address
    return p;
}
void ffi_closure_free(void *p) {
    if (!p) return;
    pthread_mutex_lock(&ffi_m); *(void **)p = ffi_freel; ffi_freel = p; pthread_mutex_unlock(&ffi_m);
}

// libffi-3.8-era API that LWJGL 3.4.x's LibFFI/Upcalls reference but the bundled libffi 3.4.6 lacks. ffi_get_closure_size is read at Upcalls class init;
// the call-plan functions are only linked (Minecraft never uses them), so they report "unsupported".
size_t ffi_get_closure_size(void) { return 56; }                // sizeof(ffi_closure) of libffi 3.4.6 x86-64 (ffi.h is not included in every build)
int ffi_get_default_abi(void) { return 2; }                  // FFI_UNIX64 = FFI_DEFAULT_ABI of the x86-64 Unix target
const char *ffi_get_version(void) { return "3.4.6"; }
unsigned long ffi_get_version_number(void) { return 30406UL; }
void *ffi_call_plan_alloc(void *cif) { (void)cif; return 0; }
void ffi_call_plan_free(void *plan) { (void)plan; }
void ffi_call_plan_invoke(void *plan, void (*fn)(void), void *rv, void **av) { (void)plan; (void)fn; (void)rv; (void)av; }
size_t ffi_call_plan_size(void *plan) { (void)plan; return 0; }

// nl_langinfo (SPIRV-Cross asks it for the decimal point) is already provided by the HotSpot port (ps5_support.cpp), so it is not defined here.

// ---------------- GPU-completion polling ----------------
// The SDK waits for the GPU with: poll the completion marker, and if it is not there yet sceKernelUsleep(1000), repeat (batch_end and the synchronous submit
// path). A buffer rewrite that hits a queued draw flushes the batch, so every such flush costs at least one 1 ms sleep even when the GPU needs 100 us
// (measured: glBufferData on a buffer used by a queued draw = 1.1 ms, a rotating buffer = 0.006 ms). Spin for the first polls of each wait instead.
// In the Minecraft log the first version (8 polls per 5 ms window) still left ~39 real 1 ms sleeps per frame: sdkwait: real-1ms-sleeps/f=39.
// build: link adds --wrap=sceKernelUsleep. Every other sleep length passes through unchanged.
extern int __real_sceKernelUsleep(unsigned);
int __wrap_sceKernelUsleep(unsigned us) {
    if (us == 1000) {
        static double last; static int run, logged;
        double t0 = now_s();
        // Polls of ONE wait come back-to-back (a marker check takes microseconds); different waits are separated by the unmap/release/submit work of the
        // flush (hundreds of microseconds). A gap over 50 us therefore starts a new wait. (A 5 ms threshold merged all ~47 flushes of a Minecraft frame into
        // one "wait", so only its first 8 polls were spun and the other ~39 still slept 1 ms each.)
        if (t0 - last > 50e-6) { run = 0; g_wait_starts++; }
        if (++run <= 40) {                                 // 40 x 25 us = the first millisecond of a wait is polled finely; the SDK's 2000-poll budget is barely touched
            if (logged++ < 3) TR("usleep(1000) -> spin 25us (poll %d of a wait)", run);
            g_spin_polls++;
            while (now_s() - t0 < 25e-6) { }
            last = now_s();
            return 0;
        }
        g_real_sleeps++;
        int r = __real_sceKernelUsleep(us); last = now_s(); return r;
    }
    return __real_sceKernelUsleep(us);
}

// ---------------- kernel-call timing (SDK work memory per draw, command submit) ----------------
typedef long (*kfn_t)(long, long, long, long, long, long, long, long, long, long);
extern long __real_sceKernelAllocateDirectMemory(long, long, long, long, long, long, long, long, long, long);
extern long __real_sceKernelMapDirectMemory(long, long, long, long, long, long, long, long, long, long);
extern long __real_munmap(long, long, long, long, long, long, long, long, long, long);
extern long __real_sceKernelReleaseDirectMemory(long, long, long, long, long, long, long, long, long, long);
#ifdef PS5_GLPROF
#define KW(NAME, IDX) long __wrap_##NAME(long a, long b, long c, long d, long e, long f, long g, long h, long i, long j) { \
    double t0 = now_s(); long r = __real_##NAME(a, b, c, d, e, f, g, h, i, j); g_k_t[IDX] += now_s() - t0; g_k_n[IDX]++; return r; }
#else
#define KW(NAME, IDX) long __wrap_##NAME(long a, long b, long c, long d, long e, long f, long g, long h, long i, long j) { return __real_##NAME(a, b, c, d, e, f, g, h, i, j); }
#endif
KW(sceKernelAllocateDirectMemory, K_ALLOC)
KW(sceKernelMapDirectMemory, K_MAP)
KW(munmap, K_MUNMAP)
KW(sceKernelReleaseDirectMemory, K_RELEASE)

// ---------------- directory listing ----------------
// libc opendir() fails with EPERM on this console, but open(O_DIRECTORY) + getdirentries() works (probed on /download0; /app0 needs a buffer of at
// least the filesystem block size). Implement opendir/readdir/closedir on top of that; the records are the native struct dirent.
#include <dirent.h>
#include <fcntl.h>
extern long ps5_getdirentries(int, char *, long, long *) __asm__("getdirentries");
typedef struct { int fd; char *buf; long cap; long len, pos, base; long nent, nopen; } PS5DIR;
static int g_dirlog;   // probe: the first few directory opens/closes are traced (Java 8 aborted in free() inside closedir)
static DIR *ps5_opendir(const char *path) {
    struct stat st; int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return 0;
    long cap = 65536;
    if (fstat(fd, &st) == 0 && st.st_blksize > cap) cap = st.st_blksize;
    PS5DIR *d = (PS5DIR *)calloc(1, sizeof *d);
    char *b = (char *)malloc(cap + 16);
    if (!d || !b) { free(d); free(b); close(fd); errno = ENOMEM; return 0; }
    d->fd = fd; d->buf = b; d->cap = cap; memset(b + cap, 0xA5, 16);
    d->nopen = g_dirlog++;
    if (d->nopen < 12) TR("dir: opendir #%ld %s fd=%d cap=%ld d=%p buf=%p", d->nopen, path, fd, cap, (void *)d, (void *)b);
    return (DIR *)d;
}
static struct dirent *ps5_readdir(DIR *dp) {
    PS5DIR *d = (PS5DIR *)dp;
    for (;;) {
        if (d->pos >= d->len) {
            d->len = ps5_getdirentries(d->fd, d->buf, d->cap, &d->base); d->pos = 0;
            if (d->len <= 0) return 0;
        }
        struct dirent *e = (struct dirent *)(d->buf + d->pos);
        if (e->d_reclen < 8 || d->pos + e->d_reclen > d->len) { TR("dir: bad d_reclen %d at pos=%ld len=%ld", (int)e->d_reclen, d->pos, d->len); return 0; }
        d->pos += e->d_reclen;
        if (e->d_fileno == 0) continue;                    // deleted entry
        d->nent++;
        return e;
    }
}
// Java 8's UnixFileSystem.list calls readdir_r (JDK 17+ call readdir): the console libc's version would dereference our PS5DIR as a real DIR and fault
static int ps5_readdir_r(DIR *dp, struct dirent *entry, struct dirent **result) {
    struct dirent *e = ps5_readdir(dp);
    if (!e) { *result = 0; return 0; }
    memcpy(entry, e, e->d_reclen < sizeof(struct dirent) ? e->d_reclen : sizeof(struct dirent)); *result = entry; return 0;
}
static int ps5_closedir(DIR *dp) { PS5DIR *d = (PS5DIR *)dp; if (!d) return -1;
    { int bad = 0; for (int i = 0; i < 16; i++) if ((unsigned char)d->buf[d->cap + i] != 0xA5) bad = 1;
      if (bad || d->nopen < 12) TR("dir: closedir #%ld entries=%ld canary=%s", d->nopen, d->nent, bad ? "CORRUPT" : "ok"); }
    int r = close(d->fd); free(d->buf); free(d); return r; }
#ifdef PS5_ZINK
static long zk_cache_bytes(const char *dir, int depth) {
    if (!depth) zk_nf = zk_nd = 0;
    DIR *d = ps5_opendir(dir); if (!d) return -1;
    long sum = 0; struct dirent *e;
    while ((e = ps5_readdir(d))) {
        if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
        char path[512]; snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st; if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { zk_nd++; if (depth < 4) { long x = zk_cache_bytes(path, depth + 1); if (x > 0) sum += x; } } else { sum += st.st_size; zk_nf++; }
    }
    ps5_closedir(d); return sum;
}
#endif
// dirfd(): ps5_support.cpp (HotSpot side) already defines it as *(int *)dir, which matches PS5DIR (fd is the first member)
static void ps5_rewinddir(DIR *dp) { PS5DIR *d = (PS5DIR *)dp; if (!d) return; lseek(d->fd, 0, SEEK_SET); d->len = d->pos = 0; d->base = 0; }
DIR *opendir(const char *p) { return ps5_opendir(p); }
struct dirent *readdir(DIR *d) { return ps5_readdir(d); }
int readdir_r(DIR *d, struct dirent *e, struct dirent **r) { return ps5_readdir_r(d, e, r); }
int closedir(DIR *d) { return ps5_closedir(d); }
void rewinddir(DIR *d) { ps5_rewinddir(d); }
#ifdef PS5_VK
// In the merged (vk) link the JDK's libjava reaches libc.prx's opendir directly: the shim above was never bound (no "dir: opendir" trace), so File.list / Files.list got empty
// results on Java 8 (readdir_r) and the launcher config "vanished". --wrap forces every undefined reference in the JDK objects here.
static int g_dirwrap;
DIR *__wrap_opendir(const char *p) { VP; SU(SU_OPENDIR); if (!g_dirwrap++) TR("dir: JDK directory listing goes through the console shim (wrapped)"); return ps5_opendir(p); }
struct dirent *__wrap_readdir(DIR *d) { return ps5_readdir(d); }
int __wrap_readdir_r(DIR *d, struct dirent *e, struct dirent **r) { return ps5_readdir_r(d, e, r); }
int __wrap_closedir(DIR *d) { return ps5_closedir(d); }
void __wrap_rewinddir(DIR *d) { ps5_rewinddir(d); }
#endif

// JNI_OnLoad_<lib>: the JDK finds statically linked libraries by this name
#define ONLOAD(n) JNIEXPORT jint JNICALL JNI_OnLoad_##n(JavaVM *vm, void *r) { (void)vm; (void)r; TR("JNI_OnLoad_" #n); return 0x00010008; }
ONLOAD(lwjgl_opengl) ONLOAD(lwjgl_stb)
// Minecraft 26.2 loads every LWJGL native up front (NativeLibrariesBootstrap, "Loading order: OpenAL,spvc,STB,tinyfd,vma,shaderc,OpenGL,freetype,GLFW") and dies with "Failed to locate library: liblwjgl_tinyfd.so" when one
// has no JNI_OnLoad_<name>. These modules have no natives in this title (file dialogs, jemalloc, ...): a weak onload lets the class initialise; a call into a missing native still throws UnsatisfiedLinkError when (if ever) it is made.
// Weak, so a real one linked later wins. Universal: any later 26.x that preloads another module only needs its name added here.
#define ONLOADW(n) JNIEXPORT __attribute__((weak)) jint JNICALL JNI_OnLoad_##n(JavaVM *vm, void *r) { (void)vm; (void)r; TR("JNI_OnLoad_" #n " (no natives in this title)"); return 0x00010008; }
ONLOADW(lwjgl_tinyfd) ONLOADW(lwjgl_jemalloc) ONLOADW(lwjgl_nfd) ONLOADW(lwjgl_nanovg) ONLOADW(lwjgl_nuklear) ONLOADW(lwjgl_meshoptimizer) ONLOADW(lwjgl_zstd) ONLOADW(lwjgl_lz4) ONLOADW(lwjgl_xxhash)
ONLOADW(lwjgl_par) ONLOADW(lwjgl_yoga) ONLOADW(lwjgl_assimp) ONLOADW(lwjgl_opus) ONLOADW(lwjgl_ktx) ONLOADW(lwjgl_lmdb) ONLOADW(lwjgl_rpmalloc) ONLOADW(lwjgl_remotery) ONLOADW(lwjgl_sse) ONLOADW(lwjgl_hwloc)

// Minecraft 26.2 probes the tinyfd module at start (NativeLibrariesBootstrap.loadTinyFD calls tinyfd_getGlobalChar) and stops the game with UnsatisfiedLinkError when the natives are missing; the module is not linked into this title
// (no file dialogs on a console), so these answer "nothing": no dialog, no value. Weak: a real one linked later wins.
#define TFD(r, n, ...) __attribute__((weak)) JNIEXPORT r JNICALL Java_org_lwjgl_util_tinyfd_TinyFileDialogs_##n(JNIEnv *e, jclass c, ##__VA_ARGS__)
TFD(jlong, ntinyfd_1getGlobalChar, jlong name) { (void)e; (void)c; const char *k = (const char *)(intptr_t)name; static const char ver[] = "ps5-none"; return (k && !strcmp(k, "tinyfd_version")) ? (jlong)(intptr_t)ver : 0; }
TFD(jint, ntinyfd_1getGlobalInt, jlong name) { (void)e; (void)c; (void)name; return 0; }
TFD(jint, ntinyfd_1setGlobalInt, jlong name, jint v) { (void)e; (void)c; (void)name; (void)v; return 0; }
TFD(void, tinyfd_1beep) { (void)e; (void)c; }
TFD(jint, ntinyfd_1notifyPopup, jlong a, jlong b, jlong d) { (void)e; (void)c; (void)a; (void)b; (void)d; return 0; }
TFD(jint, ntinyfd_1messageBox, jlong a, jlong b, jlong d, jlong f, jint g) { (void)e; (void)c; (void)a; (void)b; (void)d; (void)f; (void)g; return 0; }
TFD(jlong, ntinyfd_1inputBox, jlong a, jlong b, jlong d) { (void)e; (void)c; (void)a; (void)b; (void)d; return 0; }
TFD(jlong, ntinyfd_1saveFileDialog, jlong a, jlong b, jint d, jlong f, jlong g) { (void)e; (void)c; (void)a; (void)b; (void)d; (void)f; (void)g; return 0; }
TFD(jlong, ntinyfd_1openFileDialog, jlong a, jlong b, jint d, jlong f, jlong g, jint h) { (void)e; (void)c; (void)a; (void)b; (void)d; (void)f; (void)g; (void)h; return 0; }
TFD(jlong, ntinyfd_1selectFolderDialog, jlong a, jlong b) { (void)e; (void)c; (void)a; (void)b; return 0; }
TFD(jlong, ntinyfd_1colorChooser, jlong a, jlong b, jlong d, jlong f) { (void)e; (void)c; (void)a; (void)b; (void)d; (void)f; return 0; }
#ifdef PS5_VK
ONLOAD(lwjgl_vma) ONLOAD(lwjgl_vulkan)   // Vulkan memory allocator natives (org.lwjgl.util.vma.LibVma); lwjgl_vulkan is only asked for by the macOS bundle but harmless
#endif
// LWJGL core's real JNI_OnLoad (common_tools.c) stores the JavaVM and creates its per-thread storage key: the static-library name must forward to it
extern jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved);
// (the JDK insists on JNI 1.8 or newer for statically linked libraries; LWJGL's own function reports 1.6, so run it and return 1.8 ourselves)
JNIEXPORT jint JNICALL JNI_OnLoad_lwjgl(JavaVM *vm, void *r) { TR("JNI_OnLoad_lwjgl -> JNI_OnLoad"); (void)JNI_OnLoad(vm, r); return 0x00010008; }
// (no ONLOAD(management): libmanagement.a has the real JNI_OnLoad_management, which returns the VM's JNI version >= 1.8)
JNIEXPORT __attribute__((weak)) jint JNICALL JNI_OnLoad_jimage(JavaVM *vm, void *r) { (void)vm; (void)r; TR("JNI_OnLoad_jimage"); return 0x00010008; }   // jdk.internal.jimage.NativeImageBuffer calls System.loadLibrary("jimage"); weak: JDK 25 libjimage.a defines the real one (17/21 do not)

// ---- JNA (com.sun.jna.Native): a built-in "jnidispatch" that lets JNA INITIALISE. The real one is a shared library JNA extracts to a temp file and System.load()s, which this static title cannot do
// ("ps5: cannot load /download0/jna*.tmp"): Native.<clinit> then fails once (oshi, run by Minecraft's crash-report preload, is the first to touch it) and every later use, e.g. Legacy4J's
// Class.forName("com.sun.jna.Native") in its SDL controller handler, throws NoClassDefFoundError and crashes the game. JNA's first choice is System.loadLibrary("jnidispatch"), which finds this
// (JNI_OnLoad_jnidispatch) like the LWJGL libraries. Only the calls Native.<clinit> makes are real; open() always answers "no such library", so Native.load()/register() of any C library throws
// UnsatisfiedLinkError (callers such as oshi and libsdl4j already handle that: the console has no loadable C libraries anyway).
static jstring jna_java_version(JNIEnv *env) {   // the Java side's own VERSION_NATIVE: the compatibility check in Native.<clinit> then always passes, whichever JNA release the mod bundles
    static const char *const cls[] = { "com/sun/jna/Native", "com/sun/jna/Version" };
    for (int i = 0; i < 2; i++) {
        jclass c = (*env)->FindClass(env, cls[i]);
        if (c) { jfieldID f = (*env)->GetStaticFieldID(env, c, "VERSION_NATIVE", "Ljava/lang/String;");
                 if (f) { jstring v = (jstring)(*env)->GetStaticObjectField(env, c, f); if (v) return v; } }
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    }
    return (*env)->NewStringUTF(env, "6.1.0");
}
JNIEXPORT jint JNICALL JNI_OnLoad_jnidispatch(JavaVM *vm, void *r) { (void)vm; (void)r; TR("lwjgl: jna: built-in jnidispatch stub loaded (JNA initialises, native libraries cannot be opened)"); return 0x00010008; }
// one name per game launch (McBoot sets jna.boot.library.name): a builtin library can be loaded by one class loader at a time and the previous game's loader may still be alive
#define JNA_ONLOAD(n) JNIEXPORT jint JNICALL JNI_OnLoad_jnidispatch##n(JavaVM *vm, void *r) { return JNI_OnLoad_jnidispatch(vm, r); }
JNA_ONLOAD(2) JNA_ONLOAD(3) JNA_ONLOAD(4) JNA_ONLOAD(5) JNA_ONLOAD(6) JNA_ONLOAD(7) JNA_ONLOAD(8) JNA_ONLOAD(9) JNA_ONLOAD(10) JNA_ONLOAD(11) JNA_ONLOAD(12) JNA_ONLOAD(13) JNA_ONLOAD(14) JNA_ONLOAD(15) JNA_ONLOAD(16)
JNIEXPORT void JNICALL Java_com_sun_jna_Native_initIDs(JNIEnv *env, jclass c) { (void)env; (void)c; }
JNIEXPORT jstring JNICALL Java_com_sun_jna_Native_getNativeVersion(JNIEnv *env, jclass c) { (void)c; return jna_java_version(env); }
JNIEXPORT jstring JNICALL Java_com_sun_jna_Native_getAPIChecksum(JNIEnv *env, jclass c) { (void)c; return (*env)->NewStringUTF(env, ""); }
JNIEXPORT jint JNICALL Java_com_sun_jna_Native_sizeof(JNIEnv *env, jclass c, jint type) {   // Native.TYPE_VOIDP / LONG / WCHAR_T / SIZE_T / BOOL / LONG_DOUBLE
    (void)env; (void)c;
    switch (type) { case 0: return 8; case 1: return 8; case 2: return 4; case 3: return 8; case 4: return 4; case 5: return 16; default: return 0; }
}
JNIEXPORT void JNICALL Java_com_sun_jna_Native_setProtected(JNIEnv *env, jclass c, jboolean on) { (void)env; (void)c; (void)on; }
JNIEXPORT jboolean JNICALL Java_com_sun_jna_Native_isProtected(JNIEnv *env, jclass c) { (void)env; (void)c; return 0; }
JNIEXPORT void JNICALL Java_com_sun_jna_Native_setLastError(JNIEnv *env, jclass c, jint code) { (void)env; (void)c; (void)code; }
/* "libc" is the one library JNA can "open": a stand-in whose functions all answer 0. Mods that ask the OS about the machine through oshi (VulkanMod's SystemInfo reads the CPU name in a static initialiser with no try/catch: 1.20.1 / 1.21.1 died with
   "Could not initialize class oshi...FreeBsdCentralProcessor", log 04885226) then get blank hardware data instead of a crash. Every other library (SDL, ...) is still "not available". Memory (Pointer / Memory) is real malloc'd memory. */
#define JNA_FAKE_LIBC ((jlong)1)
static int jna_is_libc(const char *s) {
    if (!s) return 0;
    const char *b = strrchr(s, '/'); b = b ? b + 1 : s;
    return !strcmp(b, "c") || !strcmp(b, "libc") || !strncmp(b, "libc.so", 7) || !strncmp(b, "libc.", 5) || !strncmp(b, "liblibc", 7);   // JNA maps the name "libc" to "liblibc.so" on FreeBSD (log fcfd6ad8: "Native.open(liblibc.so) -> not available")
}
JNIEXPORT jlong JNICALL Java_com_sun_jna_Native_open(JNIEnv *env, jclass c, jstring name, jint flags) {
    static int n; (void)c; (void)flags;
    const char *s = name ? (*env)->GetStringUTFChars(env, name, 0) : 0;
    int fake = jna_is_libc(s);
    if (n < 8) { n++; TR("lwjgl: jna: Native.open(%s) -> %s", s ? s : "(self)", fake ? "libc stand-in (every function answers 0)" : "not available"); }
    if (s) (*env)->ReleaseStringUTFChars(env, name, s);
    return fake ? JNA_FAKE_LIBC : 0;
}
JNIEXPORT void JNICALL Java_com_sun_jna_Native_close(JNIEnv *env, jclass c, jlong h) { (void)env; (void)c; (void)h; }
JNIEXPORT jlong JNICALL Java_com_sun_jna_Native_findSymbol(JNIEnv *env, jclass c, jlong h, jstring name) { (void)env; (void)c; (void)name; return h == JNA_FAKE_LIBC ? 1 : 0; }
// Calls into the stand-in library: nothing is called, the result is 0. Only the exact JNA 5.x signatures are exported (the long JNI names): a release with another signature stays "unlinked" (UnsatisfiedLinkError) instead of reading wrong arguments.
#define JNAFN(ret, name, sig, ...) JNIEXPORT ret JNICALL Java_com_sun_jna_Native_##name##__##sig(JNIEnv *env, jclass c, ##__VA_ARGS__)
JNAFN(jint, invokeInt, Lcom_sun_jna_Function_2JI_3Ljava_lang_Object_2, jobject f, jlong fp, jint cc, jobjectArray a) { (void)env; (void)c; (void)f; (void)fp; (void)cc; (void)a; return 0; }
JNAFN(jlong, invokeLong, Lcom_sun_jna_Function_2JI_3Ljava_lang_Object_2, jobject f, jlong fp, jint cc, jobjectArray a) { (void)env; (void)c; (void)f; (void)fp; (void)cc; (void)a; return 0; }
JNAFN(void, invokeVoid, Lcom_sun_jna_Function_2JI_3Ljava_lang_Object_2, jobject f, jlong fp, jint cc, jobjectArray a) { (void)env; (void)c; (void)f; (void)fp; (void)cc; (void)a; }
JNAFN(jfloat, invokeFloat, Lcom_sun_jna_Function_2JI_3Ljava_lang_Object_2, jobject f, jlong fp, jint cc, jobjectArray a) { (void)env; (void)c; (void)f; (void)fp; (void)cc; (void)a; return 0; }
JNAFN(jdouble, invokeDouble, Lcom_sun_jna_Function_2JI_3Ljava_lang_Object_2, jobject f, jlong fp, jint cc, jobjectArray a) { (void)env; (void)c; (void)f; (void)fp; (void)cc; (void)a; return 0; }
JNAFN(jlong, invokePointer, Lcom_sun_jna_Function_2JI_3Ljava_lang_Object_2, jobject f, jlong fp, jint cc, jobjectArray a) { (void)env; (void)c; (void)f; (void)fp; (void)cc; (void)a; return 0; }
JNAFN(jobject, invokeObject, Lcom_sun_jna_Function_2JI_3Ljava_lang_Object_2, jobject f, jlong fp, jint cc, jobjectArray a) { (void)env; (void)c; (void)f; (void)fp; (void)cc; (void)a; return 0; }
JNAFN(jlong, malloc, J, jlong n) { (void)env; (void)c; return (jlong)(intptr_t)calloc(1, n > 0 ? (size_t)n : 1); }
JNAFN(void, free, J, jlong p) { (void)env; (void)c; free((void *)(intptr_t)p); }
#define JNAMEM(T, JT, name) \
    JNAFN(JT, get##name, Lcom_sun_jna_Pointer_2JJ, jobject p, jlong b, jlong o) { (void)env; (void)c; (void)p; JT v; memcpy(&v, (char *)(intptr_t)(b + o), sizeof v); return v; } \
    JNAFN(void, set##name, Lcom_sun_jna_Pointer_2JJ##T, jobject p, jlong b, jlong o, JT v) { (void)env; (void)c; (void)p; memcpy((char *)(intptr_t)(b + o), &v, sizeof v); }
JNAMEM(B, jbyte, Byte) JNAMEM(S, jshort, Short) JNAMEM(C, jchar, Char) JNAMEM(I, jint, Int) JNAMEM(J, jlong, Long) JNAMEM(F, jfloat, Float) JNAMEM(D, jdouble, Double)
JNAFN(void, setMemory, Lcom_sun_jna_Pointer_2JJJB, jobject p, jlong b, jlong o, jlong n, jbyte v) { (void)env; (void)c; (void)p; memset((char *)(intptr_t)(b + o), v, (size_t)n); }
JNAFN(jbyteArray, getStringBytes, Lcom_sun_jna_Pointer_2JJ, jobject p, jlong b, jlong o) {
    (void)c; (void)p; const char *s = (const char *)(intptr_t)(b + o); jsize n = (jsize)strlen(s);
    jbyteArray a = (*env)->NewByteArray(env, n); if (a) (*env)->SetByteArrayRegion(env, a, 0, n, (const jbyte *)s); return a;
}
JNAFN(void, read, Lcom_sun_jna_Pointer_2JJ_3BII, jobject p, jlong b, jlong o, jbyteArray a, jint i, jint n) { (void)c; (void)p; (*env)->SetByteArrayRegion(env, a, i, n, (const jbyte *)(intptr_t)(b + o)); }
JNAFN(void, write, Lcom_sun_jna_Pointer_2JJ_3BII, jobject p, jlong b, jlong o, jbyteArray a, jint i, jint n) { (void)c; (void)p; (*env)->GetByteArrayRegion(env, a, i, n, (jbyte *)(intptr_t)(b + o)); }
JNAFN(void, read, Lcom_sun_jna_Pointer_2JJ_3III, jobject p, jlong b, jlong o, jintArray a, jint i, jint n) { (void)c; (void)p; (*env)->SetIntArrayRegion(env, a, i, n, (const jint *)(intptr_t)(b + o)); }
JNAFN(void, write, Lcom_sun_jna_Pointer_2JJ_3III, jobject p, jlong b, jlong o, jintArray a, jint i, jint n) { (void)c; (void)p; (*env)->GetIntArrayRegion(env, a, i, n, (jint *)(intptr_t)(b + o)); }
JNAFN(void, read, Lcom_sun_jna_Pointer_2JJ_3JII, jobject p, jlong b, jlong o, jlongArray a, jint i, jint n) { (void)c; (void)p; (*env)->SetLongArrayRegion(env, a, i, n, (const jlong *)(intptr_t)(b + o)); }
JNAFN(void, write, Lcom_sun_jna_Pointer_2JJ_3JII, jobject p, jlong b, jlong o, jlongArray a, jint i, jint n) { (void)c; (void)p; (*env)->GetLongArrayRegion(env, a, i, n, (jlong *)(intptr_t)(b + o)); }

// libffi's x86-64 backend references its Windows-ABI (EFI64) entry points; the ms_abi code cannot be built for this target and LWJGL never uses that ABI.
int ffi_prep_cif_machdep_efi64(void *c) { (void)c; return 2; }
int ffi_prep_closure_loc_efi64(void) { return 2; }
int ffi_prep_go_closure_efi64(void) { return 2; }
void ffi_call_efi64(void) {}
void ffi_call_go_efi64(void) {}

// ---------------- native self-test (no JVM): window + 180 presented frames ----------------
typedef void (*ClearColorFn)(float, float, float, float);
typedef void (*ClearFn)(unsigned);
void ps5_gl_selftest(void);
// Kit's direct gl* calls (see gen_gl_thunks.sh): resolved on first use through whichever GL is live (SDK or Zink).
static void ps5_gl_nop(void) {}
void *ps5_gl_resolve(const char *name) {
    void *f = g_use_gl || g_zk ? (void *)eglGetProcAddress(name) : 0;
    if (!f) { TR("gl thunk: %s unresolved (renderer has no GL)", name); f = (void *)ps5_gl_nop; }
    return f;
}

int ps5_hud_hz(void) { return g_hz_real ? g_hz_real : (g_zk ? 60 : (g_hz >= 120 ? 120 : 60)); }   /* what the display really runs: Zink's surfaceless swapchain presents at 60 whatever the setting says */
void ps5_set_hz(int hz) { g_hz = hz >= 120 ? 120 : 60; TR("display: refresh rate setting %d Hz", g_hz); }
void ps5_set_renderer(int r) {   // 0 Vulkan (RADV), 1 OpenGL (SDK), 2 OpenGL through Zink (Mesa on RADV)
    g_use_gl = r == 1;
#ifdef PS5_ZINK
    g_zk = r == 2;
    if (g_zk) {
        setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1); setenv("GALLIUM_DRIVER", "zink", 1);
        setenv("MESA_GL_VERSION_OVERRIDE", "4.6", 1); setenv("MESA_GLSL_VERSION_OVERRIDE", "460", 1);
        // Mesa's disk cache (Zink's NIR + the RADV shader binaries + Zink's pipeline cache): shaders compiled once are not compiled again in the next session -> far less stutter when a shader is first used
        setenv("MESA_SHADER_CACHE_DIR", "/app0/radv-shader-cache", 1); setenv("MESA_SHADER_CACHE_MAX_SIZE", "2G", 1); unsetenv("MESA_SHADER_CACHE_DISABLE"); unsetenv("MESA_GLSL_CACHE_DISABLE");
#ifndef ZINK_DISK_CACHE
#define ZINK_DISK_CACHE 1   /* Mesa's disk shader cache (the A/B test showed it is not what drains the libc heap); 0 = off */
#endif
#if !ZINK_DISK_CACHE
        setenv("MESA_SHADER_CACHE_DISABLE", "true", 1); setenv("MESA_GLSL_CACHE_DISABLE", "true", 1); setenv("RADV_DISABLE_PIPELINE_CACHE", "1", 1);
        TR("zink: Mesa disk shader cache OFF for this build (libc-heap drain A/B test)");
#endif
        mkdir("/app0/radv-shader-cache", 0777);   // the title's folder is fresh after a deploy: Mesa does not create its parents
        {   // why would Mesa's cache stay empty: ids (Mesa switches the cache off when uid != euid) and whether the folder takes a file
            char tp[128]; snprintf(tp, sizeof tp, "/app0/radv-shader-cache/.ps5test"); FILE *tf = fopen(tp, "w"); int e = errno;
            TR("zink: cache dir write test: %s (errno %d); uid=%d euid=%d gid=%d egid=%d", tf ? "ok" : "FAILED", tf ? 0 : e, (int)getuid(), (int)geteuid(), (int)getgid(), (int)getegid());
            if (tf) { fputs("x", tf); fclose(tf); unlink(tp); }
        }
        setenv("PS5_NOMEGA", "1", 1);   // the draw-merge layer is written against the SDK's GL layer
        TR("renderer: OpenGL through Zink");
    }
#else
    if (r == 2) TR("renderer: zink requested but this build has no Zink (ZINK=1): staying on Vulkan");
#endif
}
void ps5_gl_probe(void) { g_use_gl = 1; TR("probe: OpenGL path selected (renderer=gl)"); ps5_gl_selftest(); }
void ps5_gl_selftest(void) {
    TR("selftest: begin");
    if (!glfwInit()) { TR("selftest: glfwInit failed"); return; }
    void *w = glfwCreateWindow(1920, 1080, "selftest", 0, 0);
    if (!w) { TR("selftest: glfwCreateWindow failed"); return; }
    glfwMakeContextCurrent(w);
    ClearColorFn cc = (ClearColorFn)eglGetProcAddress("glClearColor");
    ClearFn cl = (ClearFn)eglGetProcAddress("glClear");
    if (!cc || !cl) { TR("selftest: glClearColor/glClear missing"); return; }
    for (int i = 0; i < 180; i++) {
        float t = i / 180.0f;
        cc(t, 0.2f, 1.0f - t, 1.0f);
        cl(0x00004000u);                       // GL_COLOR_BUFFER_BIT
        glfwSwapBuffers(w);
        if (i < 3) TR("selftest: frame %d presented", i);
        glfwPollEvents();
    }
    TR("selftest: 180 frames presented");
}

#ifndef NO_VIDEO_WRAPS   // set by "novw" builds: no --wrap at link, so the wrappers must not reference __real_*
// ---------------- return-value logging for the SDK's video/GPU calls (the link adds --wrap for each) ----------------
extern int __real_sceVideoOutOpen(int, int, int, const void *);
extern int __real_sceVideoOutSetFlipRate(int, int);
extern int __real_sceVideoOutRegisterBuffers2(int, int, int, void *, int, void *, int, void *);
extern int __real_sceVideoOutSubmitFlip(int, int, unsigned, long);
extern int __real_sceVideoOutIsFlipPending(int);
extern int __real_sceVideoOutWaitVblank(int);
extern int __real_sceVideoOutGetFlipStatus(int, void *);
extern int __real_sceAgcDriverSubmitDcb(void *);
extern unsigned *__real_sceAgcDcbSetFlip(void *, unsigned, int, unsigned, long);
extern unsigned __real_sceAgcDriverWaitUntilSafeForRendering(unsigned **, unsigned, unsigned, unsigned, int);
#define LOGCALL(fmt, ...) do { static int n_; if (n_++ < 14 || r < 0) TR(fmt, __VA_ARGS__); } while (0)
int __wrap_sceVideoOutOpen(int u, int t, int i, const void *p) { int r = __real_sceVideoOutOpen(u, t, i, p); TR("vo Open(user=%d type=%d idx=%d) = %x", u, t, i, r); return r; }
int __wrap_sceVideoOutSetFlipRate(int h, int rate) { int r = __real_sceVideoOutSetFlipRate(h, rate); TR("vo SetFlipRate(%x,%d) = %x", h, rate, r); return r; }
int __wrap_sceVideoOutRegisterBuffers2(int h, int a, int b, void *bufs, int n, void *attr, int c, void *d) { int r = __real_sceVideoOutRegisterBuffers2(h, a, b, bufs, n, attr, c, d); TR("vo RegisterBuffers2(%x, set=%d, start=%d, n=%d) = %x", h, a, b, n, r); return r; }
// Runtime switches from the launcher's instance settings (jvm_host.cpp sets these environment variables from instance.cfg): the build flags only decide the default.
static int uncap_off(void) { static int v = -1; if (v < 0) v = getenv("PS5_NOUNCAP") != 0; return v; }
int __wrap_sceVideoOutSubmitFlip(int h, int idx, unsigned mode, long arg) {
#ifdef PS5_UNCAP
    // VSync OFF in Minecraft: submit the flip in the tearing mode (2) instead of the vsync mode (1) the SDK hard-codes, so frames present as fast as they are rendered
    if (!g_vsync && !g_uncap_fail && !uncap_off() && mode == 1) {
        int r2 = __real_sceVideoOutSubmitFlip(h, idx, 2, arg); static int n; if (n++ < 5) TR("uncap: SubmitFlip in the tearing mode returned 0x%x", r2);
        if (r2 == 0) return 0;
        g_uncap_fail = 1; TR("uncap: the console refused the tearing flip mode (0x%x); falling back to vsynced flips", r2);
    }
#endif
    int r = __real_sceVideoOutSubmitFlip(h, idx, mode, arg); LOGCALL("vo SubmitFlip(%x, buf=%d, mode=%u, arg=%ld) = %x", h, idx, mode, arg, r); return r; }
// SDK 1.0.1 presents with a flip packet in the command buffer (sceAgcDcbSetFlip); sceVideoOutSubmitFlip is not on the present path any more (no "vo SubmitFlip" line ever appeared). Same mode numbers: 1 = vsync, 2 = tearing.
unsigned *__wrap_sceAgcDcbSetFlip(void *cb, unsigned h, int idx, unsigned mode, long marker) {
    static int n_, u_; if (n_++ < 8) TR("agc DcbSetFlip(handle=%x buf=%d mode=%u marker=%lx)", h, idx, mode, marker);
#ifdef PS5_UNCAP
    if (!g_vsync && !g_uncap_fail && !uncap_off() && mode == 1) { mode = 2; if (u_++ < 3) TR("uncap: DcbSetFlip mode 1 -> 2 (tearing)"); }
#endif
    return __real_sceAgcDcbSetFlip(cb, h, idx, mode, marker); }
int __wrap_sceVideoOutIsFlipPending(int h) { int r = __real_sceVideoOutIsFlipPending(h); LOGCALL("vo IsFlipPending(%x) = %d", h, r); return r; }
int __wrap_sceVideoOutWaitVblank(int h) {
#ifdef PS5_UNCAP
    if (!g_vsync && !g_uncap_fail && !uncap_off()) return 0;   // uncapped: do not wait for the display
#endif
    int r = __real_sceVideoOutWaitVblank(h); LOGCALL("vo WaitVblank(%x) = %x", h, r); return r; }
int __wrap_sceVideoOutGetFlipStatus(int h, void *st) { int r = __real_sceVideoOutGetFlipStatus(h, st); LOGCALL("vo GetFlipStatus(%x) = %x", h, r); return r; }
#ifdef PS5_GLPROF
int __wrap_sceAgcDriverSubmitDcb(void *p) { double t0 = now_s(); int r = __real_sceAgcDriverSubmitDcb(p); g_k_t[K_SUBMIT] += now_s() - t0; g_k_n[K_SUBMIT]++; LOGCALL("agc SubmitDcb = %x", r); return r; }
#else
int __wrap_sceAgcDriverSubmitDcb(void *p) { int r = __real_sceAgcDriverSubmitDcb(p); LOGCALL("agc SubmitDcb = %x", r); return r; }
#endif
unsigned __wrap_sceAgcDriverWaitUntilSafeForRendering(unsigned **a, unsigned b, unsigned c, unsigned d, int e) {
    unsigned r0 = __real_sceAgcDriverWaitUntilSafeForRendering(a, b, c, d, e); int r = (int)r0; (void)r; static int n_; if (n_++ < 6) TR("agc WaitUntilSafeForRendering = %u", r0); return r0;
}
#endif // NO_VIDEO_WRAPS

#ifndef NO_PREPARE_FIX   // SDK 1.0.0 passes PS5_SCANOUT_POOL_BYTES itself; the clamp is only needed for the 0.3.0 binary
// ---------------- presentation fix (always on in GL builds; the link adds --wrap=ps5_agc_gate2_prepare_present) ----------------
// The SDK runtime registers the 1080p scanout buffers with 0x1400000 bytes, but eglSwapBuffers asks prepare_present with the 64 MiB render-pool
// size. Its "already registered" shortcut needs registered >= asked, so it failed, tried to reopen the port and refused (unconfirmed flip) -> BAD_SURFACE.
// Asking with the registered size makes the shortcut succeed (verified on the console with the bare trace build).
extern int __real_ps5_agc_gate2_prepare_present(void *, size_t);
int __wrap_ps5_agc_gate2_prepare_present(void *fb, size_t sz) {
    static int n; size_t use = sz > 0x1400000u ? 0x1400000u : sz;
    if (n++ < 2) TR("prepare_present: asked %zu -> %zu", sz, use);
    return __real_ps5_agc_gate2_prepare_present(fb, use);
}
#endif // NO_PREPARE_FIX

// ---------------- name resolution (getaddrinfo) ----------------
// The JDK image's getaddrinfo/freeaddrinfo are stubs that always fail (ps5_jdk_patch.py), and this console has no usable libc resolver. make_jvm_title.sh links with
// --wrap=getaddrinfo --wrap=freeaddrinfo, so the JDK's (and Minecraft's) lookups land here: "localhost", this host's own name and numeric addresses are answered locally, everything else is an
// IPv4 A-record query over UDP (UDP and blocking TCP are proven on this console: net: c: lines, 49 ms to 1.1.1.1). Only blocking calls; poll() bounds the wait. IPv4 only.
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <strings.h>
#ifndef DNS_SERVERS
#define DNS_SERVERS "1.1.1.1", "8.8.8.8"
#endif
#ifndef DNS_PORT
#define DNS_PORT 53
#endif
typedef struct { int fd; short events, revents; } DnsPollFd;
extern int ps5_dns_poll(void *, unsigned long, int) __asm__("poll");   // <poll.h> is not included: it would clash with harness-local names
typedef struct { char name[128]; unsigned char ip[8][4]; int n; double t; } DnsEnt;
static DnsEnt g_dns_cache[16]; static int g_dns_next; static unsigned short g_dns_id = 0x5a17;
static pthread_mutex_t g_dns_mu = PTHREAD_MUTEX_INITIALIZER;
void __wrap_freeaddrinfo(struct addrinfo *ai);
static int dns_skip_name(const unsigned char *p, int n, int i) {      // DNS name (labels or compression pointer) -> index after it, or -1
    while (i < n) { int l = p[i]; if (l == 0) return i + 1; if ((l & 0xC0) == 0xC0) return i + 2 <= n ? i + 2 : -1; i += l + 1; }
    return -1;
}
// one query/answer exchange; returns the number of A records (0 = none), -1 = no usable reply; *rcode = DNS response code
static int dns_ask(const char *server, const char *name, unsigned char ip[][4], int max, int *rcode) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    unsigned char q[300]; int n = 0; unsigned short id = ++g_dns_id;
    q[n++] = id >> 8; q[n++] = id & 255; q[n++] = 0x01; q[n++] = 0x00; q[n++] = 0; q[n++] = 1; for (int k = 0; k < 6; k++) q[n++] = 0;
    for (const char *p = name; *p;) { const char *d = strchr(p, '.'); int l = d ? (int)(d - p) : (int)strlen(p); q[n++] = (unsigned char)l; memcpy(q + n, p, l); n += l; p += l; if (d) p++; }
    q[n++] = 0; q[n++] = 0; q[n++] = 1; q[n++] = 0; q[n++] = 1;
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(DNS_PORT); inet_pton(AF_INET, server, &a.sin_addr);
    int res = -1;
    if (sendto(s, q, n, 0, (struct sockaddr *)&a, sizeof a) == n) {
        DnsPollFd pf; pf.fd = s; pf.events = 1 /* POLLIN */; pf.revents = 0;
        if (ps5_dns_poll(&pf, 1, 2000) > 0) {
            unsigned char r[600]; ssize_t rr = recvfrom(s, r, sizeof r, 0, 0, 0);
            if (rr >= 12 && r[0] == (id >> 8) && r[1] == (id & 255) && (r[2] & 0x80)) {
                int an = (r[6] << 8) | r[7], i = 12, got = 0; *rcode = r[3] & 15;
                i = dns_skip_name(r, (int)rr, i); i = i < 0 ? -1 : i + 4;
                for (int k = 0; k < an && i > 0 && i < (int)rr; k++) {
                    i = dns_skip_name(r, (int)rr, i); if (i < 0 || i + 10 > (int)rr) break;
                    int type = (r[i] << 8) | r[i + 1], rdlen = (r[i + 8] << 8) | r[i + 9]; i += 10;
                    if (type == 1 && rdlen == 4 && i + 4 <= (int)rr && got < max) { memcpy(ip[got], r + i, 4); got++; }
                    i += rdlen;
                }
                res = got;
            }
        }
    }
    close(s);
    return res;
}
static int dns_valid_name(const char *s) {
    size_t n = strlen(s), run = 0; if (n == 0 || n > 253) return 0;
    for (size_t i = 0; i < n; i++) { if (s[i] == '.') { if (run == 0) return 0; run = 0; } else if (++run > 63) return 0; }
    return run > 0 || s[n - 1] == '.';
}
int __wrap_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
    if (!res) return EAI_FAIL;
    *res = 0;
#ifdef PS5_VK
    TR("net: gai(%s, %s) family=%d", node ? node : "(null)", service ? service : "(null)", hints ? hints->ai_family : -1);
#endif
    int flags = hints ? hints->ai_flags : 0, family = hints ? hints->ai_family : AF_UNSPEC, socktype = hints && hints->ai_socktype ? hints->ai_socktype : SOCK_STREAM;
    if (family != AF_UNSPEC && family != AF_INET) return EAI_FAMILY;
    unsigned short port = 0;
    if (service && *service) { char *e; long p = strtol(service, &e, 10); if (*e || p < 0 || p > 65535) return EAI_SERVICE; port = (unsigned short)p; }
    unsigned char ips[8][4]; int n = 0; char name[128] = "";
    if (!node || !*node) { memcpy(ips[0], (flags & AI_PASSIVE) ? "\0\0\0\0" : "\x7f\0\0\x01", 4); n = 1; }
    else {
        struct in_addr lit; char me[128] = "";
        gethostname(me, sizeof me - 1);
        if (inet_pton(AF_INET, node, &lit) == 1) { memcpy(ips[0], &lit, 4); n = 1; }
        else if (flags & AI_NUMERICHOST) return EAI_NONAME;
        else if (!strcasecmp(node, "localhost") || (me[0] && !strcasecmp(node, me))) { memcpy(ips[0], "\x7f\0\0\x01", 4); n = 1; }
        else {
            if (strlen(node) >= sizeof name || !dns_valid_name(node)) return EAI_NONAME;
            for (int i = 0; node[i]; i++) name[i] = (node[i] >= 'A' && node[i] <= 'Z') ? node[i] + 32 : node[i];
            if (name[strlen(name) - 1] == '.') name[strlen(name) - 1] = 0;
            double t0 = now_s(); int rcode = 0, hit = 0;
            pthread_mutex_lock(&g_dns_mu);
            for (int i = 0; i < 16 && !n; i++) if (g_dns_cache[i].n && t0 - g_dns_cache[i].t < 60 && !strcmp(g_dns_cache[i].name, name)) { memcpy(ips, g_dns_cache[i].ip, sizeof ips); n = g_dns_cache[i].n; hit = 1; }
            const char *servers[] = { DNS_SERVERS }; const char *used = "";
            for (unsigned si = 0; si < sizeof servers / sizeof *servers && !n; si++)
                for (int tries = 0; tries < 2 && !n; tries++) {
                    int r = dns_ask(servers[si], name, ips, 8, &rcode);
                    if (r > 0) { n = r; used = servers[si]; }
                    else if (r == 0 && (rcode == 0 || rcode == 3)) { used = servers[si]; si = 99; break; }     // NOERROR without A record / NXDOMAIN: do not ask the others (SERVFAIL etc. do try the next)
                }
            if (n && !hit) { DnsEnt *e = &g_dns_cache[g_dns_next++ & 15]; snprintf(e->name, sizeof e->name, "%s", name); memcpy(e->ip, ips, sizeof ips); e->n = n; e->t = t0; }
            pthread_mutex_unlock(&g_dns_mu);
            if (!n) { TR("net: dns %s FAILED rcode=%d (%.0f ms)", name, rcode, (now_s() - t0) * 1000); return rcode == 3 || used[0] ? EAI_NONAME : EAI_AGAIN; }
            TR("net: dns %s -> %u.%u.%u.%u (%d addr%s, %s, %.0f ms)", name, ips[0][0], ips[0][1], ips[0][2], ips[0][3], n, n == 1 ? "" : "s", hit ? "cached" : used, (now_s() - t0) * 1000);
        }
    }
    struct addrinfo *head = 0, **tail = &head;
    size_t cn = node ? strlen(node) + 1 : 1;
    for (int i = 0; i < n; i++) {
        struct addrinfo *ai = (struct addrinfo *)calloc(1, sizeof *ai + sizeof(struct sockaddr_in) + cn);
        if (!ai) { __wrap_freeaddrinfo(head); return EAI_MEMORY; }
        struct sockaddr_in *sa = (struct sockaddr_in *)(ai + 1);
        sa->sin_family = AF_INET; sa->sin_port = htons(port); memcpy(&sa->sin_addr, ips[i], 4);
        ai->ai_family = AF_INET; ai->ai_socktype = socktype; ai->ai_protocol = hints ? hints->ai_protocol : 0; ai->ai_addrlen = sizeof *sa; ai->ai_addr = (struct sockaddr *)sa;
        if (i == 0 && (flags & AI_CANONNAME)) { ai->ai_canonname = (char *)(sa + 1); snprintf(ai->ai_canonname, cn, "%s", node ? node : ""); }
        *tail = ai; tail = &ai->ai_next;
    }
    *res = head;
    return 0;
}
void __wrap_freeaddrinfo(struct addrinfo *ai) { while (ai) { struct addrinfo *nx = ai->ai_next; free(ai); ai = nx; } }

// ---------------- non-blocking sockets (emulated) ----------------
// This console refuses fcntl(F_SETFL, O_NONBLOCK) and ioctl(FIONBIO) on sockets (EPERM/EACCES), and SOCK_NONBLOCK only sets the flag bit (a "non-blocking" connect/accept still blocked).
// Java NIO / Netty need non-blocking sockets (ServerSocketChannel, SocketChannel+Selector, every socket with a timeout) -> "Unable to create Channel from class NioServerSocketChannel".
// What does work: recv/send with MSG_DONTWAIT, poll(), blocking connect. So: when F_SETFL(O_NONBLOCK) is refused on a socket, remember the fd (g_nb) and pretend it worked; for such fds
// read/recv/readv become MSG_DONTWAIT receives (EAGAIN when empty), write/send/writev MSG_DONTWAIT sends, accept polls first (EAGAIN when nobody is waiting), F_GETFL reports O_NONBLOCK.
// connect() stays blocking (it then simply finishes at once; the JDK treats that as "connected immediately"). close() forgets the fd. Anything that is not a refused socket passes straight through.
// Linked in through --wrap=fcntl/read/write/readv/writev/recv/recvfrom/send/sendto/accept/close (make_jvm_title.sh).
#include <fcntl.h>
#include <sys/uio.h>
#define NB_MAX 4096
static volatile unsigned char g_nb[NB_MAX];   // 0 = untouched (kernel flags as they are), 1 = emulated non-blocking, 2 = emulated blocking (the kernel flag bit may still read "non-blocking")
extern int __real_fcntl(int, int, long);
extern int __real_close(int);
extern long __real_read(int, void *, size_t);
extern long __real_write(int, const void *, size_t);
extern long __real_readv(int, const struct iovec *, int);
extern long __real_writev(int, const struct iovec *, int);
extern long __real_recv(int, void *, size_t, int);
extern long __real_recvfrom(int, void *, size_t, int, struct sockaddr *, socklen_t *);
extern long __real_send(int, const void *, size_t, int);
extern long __real_sendto(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
extern int __real_accept(int, struct sockaddr *, socklen_t *);
// ioctl(FIONREAD) ("how many bytes can be read") is refused on sockets here too (EACCES), and the JDK reports that as BindException "Permission denied" from Net.available(): every
// BufferedInputStream over a socket (the game's HttpURLConnection: skins, capes, profile lookups) failed with it. The count is taken from a peek instead (0 when nothing is waiting).
extern int __real_ioctl(int, unsigned long, ...) __attribute__((weak));
int __wrap_ioctl(int fd, unsigned long req, ...) {
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
    int r = __real_ioctl ? __real_ioctl(fd, req, arg) : -1;
    if (r == 0 || req != 0x4004667fUL || !arg) return r;   // FIONREAD = _IOR('f', 127, int)
    int e = errno; if (e != EACCES && e != EPERM && e != ENOTTY && e != EINVAL) return r;
    char tmp[4096]; long n = __real_recv(fd, tmp, sizeof tmp, MSG_PEEK | MSG_DONTWAIT);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) { errno = e; return r; }   // not a socket / a real error: report the original failure
    *(int *)arg = n > 0 ? (int)n : 0; return 0;
}
#define NB(fd) ((unsigned)(fd) < NB_MAX && g_nb[fd] == 1)
static int nb_is_socket(int fd) { int t; socklen_t l = sizeof t; return getsockopt(fd, SOL_SOCKET, SO_TYPE, &t, &l) == 0; }
// ANY F_SETFL that changes O_NONBLOCK on a socket fails here (errno 13) - also clearing it: an accepted socket inherits the flag bit from its listener, and the JDK clears it again right after
// accept() ("newly accepted socket is initially in blocking mode"), which failed with "Permission denied" (log: ServerSocketChannelImpl.finishAccept -> IOUtil.configureBlocking).
// So both directions are emulated, and F_GETFL reports the emulated state.
int __wrap_fcntl(int fd, int cmd, ...) {
    va_list ap; va_start(ap, cmd); long arg = va_arg(ap, long); va_end(ap);
    if (FDV_IS(fd)) return fdv_fcntl(fd, cmd, arg);
    if (cmd == F_SETFL && (unsigned)fd < NB_MAX) {
        int want = (arg & O_NONBLOCK) != 0;
        int r = __real_fcntl(fd, cmd, arg);
        if (r == 0) { g_nb[fd] = 0; return 0; }
        int e = errno;
        if ((e == EPERM || e == EACCES) && nb_is_socket(fd)) { static int n; g_nb[fd] = want ? 1 : 2; if (n++ < 8) TR("net: nb: fd %d switched to emulated %s (fcntl said errno %d)", fd, want ? "non-blocking" : "blocking", e); return 0; }
        errno = e; return r;
    }
    if (cmd == F_GETFL) {
        int r = __real_fcntl(fd, cmd, arg);
        if (r >= 0 && (unsigned)fd < NB_MAX) { if (g_nb[fd] == 1) r |= O_NONBLOCK; else if (g_nb[fd] == 2) r &= ~O_NONBLOCK; }
        return r;
    }
    return __real_fcntl(fd, cmd, arg);
}
// per-call cost of file I/O (log c0ec3b8b: Quilt's cache copy crawls again, stacks in FileDispatcherImpl.seek0/read): count and total time of write / read / lseek / pread, one line every 10 s while there is traffic
volatile unsigned long ps5_io_n[4], ps5_io_ns[4];   // 0 write, 1 read, 2 lseek, 3 pread
static inline unsigned long io_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (unsigned long)t.tv_sec * 1000000000UL + t.tv_nsec; }
#define IO_T0() unsigned long _t0 = io_now()
#define IO_T1(k) do { __sync_fetch_and_add(&ps5_io_n[k], 1); __sync_fetch_and_add(&ps5_io_ns[k], io_now() - _t0); } while (0)
void ps5_wb_flush(void);
void ps5_wb_flush_for(int fd);
long __wrap_read(int fd, void *b, size_t n) { ps5_wb_flush_for(fd); if (FDV_IS(fd)) return fdv_read(fd, b, n); if (NB(fd)) return __real_recv(fd, b, n, MSG_DONTWAIT); IO_T0(); long r = __real_read(fd, b, n); IO_T1(1); return r; }
// Small-write coalescing for regular files (log 251cb564: Quilt's "Copying minecraft to a temporary jar file" runs ZipOutputStream straight on an unbuffered channel, every writeInt() is four 1-byte write()s: the jar grew
// 2-3 KB/s, i.e. ~2 ms per write() on this console, hours for the 25 MB jar). Writes of up to 512 bytes to a regular file (not fd 0-2, not a socket/pipe) collect in one 64 KB buffer and go out as one write() when it is
// full, when a different fd writes, before anything that could observe or reorder the file (read, readv, pread, lseek, fstat of the same file; writev, pwrite, ftruncate, fsync, fdatasync, dup, dup2, close of any fd) and at the latest 100 ms later (flusher thread).
// WB_OFF=1 in the environment turns it off. The caller gets "all bytes written"; a write error surfaces on a later call or is lost (disk full), like any buffered file system.
#include <pthread.h>
extern long __real_lseek(int, long, int);
extern long __real_pread(int, void *, size_t, long);
extern long __real_pwrite(int, const void *, size_t, long);
extern int __real_ftruncate(int, long);
extern int __real_fsync(int);
extern int __real_fdatasync(int);
extern int __real_fstat(int, struct stat *);
extern int __real_dup(int);
extern int __real_dup2(int, int);
void ps5_io_note(int k, long ns) { __sync_fetch_and_add(&ps5_io_n[k], 1); __sync_fetch_and_add(&ps5_io_ns[k], (unsigned long)ns); }
static pthread_mutex_t wb_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile int wb_fd = -1; static unsigned wb_len; static unsigned char wb_buf[65536]; static unsigned char g_reg[NB_MAX];   // g_reg: 0 unknown, 1 regular file, 2 anything else
static int wb_started, wb_off = -1; static unsigned long wb_calls, wb_writes; static dev_t wb_dev; static ino_t wb_ino; static int wb_ino_ok;   // identity of the file the pending bytes belong to
static void wb_flush_locked(void) { if (wb_fd >= 0 && wb_len) { unsigned o = 0; wb_writes++; while (o < wb_len) { long r = __real_write(wb_fd, wb_buf + o, wb_len - o); if (r <= 0) break; o += (unsigned)r; } } wb_len = 0; wb_fd = -1; }
void ps5_wb_flush(void);
void ps5_wb_flush(void) { if (wb_fd < 0) return; int e = errno; pthread_mutex_lock(&wb_mu); wb_flush_locked(); pthread_mutex_unlock(&wb_mu); errno = e; }
// Reads / seeks / stats of ANOTHER file do not need the pending bytes of this one: log 458247f5 (Quilt "Copying minecraft to a temporary jar file", 130 s): the copy reads the source jar 512 bytes at a time (lseek + read) while writing the new jar in tiny
// pieces, every lseek flushed the pending ~100 bytes as its own write() (~10 ms each on this console, 84 per second), the thread sat in lseek all the time. Now only an access to the same file (same descriptor or same inode) flushes; the 100 ms flusher
// thread, a full buffer, a write to another file and close/fsync/dup still do.
void ps5_wb_flush_for(int fd) {
    if (wb_fd < 0) return;
    if (fd == wb_fd) { ps5_wb_flush(); return; }
    if (FDV_IS(fd) || NB(fd)) return;   // jars read through virtual descriptors and sockets are never the file being written
    struct stat st; int same = 1, e = errno;
    if (__real_fstat(fd, &st) == 0) { pthread_mutex_lock(&wb_mu); same = wb_fd >= 0 && (!wb_ino_ok || (st.st_ino == wb_ino && st.st_dev == wb_dev)); pthread_mutex_unlock(&wb_mu); }
    errno = e;
    if (same) ps5_wb_flush();
}
static void *wb_thread(void *a) { (void)a; unsigned long last[4] = {0}, lns[4] = {0}, lc = 0, lw = 0; int tick = 0; for (;;) { usleep(100000); ps5_wb_flush();
        if (++tick % 100) continue;
        unsigned long n[4], ns[4]; int any = 0; for (int k = 0; k < 4; k++) { n[k] = ps5_io_n[k]; ns[k] = ps5_io_ns[k]; if (n[k] != last[k]) any = 1; }
        if (any) { char b[190]; int o = snprintf(b, sizeof b, "io: 10 s:"); const char *nm[4] = { "write", "read", "lseek", "pread" };
            for (int k = 0; k < 4; k++) { unsigned long dn = n[k] - last[k], dt = ns[k] - lns[k]; if (dn) o += snprintf(b + o, sizeof b - o, " %s x%lu avg %lu us;", nm[k], dn, dt / dn / 1000); last[k] = n[k]; lns[k] = ns[k]; }
            snprintf(b + o, sizeof b - o, " coalesced %lu in %lu writes", wb_calls - lc, wb_writes - lw); lc = wb_calls; lw = wb_writes; TR("%s", b); } }
    return 0; }
long __wrap_write(int fd, const void *b, size_t n) {
    if (!wb_started) { wb_started = 1; pthread_t t; if (pthread_create(&t, 0, wb_thread, 0) == 0) pthread_detach(t); TR("io: write wrapper active (small writes to regular files are coalesced, 64 KB buffer, flushed within 100 ms)"); }
    if (wb_off < 0) { const char *e = getenv("WB_OFF"); wb_off = e && *e == '1'; }
    if (!wb_off && fd > 2 && (unsigned)fd < NB_MAX && n > 0 && n <= 512 && !NB(fd) && !FDV_IS(fd)) {
        if (!g_reg[fd]) { struct stat st; g_reg[fd] = (__real_fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) ? 1 : 2; }
        if (g_reg[fd] == 1) {
            pthread_mutex_lock(&wb_mu);
            if (wb_fd != fd) wb_flush_locked();
            if (wb_len + n > sizeof wb_buf) wb_flush_locked();
            if (wb_fd != fd) { struct stat st2; wb_ino_ok = __real_fstat(fd, &st2) == 0; if (wb_ino_ok) { wb_dev = st2.st_dev; wb_ino = st2.st_ino; } }
            wb_fd = fd; memcpy(wb_buf + wb_len, b, n); wb_len += (unsigned)n; wb_calls++;
            pthread_mutex_unlock(&wb_mu);
            return (long)n;
        }
    }
    ps5_wb_flush();
    if (!NB(fd) && n > (1u << 20)) n = 1u << 20;   // one write() call moves at most 1 MB (log e5730a79: a single 24 MB write once looked like a hang; a short count is legal and every Java caller loops)
    if (NB(fd)) return __real_send(fd, b, n, MSG_DONTWAIT);
    IO_T0(); long r = __real_write(fd, b, n); IO_T1(0); return r;
}
long __wrap_pwrite(int fd, const void *b, size_t n, long o) { ps5_wb_flush(); return __real_pwrite(fd, b, n, o); }
int __wrap_ftruncate(int fd, long l) { ps5_wb_flush(); return __real_ftruncate(fd, l); }
int __wrap_fsync(int fd) { ps5_wb_flush(); return __real_fsync(fd); }
int __wrap_fdatasync(int fd) { ps5_wb_flush(); return __real_fdatasync(fd); }
int __wrap_dup(int fd) { ps5_wb_flush(); return __real_dup(fd); }
long __wrap_readv(int fd, const struct iovec *v, int c) {
    ps5_wb_flush_for(fd); if (FDV_IS(fd)) return fdv_readv(fd, v, c);
    if (!NB(fd)) return __real_readv(fd, v, c);
    struct msghdr m; memset(&m, 0, sizeof m); m.msg_iov = (struct iovec *)v; m.msg_iovlen = c; return recvmsg(fd, &m, MSG_DONTWAIT);
}
long __wrap_writev(int fd, const struct iovec *v, int c) {
    ps5_wb_flush();
    if (!NB(fd)) return __real_writev(fd, v, c);
    struct msghdr m; memset(&m, 0, sizeof m); m.msg_iov = (struct iovec *)v; m.msg_iovlen = c; return sendmsg(fd, &m, MSG_DONTWAIT);
}
long __wrap_recv(int fd, void *b, size_t n, int fl) { return __real_recv(fd, b, n, NB(fd) ? fl | MSG_DONTWAIT : fl); }
long __wrap_recvfrom(int fd, void *b, size_t n, int fl, struct sockaddr *a, socklen_t *al) { return __real_recvfrom(fd, b, n, NB(fd) ? fl | MSG_DONTWAIT : fl, a, al); }
long __wrap_send(int fd, const void *b, size_t n, int fl) { return __real_send(fd, b, n, NB(fd) ? fl | MSG_DONTWAIT : fl); }
extern int __real_setsockopt(int, int, int, const void *, socklen_t);
extern int __real_listen(int, int);
// --- LAN discovery helpers ---------------------------------------------------------------------------------------------------------------------------------------------------------
// Minecraft's "Open to LAN" announces the world with a plain DatagramSocket.send() to 224.0.2.60:4445 and never selects an interface. On this console a multicast datagram without
// IP_MULTICAST_IF can be accepted by sendto() and still go nowhere useful (a PC never lists the PS5's world, while the PS5 does list the PC's, so receiving works). So the first
// multicast sendto() on a socket sets IP_MULTICAST_IF to the console's own address (found by a UDP connect() to a public address, which sends nothing), and an IP_ADD_MEMBERSHIP with
// INADDR_ANY gets the same address; if the explicit variant is refused the original call is made unchanged. LAN_NOIF=1 turns this off.
static unsigned lan_local_ip(void) {      // network byte order, 0 = unknown; cached for 20 s
    static unsigned ip; static double t; double now = now_s();
    if (ip && now - t < 20.0) return ip;
    int s = socket(AF_INET, SOCK_DGRAM, 0); if (s < 0) return ip;
    struct sockaddr_in d; memset(&d, 0, sizeof d); d.sin_family = AF_INET; d.sin_port = htons(53); d.sin_addr.s_addr = htonl(0x01010101);
    if (connect(s, (struct sockaddr *)&d, sizeof d) == 0) { struct sockaddr_in l; socklen_t ll = sizeof l; memset(&l, 0, sizeof l); if (getsockname(s, (struct sockaddr *)&l, &ll) == 0 && l.sin_addr.s_addr) { ip = l.sin_addr.s_addr; t = now; } }
    __real_close(s); return ip;
}
static volatile unsigned char g_mcif[NB_MAX];   // 1 = IP_MULTICAST_IF already set on this fd
static volatile unsigned char g_bc[NB_MAX];   // 1 = SO_BROADCAST already set on this fd
static int lan_nobcast(void) { static int v = -1; if (v < 0) { const char *e = getenv("LAN_NOBCAST"); v = e && *e == '1'; } return v; }
static int lan_noif(void) { static int v = -1; if (v < 0) { const char *e = getenv("LAN_NOIF"); v = e && *e == '1'; } return v; }
// One-off self test when this console starts announcing a LAN world: can it open a TCP connection to the PC that receives the log (listen.ps1 -Lan accepts TCP 9999) and to the router?
// Result lines: "net: lan: selftest ...". Tells "PS5 cannot reach LAN hosts over TCP" from "that PC does not accept the Minecraft port".
static __thread int g_in_sockdiag;
static int conn_probe(const struct in_addr *ip, int port, int ms, int *err);
extern long __real_sendto(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
long __wrap_sendto(int fd, const void *b, size_t n, int fl, const struct sockaddr *a, socklen_t al) {
    int mc = a && a->sa_family == AF_INET && al >= sizeof(struct sockaddr_in) && (ntohl(((const struct sockaddr_in *)a)->sin_addr.s_addr) >> 28) == 0xE;
    if (mc && (unsigned)fd < NB_MAX && !g_mcif[fd] && !lan_noif()) {
        g_mcif[fd] = 1;
        unsigned ip = lan_local_ip(); struct in_addr ia; ia.s_addr = ip;
        int r = ip ? __real_setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ia, sizeof ia) : -1, e = errno;
        unsigned char ttl = 4; int r2 = __real_setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl), e2 = errno;
        static int nl; if (nl++ < 6) { unsigned char *q = (unsigned char *)&ip; TR("net: lan: multicast sender fd %d -> IP_MULTICAST_IF %u.%u.%u.%u = %d (errno %d), TTL 4 = %d (errno %d)", fd, q[0], q[1], q[2], q[3], r, r ? e : 0, r2, r2 ? e2 : 0); }
    }
    if (a && a->sa_family == AF_INET6 && !mc) { static int n6; if (n6++ < 3) TR("net: lan: sendto on an IPv6 destination (fd %d, %ld bytes): the IPv4 multicast fix does not apply to it", fd, (long)n); }
    long r = __real_sendto(fd, b, n, NB(fd) ? fl | MSG_DONTWAIT : fl, a, al), e = errno;
    if (mc && r >= 0 && ntohs(((const struct sockaddr_in *)a)->sin_port) == 4445 && !lan_nobcast()) {   // Minecraft's LAN list socket is bound to the wildcard address, so a broadcast copy reaches it too (multicast alone can be filtered by the router / Wi-Fi AP / PC)
        unsigned ip = lan_local_ip();
        if (ip) {
            int one = 1; if (fd >= 0 && (unsigned)fd < NB_MAX && !g_bc[fd]) { g_bc[fd] = 1; int sr = __real_setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one); static int ns; if (ns++ < 3) TR("net: lan: SO_BROADCAST on fd %d = %d (errno %d)", fd, sr, sr ? errno : 0); }
            struct sockaddr_in d = *(const struct sockaddr_in *)a; unsigned bc[2] = { ip | htonl(0xFF), 0xFFFFFFFFu };
            for (int i = 0; i < 2; i++) { d.sin_addr.s_addr = bc[i]; long br = __real_sendto(fd, b, n, fl, (const struct sockaddr *)&d, sizeof d), be = errno; static int nbl; if (nbl++ < 6) TR("net: lan: broadcast copy to %s:4445 -> %ld (errno %ld)", inet_ntoa(d.sin_addr), br, br < 0 ? be : 0L); }
        }
        errno = e;
    }
    if (mc) { static int c; if (c < 4 || r < 0) { if (c < 40) TR("net: lan: multicast sendto fd %d %ld bytes to %s:%u -> %ld (errno %ld)", fd, (long)n, inet_ntoa(((const struct sockaddr_in *)a)->sin_addr), (unsigned)ntohs(((const struct sockaddr_in *)a)->sin_port), r, r < 0 ? e : 0L); } c++; }
    errno = e; return r;
}
int __wrap_accept(int fd, struct sockaddr *a, socklen_t *al) {
    if (NB(fd)) { DnsPollFd pf; pf.fd = fd; pf.events = 1 /* POLLIN */; pf.revents = 0; if (ps5_dns_poll(&pf, 1, 0) <= 0) { errno = EAGAIN; return -1; } }
    struct sockaddr_in peer; socklen_t pl = sizeof peer; memset(&peer, 0, sizeof peer);
    int r = __real_accept(fd, a ? a : (struct sockaddr *)&peer, a ? al : &pl);
    if (r >= 0) { int e = errno; const struct sockaddr_in *p = (const struct sockaddr_in *)(a ? a : (struct sockaddr *)&peer); static int n; if (n++ < 60) { unsigned char *ip = (unsigned char *)&p->sin_addr; TR("net: accept fd %d -> fd %d from %u.%u.%u.%u:%u", fd, r, ip[0], ip[1], ip[2], ip[3], (unsigned)ntohs(p->sin_port)); } errno = e; }
    return r;
}
// LAN diagnostics: every failing setsockopt, the multicast options and every listen() are logged (first calls only), the result is passed through unchanged.
int __wrap_setsockopt(int fd, int lvl, int opt, const void *v, socklen_t l) {
    int r, e; static int n;
    if (lvl == IPPROTO_IP && opt == IP_ADD_MEMBERSHIP && v && l == sizeof(struct ip_mreq) && !lan_noif() && ((const struct ip_mreq *)v)->imr_interface.s_addr == 0 && lan_local_ip()) {
        struct ip_mreq m = *(const struct ip_mreq *)v; m.imr_interface.s_addr = lan_local_ip();
        r = __real_setsockopt(fd, lvl, opt, &m, sizeof m); e = errno;
        if (!g_in_sockdiag && n < 40) { g_in_sockdiag = 1; n++; unsigned char *q = (unsigned char *)&m.imr_interface; TR("net: lan: join %s on %u.%u.%u.%u (fd %d) -> %d errno %d", inet_ntoa(m.imr_multiaddr), q[0], q[1], q[2], q[3], fd, r, r ? e : 0); g_in_sockdiag = 0; }
        if (r == 0) { errno = 0; return 0; }
        r = __real_setsockopt(fd, lvl, opt, v, l); e = errno;                // explicit interface refused: the original request
    } else r = __real_setsockopt(fd, lvl, opt, v, l), e = errno;
    if (!g_in_sockdiag && (r != 0 || (lvl == 0 && opt >= 9 && opt <= 13)) && n < 40) { g_in_sockdiag = 1; n++; TR("net: setsockopt fd %d level %d opt %d len %d -> %d errno %d", fd, lvl, opt, (int)l, r, r ? e : 0); g_in_sockdiag = 0; }
    errno = e; return r;
}
// connect(): this console's connect() always blocks (SOCK_NONBLOCK / O_NONBLOCK are ignored or refused) and has no timeout, so a LAN host that does not answer would hang Minecraft's "Connecting..." forever.
// For TCP connects to private IPv4 addresses the real connect() therefore runs on a helper thread and the caller waits at most LAN_CONNECT_MS (default 8000): then shutdown(fd) aborts the attempt and
// the caller gets ETIMEDOUT ("Connection timed out" in Minecraft). On timeout the same host is probed on ports 445 and 135 to tell "this host is unreachable" from "only that port is dropped" (log line "net: connect probe").
// Everything else (public addresses, loopback, UDP) goes straight through. Every private connect is logged before and after the call.
#include <sched.h>
extern int __real_connect(int, const struct sockaddr *, socklen_t);
typedef struct { int fd; struct sockaddr_in sa; int r, e; volatile int state; } ConnJob;   // state: 0 running, 1 done, 2 abandoned by the waiter (the helper frees the job)
static void *conn_thread(void *p) {
    ConnJob *j = (ConnJob *)p;
    j->r = __real_connect(j->fd, (struct sockaddr *)&j->sa, sizeof j->sa); j->e = errno;
    if (__atomic_exchange_n(&j->state, 1, __ATOMIC_ACQ_REL) == 2) free(j);
    return 0;
}
static int conn_timed(int fd, const struct sockaddr_in *sa, int ms, int *err, int *timed_out) {
    *timed_out = 0;
    ConnJob *j = (ConnJob *)calloc(1, sizeof *j);
    if (!j) { int r = __real_connect(fd, (const struct sockaddr *)sa, sizeof *sa); *err = errno; return r; }
    j->fd = fd; j->sa = *sa;
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 256 * 1024); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t th; int pc = pthread_create(&th, &at, conn_thread, j); pthread_attr_destroy(&at);
    if (pc) { free(j); int r = __real_connect(fd, (const struct sockaddr *)sa, sizeof *sa); *err = errno; return r; }
    double t0 = now_s();
    while (!__atomic_load_n(&j->state, __ATOMIC_ACQUIRE)) {
        if ((now_s() - t0) * 1000 > ms) {
            *timed_out = 1; shutdown(fd, SHUT_RDWR);                                  // wakes the blocked connect on most BSD stacks
            for (int i = 0; i < 300 && !__atomic_load_n(&j->state, __ATOMIC_ACQUIRE); i++) { struct timespec ts = {0, 1000000}; nanosleep(&ts, 0); }
            break;
        }
        struct timespec ts = {0, 500000}; nanosleep(&ts, 0);
    }
    int r, e;
    if (__atomic_exchange_n(&j->state, 2, __ATOMIC_ACQ_REL) == 1) { r = j->r; e = j->e; free(j); }   // finished: take the result
    else { r = -1; e = ETIMEDOUT; }                                                                  // still blocked: the helper frees the job when (if) it returns
    if (*timed_out) { r = -1; e = ETIMEDOUT; }
    *err = e; return r;
}
static int conn_probe(const struct in_addr *ip, int port, int ms, int *err) {
    int s = socket(AF_INET, SOCK_STREAM, 0); if (s < 0) { *err = errno; return -1; }
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa); sa.sin_family = AF_INET; sa.sin_port = htons(port); sa.sin_addr = *ip;
    int to; int r = conn_timed(s, &sa, ms, err, &to); __real_close(s); return r;
}
extern int __wrap_bind(int, const struct sockaddr *, socklen_t);
int __wrap_connect(int fd, const struct sockaddr *a, socklen_t al) {
    if (!(a && a->sa_family == AF_INET && al >= sizeof(struct sockaddr_in)) || g_in_sockdiag) return __real_connect(fd, a, al);
    const struct sockaddr_in *s4 = (const struct sockaddr_in *)a; unsigned ip = ntohl(s4->sin_addr.s_addr);
    int priv = (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 || (ip >> 16) == 0xA9FE;
    int type = 0; socklen_t tl = sizeof type;
    int tcp = priv && getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl) == 0 && type == SOCK_STREAM;
    static int ms = -1; if (ms < 0) { const char *e = getenv("LAN_CONNECT_MS"); ms = e && atoi(e) > 0 ? atoi(e) : 8000; }
    static int nb, na;
    if (priv && nb++ < 40) { g_in_sockdiag = 1; TR("net: connect fd %d (%s%s) to %s:%u ...", fd, NB(fd) ? "emulated non-blocking" : "blocking", tcp ? ", timed" : "", inet_ntoa(s4->sin_addr), (unsigned)ntohs(s4->sin_port)); g_in_sockdiag = 0; }
    if (tcp) {   // an unbound socket gets its source port from this console's own range (20000-29999, see __wrap_bind) instead of the kernel's 49152+: LAN_NOPREBIND=1 turns it off
        static int np = -1; if (np < 0) { const char *x = getenv("LAN_NOPREBIND"); np = x && *x == '1'; }
        struct sockaddr_in me; socklen_t ml = sizeof me; memset(&me, 0, sizeof me);
        if (!np && getsockname(fd, (struct sockaddr *)&me, &ml) == 0 && me.sin_port == 0) {
            struct sockaddr_in b; memset(&b, 0, sizeof b); b.sin_family = AF_INET;
            int br = __wrap_bind(fd, (struct sockaddr *)&b, sizeof b), be = errno;
            static int nbp; if (nbp++ < 4) { g_in_sockdiag = 1; ml = sizeof me; getsockname(fd, (struct sockaddr *)&me, &ml); TR("net: connect fd %d: source port pre-bound -> %d errno %d, port %u", fd, br, br ? be : 0, (unsigned)ntohs(me.sin_port)); g_in_sockdiag = 0; }
        }
    }
    double t0 = now_s(); int to = 0, e = 0, r;
    if (tcp) r = conn_timed(fd, s4, ms, &e, &to); else { r = __real_connect(fd, a, al); e = errno; }
    double dt = (now_s() - t0) * 1000;
    if ((priv || r != 0 || dt > 2000) && ip >> 24 != 127 && na++ < 40) { g_in_sockdiag = 1; TR("net: connect fd %d to %s:%u -> %d errno %d in %.0f ms%s", fd, inet_ntoa(s4->sin_addr), (unsigned)ntohs(s4->sin_port), r, r ? e : 0, dt, to ? " TIMED OUT" : dt > 2000 ? " SLOW" : ""); g_in_sockdiag = 0; }
    if (to) {
        static int np; if (np++ < 3) {
            g_in_sockdiag = 1;
            static const int ports[2] = { 445, 135 };
            for (int i = 0; i < 2; i++) { int pe = 0; double p0 = now_s(); int pr = conn_probe(&s4->sin_addr, ports[i], 2500, &pe); TR("net: connect probe %s:%d -> %d errno %d in %.0f ms (0 or ECONNREFUSED(61) = the host answers; ETIMEDOUT(60) = nothing comes back)", inet_ntoa(s4->sin_addr), ports[i], pr, pr ? pe : 0, (now_s() - p0) * 1000); }
            unsigned lip = lan_local_ip();      // the router (x.x.x.1 of our /24): any answer, even a refusal, proves that TCP across the LAN works from this console
            if (lip) { struct in_addr gw; gw.s_addr = (lip & htonl(0xFFFFFF00u)) | htonl(1);
                static const int gp[3] = { 80, 443, 53 };
                for (int i = 0; i < 3; i++) { int pe = 0; double p0 = now_s(); int pr = conn_probe(&gw, gp[i], 1500, &pe); TR("net: connect probe router %s:%d -> %d errno %d in %.0f ms", inet_ntoa(gw), gp[i], pr, pr ? pe : 0, (now_s() - p0) * 1000); } }
            g_in_sockdiag = 0;
        }
    }
    errno = e; return r;
}
int __wrap_listen(int fd, int bl) {
    int r = __real_listen(fd, bl), e = errno; static int n;
    if (!g_in_sockdiag && n++ < 60) { g_in_sockdiag = 1; struct sockaddr_in sa; socklen_t sl = sizeof sa; memset(&sa, 0, sizeof sa); getsockname(fd, (struct sockaddr *)&sa, &sl); TR("net: listen fd %d backlog %d on port %u -> %d errno %d", fd, bl, (unsigned)ntohs(sa.sin_port), r, r ? e : 0); g_in_sockdiag = 0; }
    errno = e; return r;
}
int __wrap_close(int fd) { ps5_wb_flush(); if ((unsigned)fd < NB_MAX) g_reg[fd] = 0; if (FDV_IS(fd)) return fdv_close(fd); fdv_forget_real(fd); if ((unsigned)fd < NB_MAX) { g_nb[fd] = 0; g_mcif[fd] = 0; g_bc[fd] = 0; } return __real_close(fd); }

// ---------------- bind(): ephemeral ports the console will accept ----------------
// bind() to an explicit port only works below some threshold on this console (probed: 1025, 3000, 8080, 20000, 25565 ok; 40000, 49152, 55555, 65000 and every kernel-chosen ephemeral port, all errno 13),
// while bind(port 0) works and hands out ports in the denied range. Minecraft's "Open to LAN" does ServerSocket(0) -> port P (kernel-chosen, e.g. 62024), closes it, then binds P explicitly -> BindException
// "Permission denied" -> "Unable to host local game". So for TCP sockets a bind to port 0 picks a random port inside [BIND_LO, BIND_HI) itself (retrying on EADDRINUSE/EACCES), and the number the program reads
// back is one it can bind again, and announce on the LAN. Datagram sockets and explicit ports pass straight through; if every attempt fails the kernel's own choice is used.
#ifndef BIND_LO
#define BIND_LO 20000
#endif
#ifndef BIND_HI
#define BIND_HI 30000
#endif
extern int __real_bind(int, const struct sockaddr *, socklen_t);
int __wrap_bind(int fd, const struct sockaddr *a, socklen_t len) {
    if (a && (a->sa_family == AF_INET || a->sa_family == AF_INET6) && len >= 4 && len <= 128) {
        unsigned char buf[128]; memcpy(buf, a, len);
        unsigned short port; memcpy(&port, buf + 2, 2);                       // sin_port / sin6_port: same offset in both
        int type = 0; socklen_t tl = sizeof type;
        if (port == 0 && getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl) == 0 && type == SOCK_STREAM) {
            static unsigned long long rng; if (!rng) rng = (unsigned long long)(now_s() * 1e6) | 1;
            for (int i = 0; i < 256; i++) {
                rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
                unsigned short p = (unsigned short)(BIND_LO + rng % (BIND_HI - BIND_LO)), np = htons(p);
                memcpy(buf + 2, &np, 2);
                if (__real_bind(fd, (const struct sockaddr *)buf, len) == 0) { static int n; if (n++ < 6) TR("net: bind: TCP socket fd %d, port 0 -> %u (tries %d)", fd, p, i + 1); return 0; }
                if (errno != EADDRINUSE && errno != EACCES) return -1;
            }
            TR("net: bind: no free port in %d-%d for fd %d, using the kernel's choice", BIND_LO, BIND_HI, fd);
        }
    }
    return __real_bind(fd, a, len);
}

#ifdef PS5_ZINK
// ---------------- native Zink probe (no JVM): surfaceless EGL on Mesa's Zink over RADV, a context, GL_VERSION, one clear into a renderbuffer and a read-back ----------------
typedef const unsigned char *(*ZGetStr)(unsigned);
typedef void (*ZGen)(int, unsigned *); typedef void (*ZBind)(unsigned, unsigned); typedef void (*ZRbS)(unsigned, unsigned, int, int); typedef void (*ZFbR)(unsigned, unsigned, unsigned, unsigned);
typedef unsigned (*ZChk)(unsigned); typedef void (*ZCC)(float, float, float, float); typedef void (*ZClr)(unsigned); typedef void (*ZRd)(int, int, int, int, unsigned, unsigned, void *); typedef unsigned (*ZErr)(void); typedef void (*ZFin)(void);
void ps5_zink_probe(void) {
    TR("zprobe: begin (Zink over RADV, surfaceless EGL)");
    setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1); setenv("GALLIUM_DRIVER", "zink", 1); setenv("MESA_GL_VERSION_OVERRIDE", "4.6", 1); setenv("MESA_GLSL_VERSION_OVERRIDE", "460", 1);
    setenv("EGL_LOG_LEVEL", "debug", 1); setenv("LIBGL_DEBUG", "verbose", 1); setenv("MESA_DEBUG", "1", 1);
    void *dpy = zink_eglGetPlatformDisplay(0x31DD /* EGL_PLATFORM_SURFACELESS_MESA */, 0, 0);
    TR("zprobe: eglGetPlatformDisplay(surfaceless) = %p, error 0x%x", dpy, zink_eglGetError());
    if (!dpy) return;
    int maj = 0, min = 0; unsigned ok = zink_eglInitialize(dpy, &maj, &min);
    TR("zprobe: eglInitialize = %u (EGL %d.%d), error 0x%x", ok, maj, min, zink_eglGetError());
    if (!ok) return;
    const char *v;
    v = zink_eglQueryString(dpy, 0x3053); TR("zprobe: EGL vendor %s", v ? v : "?");
    v = zink_eglQueryString(dpy, 0x3054); TR("zprobe: EGL version %s", v ? v : "?");
    v = zink_eglQueryString(dpy, 0x3055); TR("zprobe: EGL extensions %.150s", v ? v : "?");
    TR("zprobe: eglBindAPI(OpenGL) = %u", zink_eglBindAPI(0x30A2));
    void *cfg = 0; int ncfg = 0; const int ca[] = { 0x3033, 0x4 /* EGL_WINDOW_BIT */, 0x3040, 0x8 /* EGL_OPENGL_BIT */, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3025, 24, 0x3026, 8, 0x3038 };   // the context and the window must share one config: RGBA8888 + D24S8
    unsigned cr = zink_eglChooseConfig(dpy, ca, &cfg, 1, &ncfg); TR("zprobe: eglChooseConfig = %u, %d configs, config %p, error 0x%x", cr, ncfg, cfg, zink_eglGetError());
    void *ctx = 0;
    static const int vers[][2] = { { 4, 6 }, { 4, 5 }, { 4, 3 }, { 3, 3 } };
    for (int i = 0; i < 4 && !ctx; i++) {
        const int ctxa[] = { 0x3098, vers[i][0], 0x30FB, vers[i][1], 0x30FD, 0x1 /* core */, 0x3038 };
        ctx = zink_eglCreateContext(dpy, ncfg > 0 ? cfg : 0, 0, ctxa);
        TR("zprobe: eglCreateContext(%d.%d core, %s) = %p, error 0x%x", vers[i][0], vers[i][1], ncfg > 0 ? "config" : "no config", ctx, zink_eglGetError());
    }
    if (!ctx) return;
    unsigned mk = zink_eglMakeCurrent(dpy, 0, 0, ctx); TR("zprobe: eglMakeCurrent(surfaceless) = %u, error 0x%x", mk, zink_eglGetError());
    if (!mk) return;
    ZGetStr gs = (ZGetStr)zink_eglGetProcAddress("glGetString");
    if (!gs) { TR("zprobe: glGetString missing"); return; }
    TR("zprobe: GL_VENDOR %s", (const char *)gs(0x1F00)); TR("zprobe: GL_RENDERER %s", (const char *)gs(0x1F01)); TR("zprobe: GL_VERSION %s", (const char *)gs(0x1F02));
    ZGen gr = (ZGen)zink_eglGetProcAddress("glGenRenderbuffers"), gf = (ZGen)zink_eglGetProcAddress("glGenFramebuffers");
    ZBind br = (ZBind)zink_eglGetProcAddress("glBindRenderbuffer"), bf = (ZBind)zink_eglGetProcAddress("glBindFramebuffer");
    ZRbS rs = (ZRbS)zink_eglGetProcAddress("glRenderbufferStorage"); ZFbR fr = (ZFbR)zink_eglGetProcAddress("glFramebufferRenderbuffer"); ZChk cs = (ZChk)zink_eglGetProcAddress("glCheckFramebufferStatus");
    ZCC cc = (ZCC)zink_eglGetProcAddress("glClearColor"); ZClr cl = (ZClr)zink_eglGetProcAddress("glClear"); ZRd rd = (ZRd)zink_eglGetProcAddress("glReadPixels"); ZErr ge = (ZErr)zink_eglGetProcAddress("glGetError"); ZFin fin = (ZFin)zink_eglGetProcAddress("glFinish");
    if (!gr || !gf || !br || !bf || !rs || !fr || !cs || !cc || !cl || !rd || !ge || !fin) { TR("zprobe: a GL entry point is missing (%p %p %p %p %p %p %p %p %p %p %p %p)", gr, gf, br, bf, rs, fr, cs, cc, cl, rd, ge, fin); return; }
    unsigned rb = 0, fb = 0; gr(1, &rb); gf(1, &fb); br(0x8D41, rb); rs(0x8D41, 0x8058 /* RGBA8 */, 256, 256); bf(0x8D40, fb); fr(0x8D40, 0x8CE0, 0x8D41, rb);
    TR("zprobe: framebuffer status 0x%x, GL error 0x%x", cs(0x8D40), ge());
    for (int f = 0; f < 3; f++) {
        float r = f == 0 ? 1.f : 0.f, g = f == 1 ? 1.f : 0.f, b = f == 2 ? 1.f : 0.f;
        cc(r, g, b, 1.f); cl(0x4000); unsigned char px[4] = { 0, 0, 0, 0 }; rd(10, 10, 1, 1, 0x1908, 0x1401, px); fin();
        TR("zprobe: clear %.0f,%.0f,%.0f -> read back %u,%u,%u,%u, GL error 0x%x", r, g, b, px[0], px[1], px[2], px[3], ge());
    }
    // on-screen: a window surface on the console display (Zink presents through VK_KHR_display), a colour cycle with frame timing
    bf(0x8D40, 0); fin();
    ps5vk_create_window(1920, 1080);
    void *wcfg = 0; int nw = 0; const int wa[] = { 0x3033, 0x4 /* EGL_WINDOW_BIT */, 0x3040, 0x8, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3025, 24, 0x3026, 8, 0x3038 };   // RGBA8888 + D24S8 like Minecraft asks: the first config was RGB565 and the display scans out 8888
    unsigned wr = zink_eglChooseConfig(dpy, wa, &wcfg, 1, &nw); TR("zprobe: window eglChooseConfig = %u, %d configs, config %p, error 0x%x", wr, nw, wcfg, zink_eglGetError());
    if (nw < 1) {
        void *all[512]; int na = 0; zink_eglGetConfigs(dpy, all, 512, &na); int nwin = 0;
        for (int i = 0; i < na; i++) { int st = 0, rs = 0, gs = 0, bs = 0, as = 0, ds = 0, ss = 0, id = 0; zink_eglGetConfigAttrib(dpy, all[i], 0x3033, &st); zink_eglGetConfigAttrib(dpy, all[i], 0x3024, &rs); zink_eglGetConfigAttrib(dpy, all[i], 0x3023, &gs); zink_eglGetConfigAttrib(dpy, all[i], 0x3022, &bs); zink_eglGetConfigAttrib(dpy, all[i], 0x3021, &as); zink_eglGetConfigAttrib(dpy, all[i], 0x3025, &ds); zink_eglGetConfigAttrib(dpy, all[i], 0x3026, &ss); zink_eglGetConfigAttrib(dpy, all[i], 0x3028, &id);
            if (st & 4) nwin++; if (i < 3 || (st & 4)) TR("zprobe: config %d id %d surface 0x%x rgba %d%d%d%d depth %d stencil %d", i, id, st, rs, gs, bs, as, ds, ss); }
        TR("zprobe: %d configs in all, %d with the window bit", na, nwin);
        if (!nwin) { TR("zprobe: no window-capable config"); return; }
        wcfg = all[0]; for (int i = 0; i < na; i++) { int st = 0; zink_eglGetConfigAttrib(dpy, all[i], 0x3033, &st); if (st & 4) { wcfg = all[i]; break; } }
    }
    static int wh[2] = { 1920, 1080 };
    void *surf = zink_eglCreateWindowSurface(dpy, wcfg, wh, 0); TR("zprobe: eglCreateWindowSurface = %p, error 0x%x", surf, zink_eglGetError());
    if (!surf) return;
    mk = zink_eglMakeCurrent(dpy, surf, surf, ctx); TR("zprobe: eglMakeCurrent(window) = %u, error 0x%x", mk, zink_eglGetError());
    if (!mk) return;
    TR("zprobe: eglSwapInterval(0) = %u", zink_eglSwapInterval(dpy, 0));
    bf(0x8D40, 0);
    unsigned long t0 = 0, tl = 0; struct timespec ts; clock_gettime(1, &ts); t0 = tl = ts.tv_sec * 1000UL + ts.tv_nsec / 1000000UL;
    {   // ~12 s of wall-clock time, a glFinish + fps line every second (swaps return before the GPU is done, so only a finished-work count is honest)
        int f = 0, fr = 0; unsigned long tend = t0 + 12000;
        for (;;) {
            float ph = f * 0.05f; float r = 0.5f + 0.5f * __builtin_sinf(ph), g = 0.5f + 0.5f * __builtin_sinf(ph + 2.1f), b = 0.5f + 0.5f * __builtin_sinf(ph + 4.2f);
            cc(r, g, b, 1.f); cl(0x4000); unsigned se = zink_eglSwapBuffers(dpy, surf); f++; fr++;
            if (f <= 3 || !se) TR("zprobe: frame %d swap = %u, GL error 0x%x, EGL error 0x%x", f, se, ge(), zink_eglGetError());
            clock_gettime(1, &ts); unsigned long n = ts.tv_sec * 1000UL + ts.tv_nsec / 1000000UL;
            if (n - tl >= 1000) { fin(); clock_gettime(1, &ts); n = ts.tv_sec * 1000UL + ts.tv_nsec / 1000000UL; TR("zprobe: %d frames in %lu ms = %.1f fps (after glFinish)", fr, n - tl, fr * 1000.0 / (double)(n - tl ? n - tl : 1)); tl = n; fr = 0; }
            if (n >= tend) break;
        }
    }
    TR("zprobe: window loop done");
}
#endif
