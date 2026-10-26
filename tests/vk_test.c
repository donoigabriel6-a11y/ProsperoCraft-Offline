// sandbox test of the Vulkan build of the shim (ps5lwjgl.c -DPS5_VK + ps5vk.inc): LWJGL's "vulkan" library handle, the SDL_Vulkan_* functions Minecraft 26.x's Vulkan backend calls, and the VK_KHR_display surface
// on a fake driver (vk_icdGetInstanceProcAddr below answers the five display commands the surface code uses).
#define PS5_VK 1
#define NO_VIDEO_WRAPS 1   // as the real build (make_mcvk_title.sh): ps5vk.inc defines the two flip wrappers it needs
#define NO_PREPARE_FIX 1
#include "../ps5lwjgl.c"
#include <unistd.h>
void ps5_trace(const char *m) { (void)m; }
int scePadReadState(int h, PadState *s) { (void)h; memset(s, 0, sizeof *s); s->connected = 1; s->lx = s->ly = s->rx = s->ry = 128; return 0; }
int scePadInit(void) { return 0; } int scePadOpen(int a, int b, int c, const void *d) { (void)a; (void)b; (void)c; (void)d; return 1; }
int sceUserServiceInitialize(void *p) { (void)p; return 0; } int sceUserServiceGetInitialUser(int *u) { *u = 1; return 0; }
size_t ps5_opengl_heap_live_bytes(void) { return 0; } void ps5_opengl_heap_stats_print(unsigned x) { (void)x; }
static int fails;
#define CHECK(c, ...) do { if (!(c)) { dprintf(1, "FAIL line %d: ", __LINE__); dprintf(1, __VA_ARGS__); dprintf(1, "\n"); fails++; } } while (0)
static int g_surface_calls; static VkExtent2D g_surf_ext; static uint32_t g_surf_mode;
static VkResult f_enum(VkInstance i, uint32_t *n, VkPhysicalDevice *p) { (void)i; if (p) p[0] = (VkPhysicalDevice)(uintptr_t)0x1234; *n = 1; return VK_SUCCESS; }
static VkResult f_disp(VkPhysicalDevice pd, uint32_t *n, VkDisplayPropertiesKHR *p) { (void)pd; if (p) { memset(p, 0, sizeof *p); p[0].display = (VkDisplayKHR)(uintptr_t)0x77; } *n = 1; return VK_SUCCESS; }
static VkResult f_planes(VkPhysicalDevice pd, uint32_t *n, VkDisplayPlanePropertiesKHR *p) { (void)pd; if (p) { memset(p, 0, sizeof *p); p[0].currentStackIndex = 3; } *n = 1; return VK_SUCCESS; }
static VkResult f_modes(VkPhysicalDevice pd, VkDisplayKHR d, uint32_t *n, VkDisplayModePropertiesKHR *p) {
    (void)pd; (void)d; if (p) { memset(p, 0, 3 * sizeof *p); uint32_t w[3] = { 3840, 2560, 1920 }, h[3] = { 2160, 1440, 1080 }; for (int i = 0; i < 3; i++) { p[i].displayMode = (VkDisplayModeKHR)(uintptr_t)(0x100 + i); p[i].parameters.visibleRegion.width = w[i]; p[i].parameters.visibleRegion.height = h[i]; p[i].parameters.refreshRate = 59940; } }
    *n = 3; return VK_SUCCESS; }
static VkResult f_create(VkInstance i, const VkDisplaySurfaceCreateInfoKHR *ci, const VkAllocationCallbacks *a, VkSurfaceKHR *s) {
    (void)i; (void)a; g_surface_calls++; g_surf_ext = ci->imageExtent; g_surf_mode = (uint32_t)(uintptr_t)ci->displayMode; *s = (VkSurfaceKHR)(uintptr_t)0xABCD; return VK_SUCCESS; }
static VkResult f_layers_bad(uint32_t *n, VkLayerProperties *p) { (void)n; (void)p; return VK_ERROR_LAYER_NOT_PRESENT; }
static VkResult f_pm(VkPhysicalDevice pd, VkSurfaceKHR sf, uint32_t *n, VkPresentModeKHR *m) { (void)pd; (void)sf; if (m) { if (*n < 1) { return VK_INCOMPLETE; } m[0] = VK_PRESENT_MODE_FIFO_KHR; } *n = 1; return VK_SUCCESS; }

// ---- overlay (cursor / on-screen keyboard copied into the swapchain image): fake device that records what the shim submits ----
static struct { int pool, cbn, sem, fence, buf, mem, submit, present, copy, barrier, wait, reset_f; uint32_t nreg; VkBufferImageCopy first, last; const VkSemaphore *submit_wait; uint32_t submit_nwait; VkSemaphore submit_sig, present_wait; VkImageLayout lay[4]; int nlay; uint8_t *map; VkImage copy_img; VkImageUsageFlags sc_usage; int refuse_dst; } fo;
static VkResult f_ccp(VkDevice d, const VkCommandPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkCommandPool *o) { (void)d; (void)a; fo.pool++; *o = (VkCommandPool)(uintptr_t)0x11; return ci->queueFamilyIndex == 0 ? VK_SUCCESS : VK_ERROR_UNKNOWN; }
static VkResult f_acb(VkDevice d, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *o) { (void)d; (void)ai; fo.cbn++; *o = (VkCommandBuffer)(uintptr_t)0x22; return VK_SUCCESS; }
static VkResult f_beg(VkCommandBuffer c, const VkCommandBufferBeginInfo *b) { (void)c; (void)b; return VK_SUCCESS; }
static VkResult f_end(VkCommandBuffer c) { (void)c; return VK_SUCCESS; }
static VkResult f_rcb(VkCommandBuffer c, VkCommandBufferResetFlags f) { (void)c; (void)f; return VK_SUCCESS; }
static void f_bar(VkCommandBuffer c, VkPipelineStageFlags a, VkPipelineStageFlags b, VkDependencyFlags f, uint32_t n1, const VkMemoryBarrier *p1, uint32_t n2, const VkBufferMemoryBarrier *p2, uint32_t n3, const VkImageMemoryBarrier *p3) {
    (void)c; (void)a; (void)b; (void)f; (void)n1; (void)p1; (void)n2; (void)p2; fo.barrier++; if (n3 && fo.nlay < 4) { fo.lay[fo.nlay++] = p3->oldLayout; fo.lay[fo.nlay++] = p3->newLayout; } }
static void f_copy(VkCommandBuffer c, VkBuffer b, VkImage im, VkImageLayout l, uint32_t n, const VkBufferImageCopy *r) { (void)c; (void)b; (void)l; fo.copy++; fo.nreg = n; fo.first = r[0]; fo.last = r[n - 1]; fo.copy_img = im; }
static VkResult f_cbuf(VkDevice d, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *a, VkBuffer *o) { (void)d; (void)a; fo.buf++; *o = (VkBuffer)(uintptr_t)0x33; return (ci->usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ? VK_SUCCESS : VK_ERROR_UNKNOWN; }
static void f_breq(VkDevice d, VkBuffer b, VkMemoryRequirements *r) { (void)d; (void)b; r->size = 1920u * 1080 * 4; r->alignment = 256; r->memoryTypeBits = 0x6; }
static VkResult f_amem(VkDevice d, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a, VkDeviceMemory *o) { (void)d; (void)a; fo.mem++; *o = (VkDeviceMemory)(uintptr_t)0x44; return ai->memoryTypeIndex == 2 ? VK_SUCCESS : VK_ERROR_UNKNOWN; }   // type 2 = host visible + coherent in the fake heap
static VkResult f_bind(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize o) { (void)d; (void)b; (void)m; (void)o; return VK_SUCCESS; }
static VkResult f_map(VkDevice d, VkDeviceMemory m, VkDeviceSize o, VkDeviceSize sz, VkMemoryMapFlags f, void **pp) { (void)d; (void)m; (void)o; (void)sz; (void)f; fo.map = (uint8_t *)calloc(1, 1920u * 1080 * 4); *pp = fo.map; return VK_SUCCESS; }
static VkResult f_csem(VkDevice d, const VkSemaphoreCreateInfo *ci, const VkAllocationCallbacks *a, VkSemaphore *o) { (void)d; (void)ci; (void)a; fo.sem++; *o = (VkSemaphore)(uintptr_t)0x55; return VK_SUCCESS; }
static VkResult f_cfen(VkDevice d, const VkFenceCreateInfo *ci, const VkAllocationCallbacks *a, VkFence *o) { (void)d; (void)ci; (void)a; fo.fence++; *o = (VkFence)(uintptr_t)0x66; return VK_SUCCESS; }
static VkResult f_wfen(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t t) { (void)d; (void)n; (void)f; (void)all; (void)t; fo.wait++; return VK_SUCCESS; }
static VkResult f_rfen(VkDevice d, uint32_t n, const VkFence *f) { (void)d; (void)n; (void)f; fo.reset_f++; return VK_SUCCESS; }
static VkResult f_sub(VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence f) { (void)q; (void)n; (void)f; fo.submit++; fo.submit_wait = si->pWaitSemaphores; fo.submit_nwait = si->waitSemaphoreCount; fo.submit_sig = si->pSignalSemaphores[0]; return VK_SUCCESS; }
static VkResult f_gimg(VkDevice d, VkSwapchainKHR sc, uint32_t *n, VkImage *im) { (void)d; (void)sc; if (im) for (uint32_t i = 0; i < *n && i < 3; i++) im[i] = (VkImage)(uintptr_t)(0x100 + i); *n = 3; return VK_SUCCESS; }
static VkResult f_qp(VkQueue q, const VkPresentInfoKHR *pi) { (void)q; fo.present++; fo.present_wait = pi->waitSemaphoreCount ? pi->pWaitSemaphores[0] : 0; return VK_SUCCESS; }
static void f_gmp(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties *mp) { (void)pd; memset(mp, 0, sizeof *mp); mp->memoryTypeCount = 3; mp->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT; mp->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT; mp->memoryTypes[2].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT; }
static VkPresentModeKHR g_sc_mode = (VkPresentModeKHR)99;
static VkResult f_csc(VkDevice d, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *a, VkSwapchainKHR *sc) { (void)d; (void)a; g_sc_mode = ci->presentMode; fo.sc_usage = ci->imageUsage; if (fo.refuse_dst && (ci->imageUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return VK_ERROR_INITIALIZATION_FAILED; *sc = (VkSwapchainKHR)(uintptr_t)0x99; return VK_SUCCESS; }
static PFN_vkVoidFunction f_gdpa(VkDevice d, const char *n) {
    (void)d;
#define N(nm, f) if (!strcmp(n, nm)) return (PFN_vkVoidFunction)f;
    N("vkCreateSwapchainKHR", f_csc) N("vkCreateCommandPool", f_ccp) N("vkAllocateCommandBuffers", f_acb) N("vkBeginCommandBuffer", f_beg) N("vkEndCommandBuffer", f_end) N("vkResetCommandBuffer", f_rcb)
    N("vkCmdPipelineBarrier", f_bar) N("vkCmdCopyBufferToImage", f_copy) N("vkCreateBuffer", f_cbuf) N("vkGetBufferMemoryRequirements", f_breq) N("vkAllocateMemory", f_amem) N("vkBindBufferMemory", f_bind)
    N("vkMapMemory", f_map) N("vkCreateSemaphore", f_csem) N("vkCreateFence", f_cfen) N("vkWaitForFences", f_wfen) N("vkResetFences", f_rfen) N("vkQueueSubmit", f_sub) N("vkGetSwapchainImagesKHR", f_gimg) N("vkQueuePresentKHR", f_qp)
#undef N
    return 0; }
PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance i, const char *n) {
    (void)i;
    if (!strcmp(n, "vkEnumeratePhysicalDevices")) return (PFN_vkVoidFunction)f_enum;
    if (!strcmp(n, "vkGetPhysicalDeviceDisplayPropertiesKHR")) return (PFN_vkVoidFunction)f_disp;
    if (!strcmp(n, "vkGetPhysicalDeviceDisplayPlanePropertiesKHR")) return (PFN_vkVoidFunction)f_planes;
    if (!strcmp(n, "vkGetDisplayModePropertiesKHR")) return (PFN_vkVoidFunction)f_modes;
    if (!strcmp(n, "vkCreateDisplayPlaneSurfaceKHR")) return (PFN_vkVoidFunction)f_create;
    if (!strcmp(n, "vkGetPhysicalDeviceSurfacePresentModesKHR")) return (PFN_vkVoidFunction)f_pm;
    if (!strcmp(n, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)f_gdpa;
    if (!strcmp(n, "vkQueuePresentKHR")) return (PFN_vkVoidFunction)f_qp;
    if (!strcmp(n, "vkGetPhysicalDeviceMemoryProperties")) return (PFN_vkVoidFunction)f_gmp;
    if (!strcmp(n, "vkCreateInstance")) return (PFN_vkVoidFunction)0x5151;
    if (!strcmp(n, "vkEnumerateInstanceLayerProperties")) return (PFN_vkVoidFunction)f_layers_bad;   // the Mesa/RADV answer seen on the console
    return 0;
}
static void *S(const char *n) { void *f = dlsym(dlopen("libSDL3.so.0", 0), n); if (!f) { dprintf(1, "FAIL missing %s\n", n); fails++; } return f; }
#define CALL(ret, name, args, ...) ((ret (*) args)S(name))(__VA_ARGS__)
int main(void) {
    g_pad = 1; g_w = 1920; g_h = 1080; g_inited = 1;
    // LWJGL's VK class: dlopen("libvulkan.so.1") + dlsym("vkGetInstanceProcAddr") (3.4 VK.create()), global commands through it
    void *lib = dlopen("libvulkan.so.1", 0); CHECK(lib != 0, "dlopen libvulkan.so.1");
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr"); CHECK(gipa != 0, "vkGetInstanceProcAddr");
    CHECK(gipa && (uintptr_t)gipa(0, "vkCreateInstance") == 0x5151, "gipa forwards to the driver");
    CHECK((uintptr_t)dlsym(lib, "vkCreateInstance") == 0x5151, "global command through dlsym");
    // the layer query is answered by the shim (no layers, success) through both paths LWJGL can take; the driver's own answer (VK_ERROR_LAYER_NOT_PRESENT) must not reach Minecraft
    { PFN_vkEnumerateInstanceLayerProperties a = (PFN_vkEnumerateInstanceLayerProperties)gipa(0, "vkEnumerateInstanceLayerProperties"), b = (PFN_vkEnumerateInstanceLayerProperties)dlsym(lib, "vkEnumerateInstanceLayerProperties");
      uint32_t c = 99; CHECK(a && a(&c, 0) == VK_SUCCESS && c == 0, "layer query via gipa: success, 0 layers"); c = 99; CHECK(b && b(&c, 0) == VK_SUCCESS && c == 0, "layer query via dlsym: success, 0 layers"); }
    // SDL: what VulkanBackend / VulkanInstance / VulkanPhysicalDevice / VulkanGpuSurface call
    { PFN_vkGetPhysicalDeviceSurfacePresentModesKHR pm = (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)gipa((VkInstance)1, "vkGetPhysicalDeviceSurfacePresentModesKHR");
      PFN_vkGetDeviceProcAddr gd = (PFN_vkGetDeviceProcAddr)gipa((VkInstance)1, "vkGetDeviceProcAddr");
      uint32_t c = 0; VkPresentModeKHR m[4] = {0};
      CHECK(pm && pm(0, 0, &c, 0) == VK_SUCCESS && c == 2, "present modes: the driver's FIFO + IMMEDIATE added (count %u)", c);
      c = 4; CHECK(pm(0, 0, &c, m) == VK_SUCCESS && c == 2 && m[0] == VK_PRESENT_MODE_FIFO_KHR && m[1] == VK_PRESENT_MODE_IMMEDIATE_KHR, "present modes list (%u: %d %d)", c, (int)m[0], (int)m[1]);
      CHECK(gd != 0, "vkGetDeviceProcAddr through the shim");
      PFN_vkCreateSwapchainKHR csc = gd ? (PFN_vkCreateSwapchainKHR)gd((VkDevice)1, "vkCreateSwapchainKHR") : 0; CHECK(csc != 0, "vkCreateSwapchainKHR hook");
      VkSwapchainCreateInfoKHR ci; memset(&ci, 0, sizeof ci); VkSwapchainKHR sc = 0;
      ci.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR; if (csc) csc((VkDevice)1, &ci, 0, &sc); CHECK(g_vsync == 0 && g_sc_mode == VK_PRESENT_MODE_FIFO_KHR, "IMMEDIATE: VSync off, the driver gets FIFO (vsync %d mode %d)", g_vsync, (int)g_sc_mode);
      ci.presentMode = VK_PRESENT_MODE_FIFO_KHR; if (csc) csc((VkDevice)1, &ci, 0, &sc); CHECK(g_vsync == 1 && g_sc_mode == VK_PRESENT_MODE_FIFO_KHR, "FIFO: VSync on"); }
    CHECK(CALL(int, "SDL_Vulkan_LoadLibrary", (const char *), 0) == 1, "load library");
    CHECK(CALL(void *, "SDL_Vulkan_GetVkGetInstanceProcAddr", (void)) == (void *)gipa || CALL(void *, "SDL_Vulkan_GetVkGetInstanceProcAddr", (void)) != 0, "SDL gives a vkGetInstanceProcAddr");
    PFN_vkGetInstanceProcAddr sg = (PFN_vkGetInstanceProcAddr)CALL(void *, "SDL_Vulkan_GetVkGetInstanceProcAddr", (void)); CHECK(sg && (uintptr_t)sg(0, "vkCreateInstance") == 0x5151, "SDL's gipa works");
    unsigned n = 0; const char *const *ex = CALL(const char *const *, "SDL_Vulkan_GetInstanceExtensions", (unsigned *), &n);
    CHECK(n == 2 && ex && !strcmp(ex[0], "VK_KHR_surface") && !strcmp(ex[1], "VK_KHR_display"), "instance extensions (%u)", n);
    CHECK(CALL(int, "SDL_Vulkan_GetPresentationSupport", (void *, void *, unsigned), (void *)1, (void *)2, 0) == 1 && CALL(int, "SDL_Vulkan_GetPresentationSupport", (void *, void *, unsigned), (void *)1, (void *)2, 1) == 0, "presentation support: family 0 only");
    void *win = CALL(void *, "SDL_CreateWindow", (const char *, int, int, uint64_t), "Minecraft", 1280, 720, 0x10000000);
    CHECK(win == WIN, "window handle (no EGL)"); CHECK(g_created == 1, "window created without a GL context");
    CHECK((CALL(unsigned long long, "SDL_GetWindowFlags", (void *), win) & 0x10000000) != 0 && (CALL(unsigned long long, "SDL_GetWindowFlags", (void *), win) & 0x2) == 0, "window flags: VULKAN, not OPENGL");
    uint64_t surf = 0; CHECK(CALL(int, "SDL_Vulkan_CreateSurface", (void *, void *, const void *, void *), win, (void *)1, 0, &surf) == 1, "create surface");
    CHECK(surf == 0xABCD && g_surface_calls == 1, "surface handle");
    CHECK(g_surf_ext.width == 1920 && g_surf_ext.height == 1080 && g_surf_mode == 0x102, "1080p mode picked (%ux%u mode %x)", g_surf_ext.width, g_surf_ext.height, g_surf_mode);
    CHECK(g_w == 1920 && g_h == 1080, "window size follows the mode");

    // ---- overlay: cursor and keyboard copied into the swapchain image before the present ----
    {   PFN_vkGetDeviceProcAddr gd = (PFN_vkGetDeviceProcAddr)sg((VkInstance)1, "vkGetDeviceProcAddr");
        PFN_vkCreateSwapchainKHR csc = (PFN_vkCreateSwapchainKHR)gd((VkDevice)1, "vkCreateSwapchainKHR"); PFN_vkQueuePresentKHR qp = (PFN_vkQueuePresentKHR)gd((VkDevice)1, "vkQueuePresentKHR");
        CHECK(qp != 0 && (void *)qp != (void *)f_qp, "vkQueuePresentKHR is hooked");
        CHECK((void *)sg((VkInstance)1, "vkQueuePresentKHR") == (void *)qp, "present is hooked through the instance lookup too");
        VkSwapchainCreateInfoKHR ci; memset(&ci, 0, sizeof ci); ci.presentMode = VK_PRESENT_MODE_FIFO_KHR; ci.imageExtent.width = 1920; ci.imageExtent.height = 1080; ci.imageFormat = (VkFormat)44; ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        VkSwapchainKHR sc = 0; fo.refuse_dst = 1; VkResult r = csc((VkDevice)1, &ci, 0, &sc);
        CHECK(r == VK_SUCCESS && !(fo.sc_usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT), "a surface that refuses TRANSFER_DST: swapchain made as the game asked (%d, usage 0x%x)", (int)r, (unsigned)fo.sc_usage);
        VkSemaphore gw = (VkSemaphore)(uintptr_t)0x999; uint32_t ix = 1; VkPresentInfoKHR pi; memset(&pi, 0, sizeof pi); pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR; pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &gw; pi.swapchainCount = 1; pi.pSwapchains = &sc; pi.pImageIndices = &ix;
        g_polled = 1; g_cursor_mode = 0; g_cur_vis = 1; g_cx = 300; g_cy = 200; fo.refuse_dst = 0;
        int p0 = fo.present; qp((VkQueue)1, &pi);
        CHECK(fo.present == p0 + 1 && fo.copy == 0 && fo.present_wait == gw, "no TRANSFER_DST swapchain: plain present, game's semaphore (copy %d)", fo.copy);
        r = csc((VkDevice)1, &ci, 0, &sc);
        CHECK(r == VK_SUCCESS && (fo.sc_usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) && (fo.sc_usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT), "the overlay asks for TRANSFER_DST on top of the game's usage (0x%x)", (unsigned)fo.sc_usage);
        // cursor
        p0 = fo.present; qp((VkQueue)1, &pi);
        CHECK(fo.pool == 1 && fo.cbn == 1 && fo.sem == 1 && fo.fence == 1 && fo.buf == 1 && fo.mem == 1, "overlay objects created once (pool %d cb %d sem %d fence %d buf %d mem %d)", fo.pool, fo.cbn, fo.sem, fo.fence, fo.buf, fo.mem);
        CHECK(fo.submit == 1 && fo.submit_nwait == 1 && fo.submit_wait && fo.submit_wait[0] == gw && fo.submit_sig != 0, "the copy waits for the game's semaphore and signals its own");
        CHECK(fo.present == p0 + 1 && fo.present_wait == fo.submit_sig && fo.present_wait != gw, "the present waits for the overlay's semaphore");
        CHECK(fo.copy == 1 && fo.copy_img == (VkImage)(uintptr_t)0x101, "copied into swapchain image 1");
        CHECK(fo.nlay == 4 && fo.lay[0] == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR && fo.lay[1] == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && fo.lay[2] == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && fo.lay[3] == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, "layouts PRESENT_SRC -> TRANSFER_DST -> PRESENT_SRC");
        CHECK(fo.nreg > 20 && fo.nreg < 200 && fo.first.imageOffset.x >= 300 && fo.first.imageOffset.y == 200 && fo.last.imageOffset.y < 200 + 12 * 3 + 3, "cursor: %u regions from (%d,%d) to row %d", fo.nreg, fo.first.imageOffset.x, fo.first.imageOffset.y, fo.last.imageOffset.y);
        { size_t o = (size_t)fo.first.bufferOffset; CHECK(fo.map[o + 3] == 255 && (fo.map[o] | fo.map[o + 1] | fo.map[o + 2]) != 0, "cursor pixel written to the staging buffer (BGRA %02x %02x %02x %02x)", fo.map[o], fo.map[o + 1], fo.map[o + 2], fo.map[o + 3]); }
        { unsigned tot = 0; (void)tot; CHECK(fo.first.imageExtent.height == 1 && fo.first.imageExtent.depth == 1 && fo.first.imageSubresource.layerCount == 1, "regions are single rows"); }
        // same picture next frame: the fence is waited for, the staging buffer is not rewritten, the copy is repeated
        int w0 = fo.wait; qp((VkQueue)1, &pi); CHECK(fo.wait == w0 + 1 && fo.reset_f >= 1 && fo.submit == 2 && fo.copy == 2, "second frame: previous copy awaited, copy repeated");
        // the pointer moved: new regions at the new place
        g_cx = 1000; g_cy = 500; qp((VkQueue)1, &pi); CHECK(fo.first.imageOffset.x >= 1000 && fo.first.imageOffset.y == 500, "cursor moved to (1000,500): first region (%d,%d)", fo.first.imageOffset.x, fo.first.imageOffset.y);
        // nothing to draw: gameplay (relative mode) and an idle cursor go straight through
        int c0 = fo.copy; p0 = fo.present; g_cursor_mode = GLFW_CURSOR_DISABLED; qp((VkQueue)1, &pi); CHECK(fo.copy == c0 && fo.present == p0 + 1 && fo.present_wait == gw, "gameplay: no overlay");
        g_cursor_mode = 0; g_cur_vis = 0; qp((VkQueue)1, &pi); CHECK(fo.copy == c0 && fo.present_wait == gw, "idle cursor: no overlay");
        // keyboard
        g_osk_open = 1; g_osk_row = 1; g_osk_k = 2; qp((VkQueue)1, &pi);
        CHECK(fo.copy == c0 + 1 && fo.nreg > 400 && fo.nreg < 2000, "keyboard open: %u copy regions", fo.nreg);
        CHECK(fo.first.imageOffset.x > 100 && fo.first.imageOffset.x < 960 && fo.first.imageExtent.width > 600, "keyboard panel: first row (%d, %d) %u px wide", fo.first.imageOffset.x, fo.first.imageOffset.y, fo.first.imageExtent.width);
        CHECK(fo.last.imageOffset.y > 700 && fo.last.imageOffset.y < 1080, "keyboard docked at the bottom (last row %d)", fo.last.imageOffset.y);
        { size_t o = (size_t)fo.first.bufferOffset; CHECK(fo.map[o + 3] == 255 && fo.map[o] < 60 && fo.map[o + 2] < 60, "translucent panel is dark (%02x %02x %02x)", fo.map[o], fo.map[o + 1], fo.map[o + 2]); }
        g_osk_open = 0;
        // multi-swapchain / too many waits are never touched
        p0 = fo.present; c0 = fo.copy; g_cur_vis = 1; pi.swapchainCount = 2; qp((VkQueue)1, &pi); pi.swapchainCount = 1; CHECK(fo.copy == c0 && fo.present == p0 + 1, "two swapchains in one present: untouched");
        g_cur_vis = 0; }
    dprintf(1, "fails=%d\n", fails); return fails;
}
