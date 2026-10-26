// ps5lwjgl_vk.c: LWJGL3 shim for the Vulkan title (make_vk_title.sh). Same job as ps5lwjgl.c, without EGL / OpenGL:
//   * dlopen/dlsym for LWJGL's "glfw" and "vulkan" libraries (the natives are linked into the title; the console cannot dlopen anything built)
//   * a GLFW subset: one fixed window, no GL context. glfwVulkanSupported / glfwGetRequiredInstanceExtensions / glfwCreateWindowSurface go to the
//     statically linked RADV through VK_KHR_display (the same display path vkprobe.c proved: 1920x1080, the title never opens VideoOut itself)
//   * every GLFW name that is not implemented here is a stub that returns 0 (LWJGL resolves ALL of them when GLFW loads)
//   * what LWJGL core needs from the platform: JNI_OnLoad_lwjgl, RWX memory for libffi closures
// Input (pad -> keys/mouse, on-screen keyboard) is NOT in here yet: it lives in ps5lwjgl.c and is tied to the GL overlay; it is the next step.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <jni.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>

extern void ps5_trace(const char *);
static void TR(const char *fmt, ...) { char b[240]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a); ps5_trace(b); }
extern PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance, const char *);
extern int sceSystemServiceHideSplashScreen(void) __attribute__((weak));
extern int sceKernelLoadStartModule(const char *name, size_t argc, const void *argv, unsigned flags, const void *opt, int *res);
extern int sceKernelDlsym(int handle, const char *symbol, void **addrp);

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static long stub_zero(void) { return 0; }

// ---------------- Vulkan entry points for LWJGL ----------------
static PFN_vkVoidFunction vk_gipa(VkInstance i, const char *n) { return vk_icdGetInstanceProcAddr(i, n); }   // what "libvulkan.so.1" would export

// ---------------- GLFW subset ----------------
typedef struct { int width, height, redBits, greenBits, blueBits, refreshRate; } VidMode;
static int g_win_tag, g_mon_tag;
#define WIN ((void *)&g_win_tag)
#define MON ((void *)&g_mon_tag)
static int g_w = 1920, g_h = 1080, g_inited, g_created, g_should_close;
static double g_t0;
static VidMode g_vm = { 1920, 1080, 8, 8, 8, 60 };

int glfwInit(void) {
    if (g_inited) return 1;
    g_inited = 1; g_t0 = now_s();
    TR("vkglfw: init");
    return 1;
}
void glfwTerminate(void) {}
void glfwInitHint(int h, int v) { (void)h; (void)v; }
void glfwGetVersion(int *a, int *b, int *c) { if (a) *a = 3; if (b) *b = 3; if (c) *c = 8; }
const char *glfwGetVersionString(void) { return "3.3.8 PS5 (Vulkan/VK_KHR_display)"; }
int glfwGetError(const char **d) { if (d) *d = 0; return 0; }
void glfwWindowHint(int h, int v) { static int n; if (n < 12) { n++; TR("vkglfw: windowHint 0x%x = %d", h, v); } }
void glfwDefaultWindowHints(void) {}
void *glfwCreateWindow(int w, int h, const char *title, void *mon, void *share) {
    (void)title; (void)mon; (void)share;
    if (!g_created) {
        g_created = 1;
        if (sceSystemServiceHideSplashScreen) sceSystemServiceHideSplashScreen();
        TR("vkglfw: createWindow %dx%d requested, the window is the display (%dx%d)", w, h, g_w, g_h);
    }
    return WIN;
}
void glfwDestroyWindow(void *w) { (void)w; }
int glfwWindowShouldClose(void *w) { (void)w; return g_should_close; }
void glfwSetWindowShouldClose(void *w, int v) { (void)w; g_should_close = v; }
void glfwPollEvents(void) {}
void glfwWaitEvents(void) { usleep(2000); }
void glfwWaitEventsTimeout(double t) { if (t > 0) { if (t > 0.05) t = 0.05; usleep((unsigned)(t * 1e6)); } }
void glfwPostEmptyEvent(void) {}
void glfwSwapInterval(int i) { (void)i; }
void glfwGetFramebufferSize(void *w, int *x, int *y) { (void)w; if (x) *x = g_w; if (y) *y = g_h; }
void glfwGetWindowSize(void *w, int *x, int *y) { (void)w; if (x) *x = g_w; if (y) *y = g_h; }
void *glfwGetPrimaryMonitor(void) { return MON; }
const VidMode *glfwGetVideoMode(void *m) { (void)m; return &g_vm; }
double glfwGetTime(void) { return now_s() - g_t0; }
int glfwGetWindowAttrib(void *w, int a) { (void)w; return a == 0x20001 || a == 0x20004 ? 1 : 0; }   // FOCUSED, VISIBLE; CLIENT_API = GLFW_NO_API (0)
int glfwExtensionSupported(const char *e) { (void)e; return 0; }
int glfwGetInputMode(void *w, int m) { (void)w; (void)m; return 0; }
int glfwRawMouseMotionSupported(void) { return 0; }

// ---- Vulkan side of GLFW ----
int glfwVulkanSupported(void) { return 1; }
static const char *const g_req_exts[] = { "VK_KHR_surface", "VK_KHR_display" };
const char **glfwGetRequiredInstanceExtensions(uint32_t *count) { if (count) *count = 2; return (const char **)g_req_exts; }
void *glfwGetInstanceProcAddress(VkInstance i, const char *n) { return (void *)vk_icdGetInstanceProcAddr(i, n); }
int glfwGetPhysicalDevicePresentationSupport(VkInstance i, VkPhysicalDevice p, uint32_t q) { (void)i; (void)p; return q == 0; }   // queue family 0 is the graphics+compute+transfer family
VkResult glfwCreateWindowSurface(VkInstance inst, void *w, const VkAllocationCallbacks *alloc, VkSurfaceKHR *out) {
    (void)w; (void)alloc;
#define I(f) PFN_##f f = (PFN_##f)vk_icdGetInstanceProcAddr(inst, #f); if (!f) { TR("vkglfw: %s missing", #f); return VK_ERROR_EXTENSION_NOT_PRESENT; }
    I(vkEnumeratePhysicalDevices) I(vkGetPhysicalDeviceDisplayPropertiesKHR) I(vkGetPhysicalDeviceDisplayPlanePropertiesKHR) I(vkGetDisplayModePropertiesKHR) I(vkCreateDisplayPlaneSurfaceKHR)
#undef I
    uint32_t np = 1; VkPhysicalDevice pd;
    VkResult r = vkEnumeratePhysicalDevices(inst, &np, &pd); if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !np) { TR("vkglfw: no physical device (%d)", (int)r); return VK_ERROR_INITIALIZATION_FAILED; }
    uint32_t nd = 1; VkDisplayPropertiesKHR dp;
    r = vkGetPhysicalDeviceDisplayPropertiesKHR(pd, &nd, &dp); if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !nd) { TR("vkglfw: no display (%d)", (int)r); return VK_ERROR_INITIALIZATION_FAILED; }
    uint32_t nm = 0; vkGetDisplayModePropertiesKHR(pd, dp.display, &nm, 0);
    VkDisplayModePropertiesKHR *modes = malloc(sizeof *modes * (nm + 1)); vkGetDisplayModePropertiesKHR(pd, dp.display, &nm, modes);
    int pick = -1;
    for (uint32_t i = 0; i < nm; i++) {
        TR("vkglfw: display mode %u %ux%u @ %.2f Hz", i, modes[i].parameters.visibleRegion.width, modes[i].parameters.visibleRegion.height, modes[i].parameters.refreshRate / 1000.0);
        if (pick < 0 && (int)modes[i].parameters.visibleRegion.width == 1920) pick = (int)i;   // the first 1080p mode (fastest refresh at that size; the console runs 120 Hz when it allows it whatever the table says)
    }
    if (pick < 0) pick = 0;
    VkExtent2D ext = modes[pick].parameters.visibleRegion; g_w = (int)ext.width; g_h = (int)ext.height; g_vm.width = g_w; g_vm.height = g_h;
    uint32_t npl = 0; vkGetPhysicalDeviceDisplayPlanePropertiesKHR(pd, &npl, 0);
    VkDisplayPlanePropertiesKHR pl[8]; if (npl > 8) npl = 8; vkGetPhysicalDeviceDisplayPlanePropertiesKHR(pd, &npl, pl);
    VkDisplaySurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR };
    sci.displayMode = modes[pick].displayMode; sci.planeIndex = 0; sci.planeStackIndex = npl ? pl[0].currentStackIndex : 0;
    sci.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR; sci.globalAlpha = 1.0f; sci.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR; sci.imageExtent = ext;
    free(modes);
    r = vkCreateDisplayPlaneSurfaceKHR(inst, &sci, 0, out);
    TR("vkglfw: display surface %dx%d = %d", g_w, g_h, (int)r);
    return r;
}

// ---------------- LWJGL's library loading ----------------
typedef struct { const char *name; void *fn; } Sym;
static const Sym glfw_syms[] = {
#define S(n) { #n, (void *)n },
    S(glfwInit) S(glfwTerminate) S(glfwInitHint) S(glfwGetVersion) S(glfwGetVersionString) S(glfwGetError) S(glfwWindowHint) S(glfwDefaultWindowHints)
    S(glfwCreateWindow) S(glfwDestroyWindow) S(glfwWindowShouldClose) S(glfwSetWindowShouldClose) S(glfwPollEvents) S(glfwWaitEvents) S(glfwWaitEventsTimeout) S(glfwPostEmptyEvent)
    S(glfwSwapInterval) S(glfwGetFramebufferSize) S(glfwGetWindowSize) S(glfwGetPrimaryMonitor) S(glfwGetVideoMode) S(glfwGetTime) S(glfwGetWindowAttrib) S(glfwExtensionSupported)
    S(glfwGetInputMode) S(glfwRawMouseMotionSupported)
    S(glfwVulkanSupported) S(glfwGetRequiredInstanceExtensions) S(glfwGetInstanceProcAddress) S(glfwGetPhysicalDevicePresentationSupport) S(glfwCreateWindowSurface)
#undef S
    { 0, 0 }
};
static int h_glfw, h_vk, h_self;
static const char *dl_err = "";
typedef struct { int handle; } ModH;
static ModH g_mods[24]; static int g_nmods;
static void *module_open(const char *name) {   // system modules (libSce*.sprx): the console's own loader
    const char *b = strrchr(name, '/'); b = b ? b + 1 : name;
    int res = 0, h = sceKernelLoadStartModule(b, 0, 0, 0, 0, &res);
    if (h < 0) { TR("module %s: load failed %x", b, h); return 0; }
    for (int i = 0; i < g_nmods; i++) if (g_mods[i].handle == h) return &g_mods[i];
    if (g_nmods >= 24) return 0;
    g_mods[g_nmods].handle = h;
    return &g_mods[g_nmods++];
}
static int is_module(void *p) { return (char *)p >= (char *)g_mods && (char *)p < (char *)(g_mods + 24); }
void *(*ps5_dll_handle_fn)(void) = 0;   // set by jvm_host.cpp to the ACTIVE JDK's ps5_dl.cpp
void *(*ps5_dlsym_fn)(const char *) = 0;
void *dlopen(const char *name, int flags) {
    (void)flags; TR("dlopen(%s)", name ? name : "(self)");
    if (!name) return &h_self;
    if (strstr(name, "glfw")) return &h_glfw;
    if (strstr(name, "vulkan")) return &h_vk;
    if (strstr(name, ".sprx") || strstr(name, "libSce") || strstr(name, "libkernel")) return module_open(name);
    if (strstr(name, "zip")) return &h_self;
    dl_err = "library is not available in this build"; return 0;
}
void *dlsym(void *h, const char *sym) {
    if (is_module(h)) { void *a = 0; if (sceKernelDlsym(((ModH *)h)->handle, sym, &a) == 0) return a; return 0; }
    if (h == &h_vk) {
        if (!strcmp(sym, "vkGetInstanceProcAddr")) return (void *)vk_gipa;
        return (void *)vk_icdGetInstanceProcAddr(0, sym);   // global commands; instance/device commands are asked through vkGetInstanceProcAddr
    }
    if (h == &h_glfw) {
        for (const Sym *s = glfw_syms; s->name; s++) if (!strcmp(s->name, sym)) return s->fn;
        return (void *)stub_zero;   // LWJGL resolves every GLFW function when the class loads; the ones that are not implemented do nothing
    }
    if (ps5_dlsym_fn && (h == &h_self || (ps5_dll_handle_fn && h == ps5_dll_handle_fn()))) { void *f = ps5_dlsym_fn(sym); if (f) return f; }
    TR("dlsym(other, %s): not found", sym); dl_err = "symbol not found"; return 0;
}
char *dlerror(void) { const char *e = dl_err; dl_err = ""; return (char *)e; }
int dlclose(void *h) { (void)h; return 0; }
int dlinfo(void *h, int req, void *arg) { (void)h; (void)req; (void)arg; dl_err = "dlinfo is not supported"; return -1; }

// ---------------- JNI_OnLoad_<lib>: the JDK finds statically linked libraries by this name ----------------
#define ONLOAD(n) JNIEXPORT jint JNICALL JNI_OnLoad_##n(JavaVM *vm, void *r) { (void)vm; (void)r; TR("JNI_OnLoad_" #n); return 0x00010008; }
ONLOAD(lwjgl_glfw) ONLOAD(lwjgl_vulkan)
extern jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved);
JNIEXPORT jint JNICALL JNI_OnLoad_lwjgl(JavaVM *vm, void *r) { TR("JNI_OnLoad_lwjgl -> JNI_OnLoad"); (void)JNI_OnLoad(vm, r); return 0x00010008; }   // the JDK insists on JNI 1.8+, LWJGL reports 1.6
JNIEXPORT __attribute__((weak)) jint JNICALL JNI_OnLoad_jimage(JavaVM *vm, void *r) { (void)vm; (void)r; return 0x00010008; }

// ---------------- executable memory for libffi closures (GLFW callbacks) ----------------
static pthread_mutex_t ffi_m = PTHREAD_MUTEX_INITIALIZER; static char *ffi_cur, *ffi_end; static void *ffi_freel;
void *ffi_closure_alloc(size_t size, void **code) {
    void *p = 0;
    if (!code || size > 64) return 0;
    pthread_mutex_lock(&ffi_m);
    if (ffi_freel) { p = ffi_freel; ffi_freel = *(void **)p; }
    else {
        if (ffi_cur + 64 > ffi_end) {
            void *a = mmap(0, 256 * 1024, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
            if (a == MAP_FAILED) { pthread_mutex_unlock(&ffi_m); TR("ffi_closure_alloc: mmap failed errno=%d", errno); return 0; }
            if (mprotect(a, 256 * 1024, PROT_READ | PROT_WRITE | PROT_EXEC)) { munmap(a, 256 * 1024); pthread_mutex_unlock(&ffi_m); return 0; }
            ffi_cur = (char *)a; ffi_end = ffi_cur + 256 * 1024;
        }
        p = ffi_cur; ffi_cur += 64;
    }
    pthread_mutex_unlock(&ffi_m);
    *code = p;
    return p;
}
void ffi_closure_free(void *p) { if (!p) return; pthread_mutex_lock(&ffi_m); *(void **)p = ffi_freel; ffi_freel = p; pthread_mutex_unlock(&ffi_m); }
// libffi's x86-64 backend references its Windows-ABI entry points; LWJGL never uses that ABI
int ffi_prep_cif_machdep_efi64(void *c) { (void)c; return 2; }
int ffi_prep_closure_loc_efi64(void) { return 2; }
int ffi_prep_go_closure_efi64(void) { return 2; }
void ffi_call_efi64(void) {}
void ffi_call_go_efi64(void) {}
