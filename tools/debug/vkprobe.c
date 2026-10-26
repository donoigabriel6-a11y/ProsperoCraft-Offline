/* vkprobe.c: native Vulkan probe for the RADV title (make_vk_title.sh). Instance, device report, display mode, swapchain, colour-cycling clear + present, GPU memory allocations.
 * Everything is logged through logf (UDP in jvm_host.cpp). Returns 0 when the frames were presented.
 * RADV is linked into the title: every command comes from vk_icdGetInstanceProcAddr. */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance, const char *);
extern int sceSystemServiceHideSplashScreen(void) __attribute__((weak));

static void (*L)(const char *);
static void lg(const char *fmt, ...) { char b[400]; va_list v; va_start(v, fmt); vsnprintf(b, sizeof b, fmt, v); va_end(v); L(b); }
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { lg("vk FAIL %s = %d", #x, (int)r_); goto fail; } } while (0)
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static void list_names(const char *what, int n, const char *(*name)(const void *, int), const void *arr) {
    char line[300]; line[0] = 0; int cnt = 0;
    for (int i = 0; i < n; i++) {
        const char *s = name(arr, i);
        if (strlen(line) + strlen(s) + 2 > 230) { lg("%s:%s", what, line); line[0] = 0; }
        strcat(line, " "); strcat(line, s); cnt++;
    }
    if (line[0]) lg("%s:%s", what, line);
    (void)cnt;
}
static const char *ext_name(const void *a, int i) { return ((const VkExtensionProperties *)a)[i].extensionName; }

int vkprobe_run(const char *tag, void (*logf)(const char *), int secs)
{
    L = logf;
    static int splash_done;
    if (!splash_done && sceSystemServiceHideSplashScreen) { sceSystemServiceHideSplashScreen(); splash_done = 1; }
    lg("vk[%s]: begin", tag);
    int result = 1;
    VkInstance inst = VK_NULL_HANDLE; VkSurfaceKHR surf = VK_NULL_HANDLE; VkDevice dev = VK_NULL_HANDLE; VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE;
    VkSemaphore *acq = 0, *rend = 0; VkImage *imgs = 0; VkCommandBuffer cb = VK_NULL_HANDLE; uint32_t nimg = 0;
    VkExtensionProperties *iex = 0, *dex = 0; VkDisplayModePropertiesKHR *modes = 0; VkSurfaceFormatKHR *fmts = 0;

#define GI(n) PFN_##n n = (PFN_##n)vk_icdGetInstanceProcAddr(NULL, #n)
    GI(vkCreateInstance); GI(vkEnumerateInstanceExtensionProperties); GI(vkEnumerateInstanceVersion);
    uint32_t ver = 0; if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&ver);
    lg("vk: instance version %u.%u.%u", VK_API_VERSION_MAJOR(ver), VK_API_VERSION_MINOR(ver), VK_API_VERSION_PATCH(ver));
    uint32_t nie = 0; CK(vkEnumerateInstanceExtensionProperties(0, &nie, 0));
    iex = malloc(sizeof *iex * (nie + 1)); CK(vkEnumerateInstanceExtensionProperties(0, &nie, iex));
    list_names("vk: instance extensions", (int)nie, ext_name, iex);

    const char *ie[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME };
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "ps5-vkprobe", 1, "none", 1, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &ai, 0, 0, 2, ie };
    CK(vkCreateInstance(&ici, 0, &inst));
#define I(n) PFN_##n n = (PFN_##n)vk_icdGetInstanceProcAddr(inst, #n)
    I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties); I(vkGetPhysicalDeviceFeatures2); I(vkGetPhysicalDeviceMemoryProperties);
    I(vkGetPhysicalDeviceQueueFamilyProperties); I(vkEnumerateDeviceExtensionProperties); I(vkCreateDevice); I(vkGetDeviceProcAddr); I(vkDestroyInstance);
    I(vkGetPhysicalDeviceDisplayPropertiesKHR); I(vkGetPhysicalDeviceDisplayPlanePropertiesKHR); I(vkGetDisplayModePropertiesKHR);
    I(vkCreateDisplayPlaneSurfaceKHR); I(vkDestroySurfaceKHR); I(vkGetPhysicalDeviceSurfaceSupportKHR); I(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    I(vkGetPhysicalDeviceSurfaceFormatsKHR);

    uint32_t npd = 0; CK(vkEnumeratePhysicalDevices(inst, &npd, 0));
    if (!npd) { lg("vk FAIL: no physical device"); goto fail; }
    VkPhysicalDevice pd; npd = 1; { VkResult r = vkEnumeratePhysicalDevices(inst, &npd, &pd); if (r != VK_SUCCESS && r != VK_INCOMPLETE) { lg("vk FAIL enumerate %d", (int)r); goto fail; } }
    VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(pd, &pr);
    lg("vk: device \"%s\" api %u.%u.%u driver 0x%x vendor 0x%x type %d", pr.deviceName, VK_API_VERSION_MAJOR(pr.apiVersion), VK_API_VERSION_MINOR(pr.apiVersion), VK_API_VERSION_PATCH(pr.apiVersion), pr.driverVersion, pr.vendorID, (int)pr.deviceType);
    lg("vk: limits maxImage2D %u maxAlloc %u maxBoundSets %u maxPushConst %u maxUBO %u", pr.limits.maxImageDimension2D, pr.limits.maxMemoryAllocationCount, pr.limits.maxBoundDescriptorSets, pr.limits.maxPushConstantsSize, pr.limits.maxUniformBufferRange);

    VkPhysicalDeviceVulkan11Features f11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f11 };
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &f12 };
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13 };
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    lg("vk: features timelineSem %d bufferDeviceAddr %d descIndexing(runtimeArray) %d dynRendering %d sync2 %d drawIndirectCount %d multiDraw %d geom %d tess %d int64 %d aniso %d fillNonSolid %d shaderDrawParams %d",
       f12.timelineSemaphore, f12.bufferDeviceAddress, f12.runtimeDescriptorArray, f13.dynamicRendering, f13.synchronization2, f12.drawIndirectCount, f2.features.multiDrawIndirect,
       f2.features.geometryShader, f2.features.tessellationShader, f2.features.shaderInt64, f2.features.samplerAnisotropy, f2.features.fillModeNonSolid, f11.shaderDrawParameters);

    uint32_t nde = 0; CK(vkEnumerateDeviceExtensionProperties(pd, 0, &nde, 0));
    dex = malloc(sizeof *dex * (nde + 1)); CK(vkEnumerateDeviceExtensionProperties(pd, 0, &nde, dex));
    lg("vk: %u device extensions", nde); list_names("vk: device extensions", (int)nde, ext_name, dex);

    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t localType = ~0u;
    for (uint32_t i = 0; i < mp.memoryHeapCount; i++) lg("vk: heap %u size %llu MB flags 0x%x", i, (unsigned long long)(mp.memoryHeaps[i].size >> 20), mp.memoryHeaps[i].flags);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) { lg("vk: memtype %u heap %u flags 0x%x", i, mp.memoryTypes[i].heapIndex, mp.memoryTypes[i].propertyFlags); if (localType == ~0u && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) localType = i; }

    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, 0);
    VkQueueFamilyProperties qf[16]; if (nq > 16) nq = 16; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    for (uint32_t i = 0; i < nq; i++) lg("vk: queue family %u flags 0x%x count %u", i, qf[i].queueFlags, qf[i].queueCount);

    /* display */
    uint32_t nd = 1; VkDisplayPropertiesKHR dp;
    { VkResult r = vkGetPhysicalDeviceDisplayPropertiesKHR(pd, &nd, &dp); if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !nd) { lg("vk FAIL display properties %d n=%u", (int)r, nd); goto fail; } }
    lg("vk: display \"%s\" physical %ux%u", dp.displayName ? dp.displayName : "?", dp.physicalResolution.width, dp.physicalResolution.height);
    uint32_t nm = 0; CK(vkGetDisplayModePropertiesKHR(pd, dp.display, &nm, 0));
    modes = malloc(sizeof *modes * (nm + 1)); CK(vkGetDisplayModePropertiesKHR(pd, dp.display, &nm, modes));
    int pick = 0;
    for (uint32_t i = 0; i < nm; i++) {
        lg("vk: mode %u %ux%u @ %.2f Hz", i, modes[i].parameters.visibleRegion.width, modes[i].parameters.visibleRegion.height, modes[i].parameters.refreshRate / 1000.0);
        if (!pick && modes[i].parameters.visibleRegion.width == 1920) pick = (int)i + 1;   /* the first 1080p mode = the fastest refresh at that size */
    }
    pick = pick ? pick - 1 : 0;
    VkExtent2D ext = modes[pick].parameters.visibleRegion;
    lg("vk: using mode %d %ux%u @ %.2f Hz", pick, ext.width, ext.height, modes[pick].parameters.refreshRate / 1000.0);
    uint32_t npl = 0; vkGetPhysicalDeviceDisplayPlanePropertiesKHR(pd, &npl, 0);
    VkDisplayPlanePropertiesKHR pl[8]; if (npl > 8) npl = 8; vkGetPhysicalDeviceDisplayPlanePropertiesKHR(pd, &npl, pl);
    lg("vk: %u display planes (plane 0 stack index %u)", npl, npl ? pl[0].currentStackIndex : 0);
    VkDisplaySurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR };
    sci.displayMode = modes[pick].displayMode; sci.planeIndex = 0; sci.planeStackIndex = npl ? pl[0].currentStackIndex : 0;
    sci.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR; sci.globalAlpha = 1.0f; sci.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR; sci.imageExtent = ext;
    CK(vkCreateDisplayPlaneSurfaceKHR(inst, &sci, 0, &surf));

    uint32_t qfi = ~0u;
    for (uint32_t i = 0; i < nq; i++) { VkBool32 ok = 0; vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surf, &ok); if (ok && (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { qfi = i; break; } }
    if (qfi == ~0u) { lg("vk FAIL: no graphics queue family presents to the display"); goto fail; }
    float prio = 1.0f; VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, qfi, 1, &prio };
    const char *de[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0, 1, &qci, 0, 0, 1, de, 0 };
    CK(vkCreateDevice(pd, &dci, 0, &dev));
#define D(n) PFN_##n n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    D(vkGetDeviceQueue); D(vkDeviceWaitIdle); D(vkDestroyDevice); D(vkCreateSwapchainKHR); D(vkDestroySwapchainKHR); D(vkGetSwapchainImagesKHR);
    D(vkAcquireNextImageKHR); D(vkQueuePresentKHR); D(vkQueueSubmit); D(vkCreateCommandPool); D(vkDestroyCommandPool); D(vkAllocateCommandBuffers);
    D(vkBeginCommandBuffer); D(vkEndCommandBuffer); D(vkResetCommandBuffer); D(vkCmdPipelineBarrier); D(vkCmdClearColorImage); D(vkCreateSemaphore); D(vkDestroySemaphore);
    D(vkCreateFence); D(vkDestroyFence); D(vkWaitForFences); D(vkResetFences); D(vkAllocateMemory); D(vkFreeMemory);
    VkQueue q; vkGetDeviceQueue(dev, qfi, 0, &q);

    VkSurfaceCapabilitiesKHR caps; CK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps));
    lg("vk: surface images %u..%u usage 0x%x extent %ux%u transform 0x%x alpha 0x%x", caps.minImageCount, caps.maxImageCount, caps.supportedUsageFlags, caps.currentExtent.width, caps.currentExtent.height, caps.currentTransform, caps.supportedCompositeAlpha);
    uint32_t nf = 0; CK(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, 0));
    fmts = malloc(sizeof *fmts * (nf + 1)); CK(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, fmts));
    VkSurfaceFormatKHR fmt = fmts[0];
    for (uint32_t i = 0; i < nf; i++) { lg("vk: surface format %d colorspace %d", (int)fmts[i].format, (int)fmts[i].colorSpace); if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) fmt = fmts[i]; }
    VkSwapchainCreateInfoKHR sc_i = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    sc_i.surface = surf; sc_i.minImageCount = caps.minImageCount < 3 ? 3 : caps.minImageCount; if (caps.maxImageCount && sc_i.minImageCount > caps.maxImageCount) sc_i.minImageCount = caps.maxImageCount;
    sc_i.imageFormat = fmt.format; sc_i.imageColorSpace = fmt.colorSpace; sc_i.imageExtent = ext; sc_i.imageArrayLayers = 1;
    sc_i.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; sc_i.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sc_i.preTransform = caps.currentTransform; sc_i.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
    sc_i.presentMode = VK_PRESENT_MODE_FIFO_KHR; sc_i.clipped = VK_TRUE;
    CK(vkCreateSwapchainKHR(dev, &sc_i, 0, &sc));
    CK(vkGetSwapchainImagesKHR(dev, sc, &nimg, 0));
    imgs = malloc(sizeof *imgs * nimg); CK(vkGetSwapchainImagesKHR(dev, sc, &nimg, imgs));
    lg("vk: swapchain %u images, format %d", nimg, (int)fmt.format);

    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfi };
    CK(vkCreateCommandPool(dev, &cpi, 0, &pool));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    CK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, 0, VK_FENCE_CREATE_SIGNALED_BIT };
    CK(vkCreateFence(dev, &fci, 0, &fence));
    VkSemaphoreCreateInfo semi = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    acq = calloc(nimg, sizeof *acq); rend = calloc(nimg, sizeof *rend);
    for (uint32_t i = 0; i < nimg; i++) { CK(vkCreateSemaphore(dev, &semi, 0, &acq[i])); CK(vkCreateSemaphore(dev, &semi, 0, &rend[i])); }

    lg("vk: presenting %d s of colour-cycling clears (the screen should change colour)", secs);
    double t0 = now(), tl = t0; int frames = 0, lastn = 0; uint32_t fi = 0;
    while (now() - t0 < secs) {
        CK(vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull)); CK(vkResetFences(dev, 1, &fence));
        uint32_t idx; VkResult ar = vkAcquireNextImageKHR(dev, sc, ~0ull, acq[fi % nimg], VK_NULL_HANDLE, &idx);
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) { lg("vk FAIL acquire %d", (int)ar); goto fail; }
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        vkBeginCommandBuffer(cb, &bi);
        VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageMemoryBarrier b1 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, imgs[idx], rng };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &b1);
        double t = now() - t0; VkClearColorValue cc = { { (float)(0.5 + 0.5 * sin(t * 2.0)), (float)(0.5 + 0.5 * sin(t * 2.0 + 2.1)), (float)(0.5 + 0.5 * sin(t * 2.0 + 4.2)), 1.0f } };
        vkCmdClearColorImage(cb, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &rng);
        VkImageMemoryBarrier b2 = b1; b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b2.dstAccessMask = 0; b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b2.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, 0, 0, 0, 1, &b2);
        vkEndCommandBuffer(cb);
        VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 1, &acq[fi % nimg], &ws, 1, &cb, 1, &rend[idx] };
        CK(vkQueueSubmit(q, 1, &si, fence));
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, 0, 1, &rend[idx], 1, &sc, &idx };
        VkResult pr2 = vkQueuePresentKHR(q, &pi);
        if (pr2 != VK_SUCCESS && pr2 != VK_SUBOPTIMAL_KHR) { lg("vk FAIL present %d", (int)pr2); goto fail; }
        frames++; fi++;
        if (now() - tl >= 1.0) { lg("vk: %d frames/s (total %d)", frames - lastn, frames); lastn = frames; tl = now(); }
    }
    { double dt = now() - t0; lg("vk: %d frames in %.2f s = %.1f fps", frames, dt, frames / dt); }
    vkDeviceWaitIdle(dev);

    /* GPU memory: how much device-local memory one allocation can take next to whatever the process already holds */
    if (localType != ~0u) {
        static const unsigned long long mb[] = { 256, 1024, 2048, 4096, 6144 };
        for (unsigned i = 0; i < sizeof mb / sizeof *mb; i++) {
            VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mb[i] << 20, localType }; VkDeviceMemory m = VK_NULL_HANDLE;
            VkResult r = vkAllocateMemory(dev, &mai, 0, &m); lg("vk: allocate %llu MB device-local -> %d", mb[i], (int)r);
            if (r == VK_SUCCESS) vkFreeMemory(dev, m, 0); else break;
        }
    }
    result = 0;
fail:
    if (dev) {
        PFN_vkDeviceWaitIdle wi = (PFN_vkDeviceWaitIdle)vkGetDeviceProcAddr(dev, "vkDeviceWaitIdle"); if (wi) wi(dev);
        PFN_vkDestroySemaphore ds = (PFN_vkDestroySemaphore)vkGetDeviceProcAddr(dev, "vkDestroySemaphore");
        PFN_vkDestroyFence df = (PFN_vkDestroyFence)vkGetDeviceProcAddr(dev, "vkDestroyFence");
        PFN_vkDestroyCommandPool dcp = (PFN_vkDestroyCommandPool)vkGetDeviceProcAddr(dev, "vkDestroyCommandPool");
        PFN_vkDestroySwapchainKHR dsc = (PFN_vkDestroySwapchainKHR)vkGetDeviceProcAddr(dev, "vkDestroySwapchainKHR");
        PFN_vkDestroyDevice dd = (PFN_vkDestroyDevice)vkGetDeviceProcAddr(dev, "vkDestroyDevice");
        for (uint32_t i = 0; i < nimg; i++) { if (acq && acq[i]) ds(dev, acq[i], 0); if (rend && rend[i]) ds(dev, rend[i], 0); }
        if (fence) df(dev, fence, 0);
        if (pool) dcp(dev, pool, 0);
        if (sc) dsc(dev, sc, 0);
        dd(dev, 0);
        lg("vk[%s]: device destroyed", tag);
    }
    if (inst) {
        PFN_vkDestroySurfaceKHR dsf = (PFN_vkDestroySurfaceKHR)vk_icdGetInstanceProcAddr(inst, "vkDestroySurfaceKHR");
        PFN_vkDestroyInstance di = (PFN_vkDestroyInstance)vk_icdGetInstanceProcAddr(inst, "vkDestroyInstance");
        if (surf && dsf) dsf(inst, surf, 0);
        if (di) di(inst, 0);
        lg("vk[%s]: instance destroyed", tag);
    }
    free(acq); free(rend); free(imgs); free(iex); free(dex); free(modes); free(fmts);
    lg("vk[%s]: %s", tag, result == 0 ? "PASS" : "FAIL");
    return result;
}
