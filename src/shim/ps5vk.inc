// ps5vk.inc: the Vulkan side of the shim (included by ps5lwjgl.c when it is built with -DPS5_VK, make_mcvk_title.sh).
// The title links Mesa's RADV statically, so there is no libvulkan: LWJGL's "vulkan" library is a handle whose vkGetInstanceProcAddr is a forwarder to vk_icdGetInstanceProcAddr, and
// Minecraft 26.x's own Vulkan backend (com.mojang.renderpearl.backend.vulkan) reaches the display through SDL_Vulkan_*:
//   SDL_Vulkan_LoadLibrary / GetVkGetInstanceProcAddr (VK.create gets the function pointer from here), GetInstanceExtensions, GetPresentationSupport, CreateSurface.
// The surface is a VK_KHR_display plane surface (the same path ps5lwjgl_vk.c / VkTest proved: 1920x1080, the title never opens VideoOut itself).
extern PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance, const char *);
extern int sceSystemServiceHideSplashScreen(void) __attribute__((weak));
static int h_vk;
static char sdl_err[128] = "";   // SDL_GetError text (ps5sdl.inc)
// What "libvulkan.so.1" would export as vkGetInstanceProcAddr. The loader, not the driver, answers the layer query (Minecraft's VulkanInstance.getSupportedValidationLayers calls it on every start; the first mc-vk run got
// VK_ERROR_LAYER_NOT_PRESENT from it and gave up): no layers, VK_SUCCESS. The first lookups are logged (vk: gipa ...), and every lookup the driver answers with NULL.
static VkResult VKAPI_CALL ps5vk_layers(uint32_t *n, VkLayerProperties *p) { (void)p; if (n) *n = 0; return VK_SUCCESS; }
// Probes for the VSync question: which present modes the display surface offers and which one the game's swapchain asks for (logged once each), everything else passes through.
static PFN_vkGetDeviceProcAddr ps5vk_real_gdpa; static PFN_vkCreateSwapchainKHR ps5vk_real_csc; static PFN_vkGetPhysicalDeviceSurfacePresentModesKHR ps5vk_real_pm;
static const char *ps5vk_pmname(int m) { switch (m) { case 0: return "IMMEDIATE"; case 1: return "MAILBOX"; case 2: return "FIFO"; case 3: return "FIFO_RELAXED"; default: return "?"; } }
// VSync OFF: the driver's display surface offers FIFO only (ps5vk_wsi.c), so the game never asks for anything else. The shim adds IMMEDIATE to the list, and when the game's swapchain asks for
// it (Mojang's backend does that with VSync off) the driver gets FIFO and g_vsync drops to 0: the wrapped flip below then submits the tearing flip mode, as the OpenGL title's UNCAP does.
static VkResult VKAPI_CALL ps5vk_pm(VkPhysicalDevice d, VkSurfaceKHR sf, uint32_t *n, VkPresentModeKHR *m) {
    uint32_t cap = n ? *n : 0, c = cap;
    VkResult r = ps5vk_real_pm(d, sf, &c, m);
    if (r < 0 || !n) return r;
    if (!m) { *n = c + 1; return r; }
    if (r == VK_INCOMPLETE || c >= cap) { *n = c; return r; }
    m[c] = VK_PRESENT_MODE_IMMEDIATE_KHR; *n = c + 1;
    { char b[120]; int o = 0; b[0] = 0; for (uint32_t k = 0; k < *n && o < 100; k++) o += snprintf(b + o, sizeof b - o, "%s ", ps5vk_pmname((int)m[k])); static int pl; if (pl++ < 3) TR("vk: surface present modes (IMMEDIATE added by the shim): %s", b); }
    return VK_SUCCESS;
}
// ---- on-screen overlay: the software cursor and the on-screen keyboard of the OpenGL path (ps5lwjgl.c cursor_build / osk_build), for Vulkan ----
// Minecraft draws no pointer of its own, and the keyboard needs a picture. The overlay is the same list of coloured rectangles the GL path turns into triangles (g_ov). Here they are rasterised on the CPU
// into a staging buffer and copied into the swapchain image right before vkQueuePresentKHR (transfer only: no shader, pipeline or render pass of the game is touched). The swapchain gets
// VK_IMAGE_USAGE_TRANSFER_DST for that (if the surface refuses it the swapchain is made as the game asked and there is simply no overlay). Translucent panels are blended against a fixed dark colour
// (the frame is not read back); the cursor is opaque anyway. Runs of covered pixels become one copy region each, so only the cursor / the keyboard panel is written.
static VkInstance ps5vk_inst; static VkPhysicalDevice ps5vk_pd;   // SDL_Vulkan_CreateSurface
static PFN_vkQueuePresentKHR ps5vk_real_qp;
#define PS5VK_OV_REG 16384
static struct {
    VkDevice dev; int usage_ok, failed, ready, bgr; uint32_t w, h;
    VkCommandPool pool; VkCommandBuffer cb; VkSemaphore sem; VkFence fence; int fence_armed;
    VkBuffer buf; VkDeviceMemory mem; uint8_t *map, *shadow, *cov; size_t bytes;
    VkBufferImageCopy *reg; int nreg; unsigned long hash; int have_hash;
    VkSwapchainKHR sc; VkImage img[8]; uint32_t nimg;
    PFN_vkCreateCommandPool CreateCommandPool; PFN_vkAllocateCommandBuffers AllocateCommandBuffers; PFN_vkBeginCommandBuffer Begin; PFN_vkEndCommandBuffer End; PFN_vkResetCommandBuffer Reset;
    PFN_vkCmdPipelineBarrier Barrier; PFN_vkCmdCopyBufferToImage Copy; PFN_vkCreateBuffer CreateBuffer; PFN_vkGetBufferMemoryRequirements BufReq; PFN_vkAllocateMemory AllocMem;
    PFN_vkBindBufferMemory Bind; PFN_vkMapMemory Map; PFN_vkCreateSemaphore CreateSem; PFN_vkCreateFence CreateFence; PFN_vkWaitForFences Wait; PFN_vkResetFences ResetFences;
    PFN_vkQueueSubmit Submit; PFN_vkGetSwapchainImagesKHR GetImages;
    unsigned long frames, drawn;
} ps5vk_ov;
// Rasterise g_ov (rectangles, 6 vertices each) into ps5vk_ov.shadow, then the covered runs into the staging buffer and the region list. Returns the number of regions.
static int ps5vk_ov_raster(void) {
    uint32_t W = ps5vk_ov.w, H = ps5vk_ov.h; int ymin = (int)H, ymax = -1, xmin = (int)W, xmax = -1;
    memset(ps5vk_ov.cov, 0, (size_t)W * H);
    for (int i = 0; i + 5 < g_ov_n; i += 6) {
        const OvV *a = &g_ov[i], *b = &g_ov[i + 1], *c = &g_ov[i + 2];
        int x0 = (int)(a->x + 0.5f), x1 = (int)(b->x + 0.5f), y0 = (int)(a->y + 0.5f), y1 = (int)(c->y + 0.5f);
        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > (int)W) x1 = (int)W; if (y1 > (int)H) y1 = (int)H;
        if (x1 <= x0 || y1 <= y0) continue;
        float al = a->a; int r = (int)(a->r * 255.0f + 0.5f), g = (int)(a->g * 255.0f + 0.5f), bl = (int)(a->b * 255.0f + 0.5f);
        if (x0 < xmin) xmin = x0; if (x1 > xmax) xmax = x1; if (y0 < ymin) ymin = y0; if (y1 > ymax) ymax = y1;
        for (int y = y0; y < y1; y++) {
            uint8_t *px = ps5vk_ov.shadow + ((size_t)y * W + x0) * 4, *cv = ps5vk_ov.cov + (size_t)y * W + x0;
            for (int x = x0; x < x1; x++, px += 4, cv++) {
                int pr = r, pg = g, pb = bl;
                if (al < 0.999f) {   // blended against the fixed dark backdrop (or what is already in the overlay)
                    int br = 31, bg = 31, bb = 38; if (*cv) { if (ps5vk_ov.bgr) { bb = px[0]; bg = px[1]; br = px[2]; } else { br = px[0]; bg = px[1]; bb = px[2]; } }
                    pr = (int)(r * al + br * (1 - al) + 0.5f); pg = (int)(g * al + bg * (1 - al) + 0.5f); pb = (int)(bl * al + bb * (1 - al) + 0.5f);
                }
                if (ps5vk_ov.bgr) { px[0] = (uint8_t)pb; px[1] = (uint8_t)pg; px[2] = (uint8_t)pr; } else { px[0] = (uint8_t)pr; px[1] = (uint8_t)pg; px[2] = (uint8_t)pb; }
                px[3] = 255; *cv = 1;
            }
        }
    }
    int n = 0;
    for (int y = ymin; y < ymax && n < PS5VK_OV_REG; y++) {
        const uint8_t *cv = ps5vk_ov.cov + (size_t)y * W;
        for (int x = xmin; x < xmax && n < PS5VK_OV_REG;) {
            if (!cv[x]) { x++; continue; }
            int x0 = x; while (x < xmax && cv[x]) x++;
            size_t off = ((size_t)y * W + x0) * 4; memcpy(ps5vk_ov.map + off, ps5vk_ov.shadow + off, (size_t)(x - x0) * 4);
            VkBufferImageCopy *rg = &ps5vk_ov.reg[n++]; memset(rg, 0, sizeof *rg);
            rg->bufferOffset = off; rg->imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rg->imageSubresource.layerCount = 1;
            rg->imageOffset.x = x0; rg->imageOffset.y = y; rg->imageExtent.width = (uint32_t)(x - x0); rg->imageExtent.height = 1; rg->imageExtent.depth = 1;
        }
    }
    return n;
}
static int ps5vk_ov_init(void) {
    VkDevice d = ps5vk_ov.dev; ps5vk_ov.failed = 1;
#define G(m, T, n) ps5vk_ov.m = (T)ps5vk_real_gdpa(d, n); if (!ps5vk_ov.m) { TR("vk: overlay unavailable: %s missing", n); return 0; }
    G(CreateCommandPool, PFN_vkCreateCommandPool, "vkCreateCommandPool") G(AllocateCommandBuffers, PFN_vkAllocateCommandBuffers, "vkAllocateCommandBuffers") G(Begin, PFN_vkBeginCommandBuffer, "vkBeginCommandBuffer")
    G(End, PFN_vkEndCommandBuffer, "vkEndCommandBuffer") G(Reset, PFN_vkResetCommandBuffer, "vkResetCommandBuffer") G(Barrier, PFN_vkCmdPipelineBarrier, "vkCmdPipelineBarrier")
    G(Copy, PFN_vkCmdCopyBufferToImage, "vkCmdCopyBufferToImage") G(CreateBuffer, PFN_vkCreateBuffer, "vkCreateBuffer") G(BufReq, PFN_vkGetBufferMemoryRequirements, "vkGetBufferMemoryRequirements")
    G(AllocMem, PFN_vkAllocateMemory, "vkAllocateMemory") G(Bind, PFN_vkBindBufferMemory, "vkBindBufferMemory") G(Map, PFN_vkMapMemory, "vkMapMemory") G(CreateSem, PFN_vkCreateSemaphore, "vkCreateSemaphore")
    G(CreateFence, PFN_vkCreateFence, "vkCreateFence") G(Wait, PFN_vkWaitForFences, "vkWaitForFences") G(ResetFences, PFN_vkResetFences, "vkResetFences") G(Submit, PFN_vkQueueSubmit, "vkQueueSubmit")
    G(GetImages, PFN_vkGetSwapchainImagesKHR, "vkGetSwapchainImagesKHR")
#undef G
    PFN_vkGetPhysicalDeviceMemoryProperties gm = (PFN_vkGetPhysicalDeviceMemoryProperties)vk_icdGetInstanceProcAddr(ps5vk_inst, "vkGetPhysicalDeviceMemoryProperties");
    if (!gm || !ps5vk_pd) { TR("vk: overlay unavailable: no physical device memory properties"); return 0; }
    VkCommandPoolCreateInfo pci; memset(&pci, 0, sizeof pci); pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = 0;   // family 0: graphics + transfer (SDL_Vulkan_GetPresentationSupport)
    VkCommandBufferAllocateInfo ai; memset(&ai, 0, sizeof ai); ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VkSemaphoreCreateInfo sci; memset(&sci, 0, sizeof sci); sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fci; memset(&fci, 0, sizeof fci); fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    size_t bytes = (size_t)ps5vk_ov.w * ps5vk_ov.h * 4; ps5vk_ov.bytes = bytes;
    VkBufferCreateInfo bci; memset(&bci, 0, sizeof bci); bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; bci.size = bytes; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult r;
    if ((r = ps5vk_ov.CreateCommandPool(d, &pci, 0, &ps5vk_ov.pool)) != VK_SUCCESS) { TR("vk: overlay unavailable: vkCreateCommandPool %d", (int)r); return 0; }
    ai.commandPool = ps5vk_ov.pool;
    if ((r = ps5vk_ov.AllocateCommandBuffers(d, &ai, &ps5vk_ov.cb)) != VK_SUCCESS) { TR("vk: overlay unavailable: vkAllocateCommandBuffers %d", (int)r); return 0; }
    if ((r = ps5vk_ov.CreateSem(d, &sci, 0, &ps5vk_ov.sem)) != VK_SUCCESS || (r = ps5vk_ov.CreateFence(d, &fci, 0, &ps5vk_ov.fence)) != VK_SUCCESS) { TR("vk: overlay unavailable: semaphore / fence %d", (int)r); return 0; }
    if ((r = ps5vk_ov.CreateBuffer(d, &bci, 0, &ps5vk_ov.buf)) != VK_SUCCESS) { TR("vk: overlay unavailable: vkCreateBuffer %d", (int)r); return 0; }
    VkMemoryRequirements mr; ps5vk_ov.BufReq(d, ps5vk_ov.buf, &mr);
    VkPhysicalDeviceMemoryProperties mp; gm(ps5vk_pd, &mp); int mt = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((mr.memoryTypeBits >> i & 1) && (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { mt = (int)i; break; }
    if (mt < 0) { TR("vk: overlay unavailable: no host-visible coherent memory type (bits 0x%x)", mr.memoryTypeBits); return 0; }
    VkMemoryAllocateInfo mai; memset(&mai, 0, sizeof mai); mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; mai.allocationSize = mr.size; mai.memoryTypeIndex = (uint32_t)mt;
    if ((r = ps5vk_ov.AllocMem(d, &mai, 0, &ps5vk_ov.mem)) != VK_SUCCESS || (r = ps5vk_ov.Bind(d, ps5vk_ov.buf, ps5vk_ov.mem, 0)) != VK_SUCCESS || (r = ps5vk_ov.Map(d, ps5vk_ov.mem, 0, VK_WHOLE_SIZE, 0, (void **)&ps5vk_ov.map)) != VK_SUCCESS) { TR("vk: overlay unavailable: staging memory %d", (int)r); return 0; }
    ps5vk_ov.shadow = (uint8_t *)calloc(1, bytes); ps5vk_ov.cov = (uint8_t *)calloc(1, (size_t)ps5vk_ov.w * ps5vk_ov.h); ps5vk_ov.reg = (VkBufferImageCopy *)calloc(PS5VK_OV_REG, sizeof(VkBufferImageCopy));
    if (!ps5vk_ov.shadow || !ps5vk_ov.cov || !ps5vk_ov.reg) { TR("vk: overlay unavailable: out of memory"); return 0; }
    ps5vk_ov.failed = 0; ps5vk_ov.ready = 1;
    TR("vk: overlay ready (%ux%u, %s, staging %zu MB in memory type %d)", ps5vk_ov.w, ps5vk_ov.h, ps5vk_ov.bgr ? "BGRA" : "RGBA", bytes >> 20, mt);
    return 1;
}
static VkResult VKAPI_CALL ps5vk_qp_main(VkQueue q, const VkPresentInfoKHR *pi) {
    // the same conditions as the GL path (glfwSwapBuffers): never in gameplay (cursor disabled), the cursor only after the pad moved it, the keyboard whenever it is open
    const int menu = g_polled && g_cursor_mode != GLFW_CURSOR_DISABLED && (g_cur_vis || g_osk_open), hud = g_hud_on && !g_kit_ui;   // the stats overlay (launcher setting) is drawn in gameplay too
    if (g_zk || ps5vk_ov.failed || !ps5vk_ov.usage_ok || !pi || pi->swapchainCount != 1 || pi->waitSemaphoreCount > 8 || (!menu && !hud))
        return ps5vk_real_qp(q, pi);
    if (!ps5vk_ov.ready && !ps5vk_ov_init()) return ps5vk_real_qp(q, pi);
    VkDevice d = ps5vk_ov.dev;
    g_ov_n = 0; if (menu) { if (g_osk_open) osk_build(); cursor_build(); }
    if (hud) hud_build();
    if (g_ov_n == 0) return ps5vk_real_qp(q, pi);
    if (pi->pSwapchains[0] != ps5vk_ov.sc) {
        ps5vk_ov.nimg = 8; if (ps5vk_ov.GetImages(d, pi->pSwapchains[0], &ps5vk_ov.nimg, ps5vk_ov.img) < 0) { ps5vk_ov.nimg = 0; return ps5vk_real_qp(q, pi); }
        ps5vk_ov.sc = pi->pSwapchains[0];
    }
    uint32_t ix = pi->pImageIndices[0]; if (ix >= ps5vk_ov.nimg) return ps5vk_real_qp(q, pi);
    if (ps5vk_ov.fence_armed) {   // the previous overlay copy must be done before the staging buffer and the command buffer are touched again
        if (ps5vk_ov.Wait(d, 1, &ps5vk_ov.fence, VK_TRUE, 1000000000ull) != VK_SUCCESS) { TR("vk: overlay: the previous copy did not finish in 1 s: overlay off"); ps5vk_ov.failed = 1; return ps5vk_real_qp(q, pi); }
        ps5vk_ov.ResetFences(d, 1, &ps5vk_ov.fence); ps5vk_ov.fence_armed = 0;
    }
    unsigned long hs = 1469598103934665603UL;
    for (int i = 0; i < g_ov_n; i++) { const unsigned char *b = (const unsigned char *)&g_ov[i]; for (size_t k = 0; k < sizeof(OvV); k++) { hs ^= b[k]; hs *= 1099511628211UL; } }
    if (!ps5vk_ov.have_hash || hs != ps5vk_ov.hash) { ps5vk_ov.nreg = ps5vk_ov_raster(); ps5vk_ov.hash = hs; ps5vk_ov.have_hash = 1; }
    if (ps5vk_ov.nreg <= 0) return ps5vk_real_qp(q, pi);
    VkImage im = ps5vk_ov.img[ix];
    VkCommandBufferBeginInfo bi; memset(&bi, 0, sizeof bi); bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ps5vk_ov.Reset(ps5vk_ov.cb, 0);
    if (ps5vk_ov.Begin(ps5vk_ov.cb, &bi) != VK_SUCCESS) return ps5vk_real_qp(q, pi);
    VkImageMemoryBarrier mb; memset(&mb, 0, sizeof mb); mb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; mb.image = im;
    mb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; mb.subresourceRange.levelCount = 1; mb.subresourceRange.layerCount = 1;
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; mb.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; mb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ps5vk_ov.Barrier(ps5vk_ov.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &mb);
    ps5vk_ov.Copy(ps5vk_ov.cb, ps5vk_ov.buf, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)ps5vk_ov.nreg, ps5vk_ov.reg);
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; mb.dstAccessMask = 0; mb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; mb.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    ps5vk_ov.Barrier(ps5vk_ov.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, 0, 0, 0, 1, &mb);
    if (ps5vk_ov.End(ps5vk_ov.cb) != VK_SUCCESS) return ps5vk_real_qp(q, pi);
    VkPipelineStageFlags ws[8]; for (uint32_t i = 0; i < pi->waitSemaphoreCount; i++) ws[i] = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si; memset(&si, 0, sizeof si); si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.waitSemaphoreCount = pi->waitSemaphoreCount; si.pWaitSemaphores = pi->pWaitSemaphores; si.pWaitDstStageMask = ws;
    si.commandBufferCount = 1; si.pCommandBuffers = &ps5vk_ov.cb; si.signalSemaphoreCount = 1; si.pSignalSemaphores = &ps5vk_ov.sem;
    VkResult r = ps5vk_ov.Submit(q, 1, &si, ps5vk_ov.fence);
    if (r != VK_SUCCESS) { static int el; if (el++ < 3) TR("vk: overlay: vkQueueSubmit %d: overlay off", (int)r); ps5vk_ov.failed = 1; return ps5vk_real_qp(q, pi); }
    ps5vk_ov.fence_armed = 1;
    VkPresentInfoKHR p2 = *pi; p2.waitSemaphoreCount = 1; p2.pWaitSemaphores = &ps5vk_ov.sem;
    { static int first; if (!first++) TR("vk: overlay: first frame drawn (%d rectangles -> %d copy regions, cursor %d keyboard %d stats %d)", g_ov_n / 6, ps5vk_ov.nreg, g_cur_vis, g_osk_open, hud); }
    ps5vk_ov.drawn++;
    return ps5vk_real_qp(q, &p2);
}

// Frame-time probe (the Vulkan run has no other stutter evidence): the time between two presents. Every 600 frames one line (mean / worst / how many frames took more than 2.5 x the mean and over 25 ms), and each such hitch
// on its own line (60 per run) in the order of the log, so the "[jvm] GC(n) Pause ..." and "Safepoint" lines next to it show whether a collector pause, a shader compile or the present itself was the stall.
static void ps5vk_ft_note(void) {
    static double last, sum, worst; static int n, hitch, shown; double t = now_s();
    if (last > 0 && !g_zk) {
        double dt = (t - last) * 1000.0; n++; sum += dt; if (dt > worst) worst = dt;
        double mean = sum / n;
        if (n > 120 && dt > 25.0 && dt > 2.5 * mean) { hitch++; if (shown < 60) { shown++; TR("vk: hitch %.1f ms (mean %.1f ms)", dt, mean); } }
        if (n >= 600) { TR("vk: frame time: mean %.1f ms worst %.1f ms, %d hitches over 600 frames", mean, worst, hitch); n = 0; sum = 0; worst = 0; hitch = 0; }
    }
    last = t;
}
static VkResult VKAPI_CALL ps5vk_qp(VkQueue q, const VkPresentInfoKHR *pi) {
    VkResult r = ps5vk_qp_main(q, pi);
    ps5vk_ft_note();
    return r;
}
// Zink's swapchain, as the driver sees it: the hand-over to a Vulkan game needs it gone. Mesa's EGL teardown does not destroy it (log 92330f8f: eglDestroySurface / eglDestroyContext / eglTerminate all succeeded and the next
// vkCreateSwapchainKHR still got VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: wsi_common_videoout.c keeps ONE current swapchain (out->current) and refuses another until it is destroyed).
static VkDevice zk_sc_dev; static VkSwapchainKHR zk_sc_handle; static PFN_vkDestroySwapchainKHR ps5vk_real_dsc;
static void VKAPI_CALL ps5vk_dsc(VkDevice d, VkSwapchainKHR sc, const VkAllocationCallbacks *a) {
    if (sc && sc == zk_sc_handle) zk_sc_handle = 0;
    ps5vk_real_dsc(d, sc, a);
}
static VkResult VKAPI_CALL ps5vk_csc(VkDevice d, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *a, VkSwapchainKHR *sc) {
    VkSwapchainCreateInfoKHR c2 = *ci;
    int vs = ci->presentMode == VK_PRESENT_MODE_FIFO_KHR || ci->presentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    TR("vk: vkCreateSwapchainKHR presentMode=%s minImageCount=%u extent=%ux%u format=%d -> VSync %s", ps5vk_pmname((int)ci->presentMode), ci->minImageCount, ci->imageExtent.width, ci->imageExtent.height, (int)ci->imageFormat, vs ? "on" : "off (tearing flips)");
    g_vsync = vs; c2.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    c2.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;   // the overlay copies into the swapchain image
    VkResult r = ps5vk_real_csc(d, &c2, a, sc);
    if (r == VK_SUCCESS && g_zk) { zk_sc_dev = d; zk_sc_handle = *sc; }
    if (r != VK_SUCCESS && !(ci->imageUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { TR("vk: the swapchain refused TRANSFER_DST (%d): created as the game asked, no cursor / keyboard overlay", (int)r); c2.imageUsage = ci->imageUsage; ps5vk_ov.usage_ok = 0; return ps5vk_real_csc(d, &c2, a, sc); }
    if (r == VK_SUCCESS) {
        ps5vk_ov.usage_ok = 1; ps5vk_ov.sc = 0; ps5vk_ov.nimg = 0; ps5vk_ov.bgr = !(ci->imageFormat == 37 || ci->imageFormat == 43);   // RGBA8 UNORM / SRGB; everything else is taken as BGRA (44, the console's)
        uint32_t nw = ci->imageExtent.width, nh = ci->imageExtent.height;
        if (!ps5vk_ov.ready) { ps5vk_ov.dev = d; ps5vk_ov.w = nw; ps5vk_ov.h = nh; }
        else if ((size_t)nw * nh * 4 <= ps5vk_ov.bytes) { ps5vk_ov.w = nw; ps5vk_ov.h = nh; ps5vk_ov.have_hash = 0; }   // a smaller swapchain (resize): same buffers, new row length
        else { ps5vk_ov.usage_ok = 0; TR("vk: overlay: the swapchain grew to %ux%u after the overlay was set up: overlay off", nw, nh); }
    }
    return r;
}
// The console's flip mode: 1 = vsync, 2 = tearing (ps5lwjgl.c UNCAP, same numbers). The driver presents through sceAgcDcbSetFlip and waits for a free image with sceVideoOutWaitVblank.
unsigned *__real_sceAgcDcbSetFlip(void *cb, unsigned h, int idx, unsigned mode, long marker);
int __real_sceVideoOutWaitVblank(int h);
static int ps5vk_nouncap(void) { static int v = -1; if (v < 0) v = getenv("PS5_NOUNCAP") != 0; return v; }
static volatile unsigned long ps5vk_nflip, ps5vk_nwait, ps5vk_nwait_skipped; static double ps5vk_t0, ps5vk_last, ps5vk_maxgap; static unsigned long ps5vk_gap[5];   // gap buckets: >16.7 ms, >25, >33, >50, >100
// one line for the first flips and a rate line every 5 s: what the display is really given, whatever the game's own counter says; plus the stutter numbers (gaps between two flips)
// Refresh rate 60 / 120, changeable at any time (ps5_set_hz, the launcher's "Refresh rate" row): the driver configures the console's output mode once, on the VideoOut handle it opens (selector 15 =
// 119.88 Hz, 1 = the default 59.94 Hz). These wrappers learn that handle and mode, and the next flip after a change re-configures the SAME handle to the wanted mode (real output switch, no restart,
// no swapchain rebuild; the console's own games do it this way). If the console refuses the switch the frame limiter below takes over (59.94 presents a second on the 120 Hz output).
int __real_sceVideoOutWaitVblank(int h);
int __real_sceVideoOutOpen(int user, int type, int idx, const void *p);
int __real_sceVideoOutClose(int h);
int __real_sceVideoOutConfigureOutput(int h, int mode, const void *a, const void *b, const void *c);
static volatile int g_vo_handle = -1, g_vo_mode, g_vo_fail;   // handle the driver opened; output mode it configured last (15 = 119.88 Hz, 1 = 59.94 Hz, 0 = never configured: the console refused 120)
int __wrap_sceVideoOutOpen(int u, int t, int i, const void *p) { int r = __real_sceVideoOutOpen(u, t, i, p); if (r >= 0) { g_vo_handle = r; g_vo_mode = 0; g_vo_fail = 0; TR("vo: output opened, handle %d", r); } return r; }
int __wrap_sceVideoOutClose(int h) { if (h == g_vo_handle) { g_vo_handle = -1; g_vo_mode = 0; } return __real_sceVideoOutClose(h); }
int __wrap_sceVideoOutConfigureOutput(int h, int mode, const void *a, const void *b, const void *c) {
    int r = __real_sceVideoOutConfigureOutput(h, mode, a, b, c); if (r == 0 && h == g_vo_handle) g_vo_mode = mode; return r; }
static void ps5vk_hz_apply(void) {
    if (g_use_gl && !g_zk) return;   // the OpenGL SDK owns its output (eglSetDisplayRefreshPS5); there the limiter does the 60
    int h = g_vo_handle, cur = g_vo_mode, want = g_hz >= 120 ? 15 : 1;
    if (h < 0 || !cur || cur == want || g_vo_fail) return;
    int r = __real_sceVideoOutConfigureOutput(h, want, 0, 0, 0);
    if (r != 0) { g_vo_fail = 1; TR("display: the console refused the switch to %d Hz (0x%x): the setting works as a frame limiter instead", want == 15 ? 120 : 60, (unsigned)r); return; }
    g_vo_mode = want; g_hz_real = want == 15 ? 120 : 60;
    { double t0 = now_s(); int ok = 0; for (int i = 0; i < 4; i++) ok += __real_sceVideoOutWaitVblank(h) == 0; TR("display: output switched to %d Hz at run time (4 vblanks in %.1f ms, ok %d)", want == 15 ? 120 : 60, (now_s() - t0) * 1e3, ok); }
}
static void ps5vk_hz_limit(void) {
    ps5vk_hz_apply();
    if (g_hz >= 120 || g_vo_mode == 1) return;   // the output itself runs at 59.94 Hz: nothing to limit
    static double next; const double P = 1.0 / 59.94; double t = now_s();
    if (next < t - P) next = t;   // behind (loading screen, hitch): do not try to catch up
    else { double w = next - t - 0.0004; if (w > 0) { struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = (long)(w * 1e9); nanosleep(&ts, 0); } while (now_s() < next) { } }
    next += P;
}
static void ps5vk_flip_stat(const char *fn, unsigned in, unsigned out, int idx, int un) {
    ps5vk_hz_limit();
    static unsigned long shown; unsigned long n = ++ps5vk_nflip;
    if (shown < 3) { shown++; TR("vk: flip #%lu: %s(mode %u -> %u, buf %d) vsync=%d uncap=%d", shown, fn, in, out, idx, g_vsync, un); }
    double t = now_s(); if (!ps5vk_t0) ps5vk_t0 = t;
    if (ps5vk_last) { double g = (t - ps5vk_last) * 1000.0; if (g > ps5vk_maxgap) ps5vk_maxgap = g; if (g > 100) ps5vk_gap[4]++; else if (g > 50) ps5vk_gap[3]++; else if (g > 33.4) ps5vk_gap[2]++; else if (g > 25) ps5vk_gap[1]++; else if (g > 16.7) ps5vk_gap[0]++; }
    ps5vk_last = t;
    if (t - ps5vk_t0 >= 5.0) {
        TR("vk: %lu flips in %.1f s = %.1f/s (waits %lu, skipped %lu; vsync=%d uncap=%d) | gaps between flips: max %.0f ms, >16.7ms %lu, >25ms %lu, >33ms %lu, >50ms %lu, >100ms %lu", n, t - ps5vk_t0, n / (t - ps5vk_t0), (unsigned long)ps5vk_nwait, (unsigned long)ps5vk_nwait_skipped, g_vsync, un, ps5vk_maxgap, ps5vk_gap[0], ps5vk_gap[1], ps5vk_gap[2], ps5vk_gap[3], ps5vk_gap[4]);
        ps5vk_t0 = t; ps5vk_nflip = 0; ps5vk_nwait = ps5vk_nwait_skipped = 0; ps5vk_maxgap = 0; memset(ps5vk_gap, 0, sizeof ps5vk_gap);
    }
}
// RADV's wsi_common_videoout.c presents with sceVideoOutSubmitFlip(handle, index, 1, arg) (the OpenGL SDK 1.0.x uses sceAgcDcbSetFlip instead): both get the tearing mode when VSync is off.
int __real_sceVideoOutSubmitFlip(int h, int idx, unsigned mode, long arg);
int __wrap_sceVideoOutSubmitFlip(int h, int idx, unsigned mode, long arg) {
    int un = !g_vsync && !g_uncap_fail && !ps5vk_nouncap();
    if (un && mode == 1) {
        LPB(0); int r2 = __real_sceVideoOutSubmitFlip(h, idx, 2, arg); LPA(0); static int rn; if (rn++ < 5) TR("uncap: SubmitFlip in the tearing mode returned 0x%x", r2);
        if (r2 == 0) { ps5vk_flip_stat("SubmitFlip", 1, 2, idx, un); return 0; }
        g_uncap_fail = 1; TR("uncap: the console refused the tearing flip mode (0x%x); falling back to vsynced flips", r2);
    }
    ps5vk_flip_stat("SubmitFlip", mode, mode, idx, 0);
    { LPB(0); int fr_ = __real_sceVideoOutSubmitFlip(h, idx, mode, arg); LPA(0); return fr_; } }
unsigned *__wrap_sceAgcDcbSetFlip(void *cb, unsigned h, int idx, unsigned mode, long marker) {
    unsigned in = mode; int un = !g_vsync && !g_uncap_fail && !ps5vk_nouncap();
    if (un && mode == 1) mode = 2;
    ps5vk_flip_stat("DcbSetFlip", in, mode, idx, un);
    return __real_sceAgcDcbSetFlip(cb, h, idx, mode, marker); }
int __wrap_sceVideoOutWaitVblank(int h) {
    ps5vk_nwait++;
    if (!g_vsync && !g_uncap_fail && !ps5vk_nouncap()) { ps5vk_nwait_skipped++; return 0; }   // uncapped: do not wait for the display
    return __real_sceVideoOutWaitVblank(h); }
static PFN_vkVoidFunction VKAPI_CALL ps5vk_gdpa(VkDevice d, const char *n) {
    PFN_vkVoidFunction f = ps5vk_real_gdpa(d, n);
    if (f && !strcmp(n, "vkCreateSwapchainKHR")) { ps5vk_real_csc = (PFN_vkCreateSwapchainKHR)f; return (PFN_vkVoidFunction)ps5vk_csc; }
    if (f && !strcmp(n, "vkQueuePresentKHR")) { ps5vk_real_qp = (PFN_vkQueuePresentKHR)f; return (PFN_vkVoidFunction)ps5vk_qp; }
    if (f && !strcmp(n, "vkDestroySwapchainKHR")) { ps5vk_real_dsc = (PFN_vkDestroySwapchainKHR)f; return (PFN_vkVoidFunction)ps5vk_dsc; }
    return f;
}
// The real libvulkan also answers vkGetDeviceProcAddr for a NULL instance (Mesa's Zink asks before it has an instance); the driver's answer needs an instance, so use the latest one the caller has shown.
static VkInstance ps5vk_last_inst;
static PFN_vkVoidFunction VKAPI_CALL ps5vk_gdpa_boot(VkDevice d, const char *n) {
    if (!ps5vk_real_gdpa && ps5vk_last_inst) ps5vk_real_gdpa = (PFN_vkGetDeviceProcAddr)vk_icdGetInstanceProcAddr(ps5vk_last_inst, "vkGetDeviceProcAddr");
    return ps5vk_real_gdpa ? ps5vk_gdpa(d, n) : 0;
}
static PFN_vkVoidFunction ps5vk_gipa(VkInstance i, const char *n) {
    if (i) ps5vk_last_inst = i;
    PFN_vkVoidFunction f = vk_icdGetInstanceProcAddr(i, n);
    if (!f && !strcmp(n, "vkGetDeviceProcAddr")) { TR("vk: gipa(%p, vkGetDeviceProcAddr): answered by the shim", (void *)i); return (PFN_vkVoidFunction)ps5vk_gdpa_boot; }
    static int ln, lnull, lprobe;
    if (!strcmp(n, "vkEnumerateInstanceLayerProperties")) {
        if (!lprobe++) {
            if (f) { uint32_t c = 0; VkResult r = ((PFN_vkEnumerateInstanceLayerProperties)f)(&c, 0); TR("vk: the driver's vkEnumerateInstanceLayerProperties -> %d (%u layers); the shim answers it instead", (int)r, c); }
            else TR("vk: the driver has no vkEnumerateInstanceLayerProperties; the shim answers it");
        }
        return (PFN_vkVoidFunction)ps5vk_layers;
    }
    if (f && !strcmp(n, "vkGetDeviceProcAddr")) { ps5vk_real_gdpa = (PFN_vkGetDeviceProcAddr)f; return (PFN_vkVoidFunction)ps5vk_gdpa; }
    if (f && !strcmp(n, "vkQueuePresentKHR")) { ps5vk_real_qp = (PFN_vkQueuePresentKHR)f; return (PFN_vkVoidFunction)ps5vk_qp; }
    if (f && !strcmp(n, "vkDestroySwapchainKHR")) { ps5vk_real_dsc = (PFN_vkDestroySwapchainKHR)f; return (PFN_vkVoidFunction)ps5vk_dsc; }
    if (f && !strcmp(n, "vkGetPhysicalDeviceSurfacePresentModesKHR")) { ps5vk_real_pm = (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)f; return (PFN_vkVoidFunction)ps5vk_pm; }
    if (!i && ln++ < 12) TR("vk: gipa(NULL, %s) = %p", n, (void *)f);
    else if (!f && lnull++ < 40) TR("vk: gipa(%p, %s) = NULL", (void *)i, n);
    return f;
}

static void *ps5vk_create_window(int w, int h) {
    if (sceSystemServiceHideSplashScreen) sceSystemServiceHideSplashScreen();
    // The game reads the window size right after it makes the window, but the display surface (and with it the real size) only exists once the game asks for its Vulkan surface: a game that does that later
    // (VulkanMod on GLFW) had a 1920x1080 window on a 2560x1440 display = cursor off the real spot and a stretched, torn menu. The launcher's resolution (PS5_RES_H) is applied here already, when the
    // display really offers that width (<root>/.display_modes, written by the first surface of the launcher); the surface picks the same mode.
    { const char *rh = getenv("PS5_RES_H"); int hh = rh ? atoi(rh) : 0, ww = hh == 1440 ? 2560 : hh == 2160 ? 3840 : 0;
      if (ww) { char wl[160]; wl[0] = 0; FILE *mf = fopen("/app0/mc/.display_modes", "r"); if (mf) { size_t nr = fread(wl, 1, sizeof wl - 1, mf); wl[nr] = 0; fclose(mf); } char t[16]; snprintf(t, sizeof t, "%d", ww);
        if (wl[0] && strstr(wl, t)) { g_w = ww; g_h = hh; TR("vk: window size %dx%d from the launcher resolution (the display offers it)", g_w, g_h); } } }
    g_cx = g_w / 2.0; g_cy = g_h / 2.0; g_created = 1;
    TR("vk: createWindow %dx%d requested, the window is the display (%dx%d)", w, h, g_w, g_h);
    return WIN;
}

int SDL_Vulkan_LoadLibrary(const char *p) { TR("vk: SDL_Vulkan_LoadLibrary(%s)", p ? p : "(default)"); return 1; }
void SDL_Vulkan_UnloadLibrary(void) {}
void *SDL_Vulkan_GetVkGetInstanceProcAddr(void) { return (void *)ps5vk_gipa; }
static const char *const ps5vk_exts[] = { "VK_KHR_surface", "VK_KHR_display" };
const char *const *SDL_Vulkan_GetInstanceExtensions(unsigned *n) { if (n) *n = 2; return ps5vk_exts; }
int SDL_Vulkan_GetPresentationSupport(void *inst, void *pd, unsigned q) { (void)inst; (void)pd; return q == 0; }   // queue family 0 = graphics + compute + transfer
int SDL_Vulkan_CreateSurface(void *win, void *instance, const void *alloc, void *out) {
    (void)win; (void)alloc;
    VkInstance inst = (VkInstance)instance;
#define I(f) PFN_##f f = (PFN_##f)vk_icdGetInstanceProcAddr(inst, #f); if (!f) { snprintf(sdl_err, sizeof sdl_err, "%s missing", #f); TR("vk: %s missing", #f); return 0; }
    I(vkEnumeratePhysicalDevices) I(vkGetPhysicalDeviceDisplayPropertiesKHR) I(vkGetPhysicalDeviceDisplayPlanePropertiesKHR) I(vkGetDisplayModePropertiesKHR) I(vkCreateDisplayPlaneSurfaceKHR)
#undef I
    uint32_t np = 1; VkPhysicalDevice pd;
    VkResult r = vkEnumeratePhysicalDevices(inst, &np, &pd); if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !np) { snprintf(sdl_err, sizeof sdl_err, "no physical device (%d)", (int)r); return 0; }
    ps5vk_inst = inst; ps5vk_pd = pd;
    uint32_t nd = 1; VkDisplayPropertiesKHR dp;
    r = vkGetPhysicalDeviceDisplayPropertiesKHR(pd, &nd, &dp); if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !nd) { snprintf(sdl_err, sizeof sdl_err, "no display (%d)", (int)r); return 0; }
    uint32_t nm = 0; vkGetDisplayModePropertiesKHR(pd, dp.display, &nm, 0);
    VkDisplayModePropertiesKHR *modes = (VkDisplayModePropertiesKHR *)malloc(sizeof *modes * (nm + 1)); vkGetDisplayModePropertiesKHR(pd, dp.display, &nm, modes);
    int pick = -1, pick_d = 0;
    // launcher.cfg resolution=1080|1440|2160 (jvm_host.cpp -> PS5_RES_H): the display mode of that width, else 1080p. The widths this console offers are written to <root>/.display_modes so that the launcher (McBoot) asks Minecraft for the same size.
    int want_w = 1920; { const char *rh = getenv("PS5_RES_H"); int h = rh ? atoi(rh) : 0; if (h == 1440) want_w = 2560; else if (h == 2160) want_w = 3840; }
    { int have = 0; for (uint32_t i = 0; i < nm; i++) if ((int)modes[i].parameters.visibleRegion.width == want_w) have = 1; if (!have) { if (want_w != 1920) TR("vk: no %d-wide display mode on this output: staying at 1080p", want_w); want_w = 1920; } }
    { char wl[120]; int wn = 0; wl[0] = 0; for (uint32_t i = 0; i < nm && wn < 100; i++) { int ww = (int)modes[i].parameters.visibleRegion.width, dup = 0; char t[16]; snprintf(t, sizeof t, "%d", ww); if (strstr(wl, t)) dup = 1; if (!dup) wn += snprintf(wl + wn, sizeof wl - wn, "%s%d", wn ? " " : "", ww); }
      FILE *mf = fopen("/app0/mc/.display_modes", "w"); if (mf) { fprintf(mf, "%s\n", wl); fclose(mf); } }
    for (uint32_t i = 0; i < nm; i++) {
        TR("vk: display mode %u %ux%u @ %.2f Hz", i, modes[i].parameters.visibleRegion.width, modes[i].parameters.visibleRegion.height, modes[i].parameters.refreshRate / 1000.0);
        if ((int)modes[i].parameters.visibleRegion.width == want_w) {   // 1080p mode closest to the wanted rate (launcher 'hz'; the driver lists 119.88 only when the title declares 120 Hz and the output allows it)
            int d = -(int)modes[i].parameters.refreshRate;   // the fastest mode: the 60 Hz setting is a frame limiter (ps5vk_hz_limit), so the rate can change at run time without a mode switch
            if (pick < 0 || d < pick_d) { pick = (int)i; pick_d = d; }
        }
    }
    if (pick < 0) pick = 0;
    g_hz_real = (int)(modes[pick].parameters.refreshRate / 1000.0 + 0.5);
    TR("vk: refresh setting %d Hz (frame limiter), fastest display mode %d picked (%.2f Hz)", g_hz, pick, modes[pick].parameters.refreshRate / 1000.0);
    VkExtent2D ext = modes[pick].parameters.visibleRegion; g_w = (int)ext.width; g_h = (int)ext.height;
    uint32_t npl = 0; vkGetPhysicalDeviceDisplayPlanePropertiesKHR(pd, &npl, 0);
    VkDisplayPlanePropertiesKHR pl[8]; if (npl > 8) npl = 8; vkGetPhysicalDeviceDisplayPlanePropertiesKHR(pd, &npl, pl);
    VkDisplaySurfaceCreateInfoKHR sci; memset(&sci, 0, sizeof sci); sci.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR;
    sci.displayMode = modes[pick].displayMode; sci.planeIndex = 0; sci.planeStackIndex = npl ? pl[0].currentStackIndex : 0;
    sci.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR; sci.globalAlpha = 1.0f; sci.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR; sci.imageExtent = ext;
    free(modes);
    r = vkCreateDisplayPlaneSurfaceKHR(inst, &sci, 0, (VkSurfaceKHR *)out);
    TR("vk: display surface %dx%d = %d", g_w, g_h, (int)r);
    if (r != VK_SUCCESS) { snprintf(sdl_err, sizeof sdl_err, "vkCreateDisplayPlaneSurfaceKHR failed (%d)", (int)r); return 0; }
    return 1;
}

// GLFW's Vulkan functions, for mods that bring their own Vulkan renderer on a GLFW window (VulkanMod on 26.1 and older): LWJGL's GLFWVulkan class looks them up in the "glfw" library (dlsym, ps5vk_glfw_sym).
// They share everything with the SDL_Vulkan_* shim above: the window is the display, the surface is the VK_KHR_display plane, queue family 0 is the only one that presents.
static int vkg_supported(void) { return !g_use_gl && (!g_zk || g_vk_defer); }   // a deferred hand-over: the game on Vulkan is already starting while the launcher's loading screen is still on Zink
static const char *const *vkg_req_exts(unsigned *n) { if (n) *n = 2; return ps5vk_exts; }
static void *vkg_gipa(void *inst, const char *n) { return (void *)ps5vk_gipa((VkInstance)inst, n); }
static int vkg_present(void *inst, void *pd, unsigned q) { (void)inst; (void)pd; return q == 0; }
static int vkg_surface(void *inst, void *win, const void *alloc, void *out) { int ok = SDL_Vulkan_CreateSurface(win, inst, alloc, out); TR("vk: glfwCreateWindowSurface -> %s", ok ? "ok" : "FAILED"); return ok ? 0 : -3; }   // VK_SUCCESS / VK_ERROR_INITIALIZATION_FAILED
static void vkg_loader(void *getInstanceProcAddr) { (void)getInstanceProcAddr; }   // GLFW 3.4 glfwInitVulkanLoader: LWJGL 3.4 lists it as REQUIRED ("A required function is missing: glfwInitVulkanLoader" ended the first VulkanMod start); the loader is the shim's own
static void *ps5vk_glfw_sym(const char *n) {
    if (!strcmp(n, "glfwInitVulkanLoader")) return (void *)vkg_loader;
    if (!strcmp(n, "glfwVulkanSupported")) return (void *)vkg_supported;
    if (!strcmp(n, "glfwGetRequiredInstanceExtensions")) return (void *)vkg_req_exts;
    if (!strcmp(n, "glfwGetInstanceProcAddress")) return (void *)vkg_gipa;
    if (!strcmp(n, "glfwGetPhysicalDevicePresentationSupport")) return (void *)vkg_present;
    if (!strcmp(n, "glfwCreateWindowSurface")) return (void *)vkg_surface;
    return 0;
}

#ifdef PS5_ZINK
// Mesa's Zink (zink.o, patched by patch_mesa_zink_ps5.py) asks for its presentation surface here: the same VK_KHR_display surface the native Vulkan path uses, made on Zink's own VkInstance.
int ps5_zink_create_display_surface(void *inst, unsigned long *out) {
    int ok = SDL_Vulkan_CreateSurface(WIN, inst, 0, out);
    TR("zink: display surface on instance %p -> %s (%#lx)", inst, ok ? "ok" : "FAILED", ok ? *out : 0UL);
    return ok ? 0 : -3;   // VK_SUCCESS / VK_ERROR_INITIALIZATION_FAILED
}
#endif

#ifdef PS5_ZINK
// PROBE (opt-in: <root>/.kit/vkprobe exists; kit_stage calls this when Play is pressed, then ends the app). Question it answers: can the native Vulkan game take the display after the launcher has run on Zink in the same process?
// Step A: with Zink's display surface still alive, make a second VkInstance + display surface + swapchain (what the game would do). Step B: destroy Zink's EGL surface first, then try again. Every result goes to the log ("vkh:").
// Nothing here is used in normal runs.
static int vkh_try(const char *tag) {
    PFN_vkCreateInstance ci = (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(0, "vkCreateInstance");
    if (!ci) { TR("vkh %s: vkCreateInstance missing", tag); return 0; }
    VkApplicationInfo ai; memset(&ai, 0, sizeof ai); ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO; ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici; memset(&ici, 0, sizeof ici); ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ici.pApplicationInfo = &ai; ici.enabledExtensionCount = 2; ici.ppEnabledExtensionNames = ps5vk_exts;
    VkInstance inst = 0; VkResult r = ci(&ici, 0, &inst); TR("vkh %s: vkCreateInstance = %d", tag, (int)r); if (r != VK_SUCCESS) return 0;
#define I(f) PFN_##f f = (PFN_##f)vk_icdGetInstanceProcAddr(inst, #f);
    I(vkDestroyInstance) I(vkDestroySurfaceKHR) I(vkEnumeratePhysicalDevices) I(vkCreateDevice) I(vkGetDeviceProcAddr) I(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) I(vkGetPhysicalDeviceSurfaceFormatsKHR)
#undef I
    unsigned long surf = 0; int ok = SDL_Vulkan_CreateSurface(0, inst, 0, &surf); TR("vkh %s: display surface = %s (%#lx)", tag, ok ? "ok" : "FAILED", surf);
    int good = 0;
    if (ok) {
        uint32_t np = 1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(inst, &np, &pd);
        float pr = 1.0f; VkDeviceQueueCreateInfo qi; memset(&qi, 0, sizeof qi); qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO; qi.queueCount = 1; qi.pQueuePriorities = &pr;
        const char *dx[1] = { "VK_KHR_swapchain" };
        VkDeviceCreateInfo dci; memset(&dci, 0, sizeof dci); dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qi; dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = dx;
        VkDevice dev = 0; r = vkCreateDevice(pd, &dci, 0, &dev); TR("vkh %s: vkCreateDevice = %d", tag, (int)r);
        if (r == VK_SUCCESS) {
            PFN_vkCreateSwapchainKHR csw = (PFN_vkCreateSwapchainKHR)vkGetDeviceProcAddr(dev, "vkCreateSwapchainKHR"); PFN_vkDestroySwapchainKHR dsw = (PFN_vkDestroySwapchainKHR)vkGetDeviceProcAddr(dev, "vkDestroySwapchainKHR");
            PFN_vkDestroyDevice dd = (PFN_vkDestroyDevice)vkGetDeviceProcAddr(dev, "vkDestroyDevice");
            VkSurfaceCapabilitiesKHR cap; memset(&cap, 0, sizeof cap); r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, (VkSurfaceKHR)surf, &cap);
            uint32_t nf = 1; VkSurfaceFormatKHR sf; memset(&sf, 0, sizeof sf); vkGetPhysicalDeviceSurfaceFormatsKHR(pd, (VkSurfaceKHR)surf, &nf, &sf);
            TR("vkh %s: surface caps rc %d min images %u extent %ux%u format %d", tag, (int)r, cap.minImageCount, cap.currentExtent.width, cap.currentExtent.height, (int)sf.format);
            VkSwapchainCreateInfoKHR sc; memset(&sc, 0, sizeof sc); sc.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR; sc.surface = (VkSurfaceKHR)surf; sc.minImageCount = cap.minImageCount > 3 ? cap.minImageCount : 3; sc.imageFormat = sf.format; sc.imageColorSpace = sf.colorSpace;
            sc.imageExtent = cap.currentExtent.width ? cap.currentExtent : (VkExtent2D){ (uint32_t)g_w, (uint32_t)g_h }; sc.imageArrayLayers = 1; sc.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; sc.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
            sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; sc.presentMode = VK_PRESENT_MODE_FIFO_KHR; sc.clipped = VK_TRUE;
            VkSwapchainKHR sw = 0; r = csw(dev, &sc, 0, &sw); TR("vkh %s: vkCreateSwapchainKHR = %d  (0 = the game could take the display like this)", tag, (int)r); good = r == VK_SUCCESS;
            if (sw) dsw(dev, sw, 0);
            dd(dev, 0);
        }
        vkDestroySurfaceKHR(inst, (VkSurfaceKHR)surf, 0);
    }
    vkDestroyInstance(inst, 0);
    return good;
}
/* The probe of log ac0eeea2 called the GL SDK's eglDestroySurface on Mesa's surface (it answered 0 = failed) and the Zink swapchain stayed: vkCreateSwapchainKHR = VK_ERROR_NATIVE_WINDOW_IN_USE_KHR.
   Mesa's own eglTerminate releases every surface and context of its display (and with them Zink's swapchain). */
extern unsigned zink_eglTerminate(void *); extern int zink_eglGetError(void);
extern unsigned zink_eglDestroySurface(void *, void *) __attribute__((weak)); extern unsigned zink_eglDestroyContext(void *, void *) __attribute__((weak)); extern unsigned zink_eglReleaseThread(void) __attribute__((weak));   // zink.o renames the whole EGL API (make_zink_obj.sh), these just were never declared
extern volatile int g_gl_down;
static int zink_release_display(void) {
    if (!g_dpy) return 0;
    if (kit_boot_stop) { TR("vkswitch: ending the launcher's loading screen first (its thread was still drawing on Zink: log adf09874)"); kit_boot_stop(); }   // it destroys its GL objects and releases the context
    g_gl_down = 1; usleep(100000);   // swap threads stop touching Mesa's display (log adf09874: ~900 eglSwapBuffers on the terminated display)
    // log 2c480f0a: eglMakeCurrent(none) + eglTerminate answered 1 (success) and the Zink swapchain still held the display (VK_ERROR_NATIVE_WINDOW_IN_USE_KHR). Take the surface and the context down explicitly first,
    // as the EGL spec's order has it (nothing current, destroy, terminate, release the thread).
    unsigned mc = eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    unsigned ds = g_surf && zink_eglDestroySurface ? zink_eglDestroySurface((void *)g_dpy, (void *)g_surf) : 0, dc = g_ctx && zink_eglDestroyContext ? zink_eglDestroyContext((void *)g_dpy, (void *)g_ctx) : 0;
    TR("vkswitch: Mesa eglMakeCurrent(none) = %u, eglDestroySurface = %u, eglDestroyContext = %u (egl error %x)", mc, ds, dc, zink_eglGetError());
    unsigned t = zink_eglTerminate((void *)g_dpy); if (zink_eglReleaseThread) zink_eglReleaseThread();
    TR("vkswitch: Mesa eglTerminate = %u (egl error %x)", t, zink_eglGetError());
    g_surf = EGL_NO_SURFACE; g_ctx = EGL_NO_CONTEXT; g_dpy = 0;
    usleep(500000);
    return (int)t;
}
void ps5_vk_handover_probe(void) {
    TR("vkh: handover probe starts (Zink display surface %p still alive)", (void *)g_surf);
    int a = vkh_try("A (Zink surface alive)");
    if (!a && g_dpy) { zink_release_display(); int b = vkh_try("B (Mesa EGL terminated)"); TR("vkh: result: A=%d B=%d", a, b); }
    else TR("vkh: result: A=%d", a);
}
// McBoot (Hello.quit("vkswitch")): the launcher ran on Zink and the game wants Vulkan. Release Mesa's display, prove that a Vulkan swapchain can be made, and from then on this process is a plain Vulkan one (the next glfwCreateWindow makes the
// Vulkan window). Returns 0 when it did not work: the GL side is gone then, McBoot ends the app the old way (the next start is a Vulkan start).
int ps5_vk_switch(void) {
    if (!g_zk) return 0;
    TR("vkswitch: begin (Zink display surface %p)", (void *)g_surf);
    zink_release_display();
    if (zk_sc_handle && ps5vk_real_gdpa && ps5vk_real_dsc) {   // Zink's swapchain is still the driver's current one: destroy it ourselves (Zink's context and device no longer use it)
        PFN_vkDeviceWaitIdle wi = (PFN_vkDeviceWaitIdle)ps5vk_real_gdpa(zk_sc_dev, "vkDeviceWaitIdle");
        TR("vkswitch: Zink's swapchain %p is still alive after the EGL teardown: waiting for its device, then destroying it", (void *)zk_sc_handle);
        if (wi) wi(zk_sc_dev);
        VkSwapchainKHR sc = zk_sc_handle; zk_sc_handle = 0; ps5vk_real_dsc(zk_sc_dev, sc, 0);
        usleep(300000);
    } else TR("vkswitch: no live Zink swapchain recorded (handle %p, gdpa %d, dsc %d)", (void *)zk_sc_handle, ps5vk_real_gdpa != 0, ps5vk_real_dsc != 0);
    g_zk = 0; g_use_gl = 0; g_created = 0; g_gl_down = 0;
    int ok = 0;   // Zink may give its swapchain back a little later (its resources are freed when the last GPU work that uses them has finished): try for a few seconds
    for (int i = 0; i < 8 && !ok; i++) { if (i) usleep(1000000); ok = vkh_try(i ? "vkswitch (again)" : "vkswitch"); }
    TR("vkswitch: a Vulkan swapchain %s", ok ? "can be made: the game takes the display" : "could NOT be made");
    return ok;
}
#endif
