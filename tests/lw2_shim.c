// lw2_shim.c - test double for what ps5lwjgl.c gives ps5lwjgl2.c on the console: a GLFW subset on a headless EGL core context (Mesa) with gl4es in front of it,
// the software OpenAL (the real ps5al.inc, audio output stubbed), and a few test-only JNI functions to inject input. Linked with the real LWJGL 2 objects
// into liblwjgl2ps5.so by tests/lw2_test.sh.
#define _GNU_SOURCE
#include <jni.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

void ps5_trace(const char *s) { if (getenv("LW2_VERBOSE")) printf("   [trace] %s\n", s); }
static void TR(const char *fmt, ...) { char b[300]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a); ps5_trace(b); }
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
int sceAudioOutInit(void) { return 0; }
int sceAudioOutOpen(int u, int t, int i, unsigned l, unsigned f, unsigned p) { (void)u; (void)t; (void)i; (void)l; (void)f; (void)p; return 1; }
int sceAudioOutOutput(int h, const void *p) { (void)h; (void)p; usleep(10000); return 0; }
#include "../ps5al.inc"
void *ps5_pal_lookup(const char *n) { return pal_lookup(n); }
void ps5_l2_log(const char *m) { ps5_trace(m); }

extern void ps5g4_configure(void *(*)(const char *), int);
extern void *ps5g4_lookup(const char *);
extern void ps5g4_pre_swap(void);
extern jint JNI_OnLoad_lwjgl2ps5(JavaVM *vm, void *reserved);
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) { return JNI_OnLoad_lwjgl2ps5(vm, reserved); }   // System.loadLibrary on the host looks for the plain name

typedef struct { int width, height, redBits, greenBits, blueBits, refreshRate; } VidMode;
static EGLDisplay g_d; static EGLSurface g_s; static EGLContext g_x;
static int g_win_dummy, g_mode = 0x34001, g_swaps, g_ww, g_wh; static double g_cx, g_cy;
typedef void (*KeyCb)(void *, int, int, int, int); typedef void (*CharCb)(void *, unsigned); typedef void (*BtnCb)(void *, int, int, int);
typedef void (*PosCb)(void *, double, double); typedef void (*FocusCb)(void *, int);
static KeyCb c_key; static CharCb c_char; static BtnCb c_btn; static PosCb c_pos, c_scroll; static FocusCb c_focus;

int glfwInit(void) { return 1; }
void *glfwCreateWindow(int w, int h, const char *title, void *m, void *sh) {
    (void)title; (void)m; (void)sh;
    g_d = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    EGLint maj, min; if (!g_d || !eglInitialize(g_d, &maj, &min)) return 0;
    eglBindAPI(EGL_OPENGL_API);
    EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE }; EGLConfig c; EGLint n = 0;
    eglChooseConfig(g_d, ca, &c, 1, &n); if (!n) return 0;
    EGLint xa[] = { EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5, EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
    g_x = eglCreateContext(g_d, c, EGL_NO_CONTEXT, xa); EGLint pa[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
    g_s = eglCreatePbufferSurface(g_d, c, pa); g_ww = w; g_wh = h;
    if (!g_x || !g_s) return 0;
    return &g_win_dummy;
}
void glfwMakeContextCurrent(void *w) { (void)w; if (!eglMakeCurrent(g_d, g_s, g_s, g_x)) return; static int once; if (!once) { once = 1; ps5g4_configure((void *(*)(const char *))eglGetProcAddress, 21); } }
void glfwSwapBuffers(void *w) { (void)w; ps5g4_pre_swap(); g_swaps++; }
void glfwSwapInterval(int i) { (void)i; }
void ps5_gl_release(void) {}
void glfwPollEvents(void) { }
void *glfwGetProcAddress(const char *n) { return ps5g4_lookup(n); }
void *glfwGetPrimaryMonitor(void) { return &g_win_dummy; }
static const VidMode g_vm = { 1920, 1080, 8, 8, 8, 60 };
const VidMode *glfwGetVideoMode(void *m) { (void)m; return &g_vm; }
void glfwGetWindowSize(void *w, int *a, int *b) { (void)w; *a = g_ww; *b = g_wh; }
void glfwSetInputMode(void *w, int mode, int v) { (void)w; if (mode == 0x33001) g_mode = v; }
void glfwSetCursorPos(void *w, double x, double y) { (void)w; g_cx = x; g_cy = y; }
void glfwGetCursorPos(void *w, double *x, double *y) { (void)w; *x = g_cx; *y = g_cy; }
int glfwWindowShouldClose(void *w) { (void)w; return 0; }
void glfwSetWindowTitle(void *w, const char *t) { (void)w; (void)t; }
void *glfwSetKeyCallback(void *w, void *f) { (void)w; c_key = (KeyCb)f; return 0; }
void *glfwSetCharCallback(void *w, void *f) { (void)w; c_char = (CharCb)f; return 0; }
void *glfwSetMouseButtonCallback(void *w, void *f) { (void)w; c_btn = (BtnCb)f; return 0; }
void *glfwSetCursorPosCallback(void *w, void *f) { (void)w; c_pos = (PosCb)f; return 0; }
void *glfwSetScrollCallback(void *w, void *f) { (void)w; c_scroll = (PosCb)f; return 0; }
void *glfwSetWindowFocusCallback(void *w, void *f) { (void)w; c_focus = (FocusCb)f; return 0; }

JNIEXPORT void JNICALL Java_org_lwjgl_Lw2Inject_inject(JNIEnv *e, jclass c, jint kind, jint a, jint b, jint x) {
    (void)e; (void)c; void *w = &g_win_dummy;
    switch (kind) {
    case 1: if (c_key) c_key(w, a, 0, b, x); break;
    case 2: if (c_char) c_char(w, (unsigned)a); break;
    case 3: if (c_btn) c_btn(w, a, b, x); break;
    case 4: g_cx = a; g_cy = b; if (c_pos) c_pos(w, a, b); break;
    case 5: if (c_scroll) c_scroll(w, a / 1000.0, b / 1000.0); break;
    case 6: if (c_focus) c_focus(w, a); break;
    }
}
JNIEXPORT jint JNICALL Java_org_lwjgl_Lw2Inject_cursorMode(JNIEnv *e, jclass c) { (void)e; (void)c; return g_mode; }
JNIEXPORT jint JNICALL Java_org_lwjgl_Lw2Inject_swaps(JNIEnv *e, jclass c) { (void)e; (void)c; return g_swaps; }
JNIEXPORT jstring JNICALL Java_org_lwjgl_Lw2Inject_glString(JNIEnv *e, jclass c, jint n) {
    (void)c; const unsigned char *(*f)(unsigned) = (const unsigned char *(*)(unsigned))ps5g4_lookup("glGetString"); return (*e)->NewStringUTF(e, f ? (const char *)f((unsigned)n) : "?");
}
