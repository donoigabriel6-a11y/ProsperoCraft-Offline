import org.lwjgl.PointerBuffer;
import org.lwjgl.glfw.GLFW;
import org.lwjgl.glfw.GLFWVulkan;
import org.lwjgl.system.MemoryStack;
import org.lwjgl.vulkan.*;

import java.io.OutputStream;
import java.io.PrintStream;
import java.nio.IntBuffer;
import java.nio.LongBuffer;

import static org.lwjgl.vulkan.KHRSurface.*;
import static org.lwjgl.vulkan.KHRSwapchain.*;
import static org.lwjgl.vulkan.VK10.*;

/** Vulkan smoke test (make_vk_title.sh, mode "vk"): the real LWJGL Vulkan classes -> ps5lwjgl_vk.c -> RADV. Clears the screen with a cycling colour for ~8 s and logs the frame rate. */
public class VkTest {
    static void ck(int r, String what) { if (r != VK_SUCCESS) throw new IllegalStateException(what + " = " + r); }

    static void routeOutput() {
        PrintStream ps = new PrintStream(new OutputStream() {
            final StringBuilder sb = new StringBuilder();
            @Override public void write(int b) { if (b == '\n') { Hello.log("[lwjgl] " + sb); sb.setLength(0); } else if (b != '\r') sb.append((char) b); }
        }, true);
        System.setErr(ps);
        System.setOut(ps);
    }

    public static void run() {
        routeOutput();
        try { go(); } catch (Throwable t) { Hello.log("FAIL VkTest: " + t); t.printStackTrace(); }
    }

    static void barrier(VkImageMemoryBarrier.Buffer b, VkCommandBuffer cmd, long image, int oldL, int newL, int srcA, int dstA, int srcS, int dstS) {   // one struct reused every frame (the per-thread stack is only 64 KB)
        b.srcAccessMask(srcA).dstAccessMask(dstA).oldLayout(oldL).newLayout(newL).image(image);
        vkCmdPipelineBarrier(cmd, srcS, dstS, 0, null, null, b);
    }

    static void go() {
        Hello.log("VkTest: start");
        if (!GLFW.glfwInit()) { Hello.log("FAIL glfwInit"); return; }
        GLFW.glfwWindowHint(GLFW.GLFW_CLIENT_API, GLFW.GLFW_NO_API);
        Hello.log("VkTest: glfwVulkanSupported=" + GLFWVulkan.glfwVulkanSupported());
        long win = GLFW.glfwCreateWindow(1920, 1080, "vktest", 0L, 0L);
        if (win == 0L) { Hello.log("FAIL glfwCreateWindow"); return; }
        Hello.log("VkTest: VK loaded (it creates itself in 3.3.x), instance version " + VK.getInstanceVersionSupported());
        try (MemoryStack s = MemoryStack.stackPush()) {
            PointerBuffer req = GLFWVulkan.glfwGetRequiredInstanceExtensions();
            if (req == null) { Hello.log("FAIL glfwGetRequiredInstanceExtensions"); return; }
            for (int i = 0; i < req.remaining(); i++) Hello.log("VkTest: required extension " + org.lwjgl.system.MemoryUtil.memUTF8(req.get(i)));
            VkApplicationInfo app = VkApplicationInfo.calloc(s).sType(VK_STRUCTURE_TYPE_APPLICATION_INFO).pApplicationName(s.UTF8("vktest")).apiVersion(VK11.VK_API_VERSION_1_1);
            VkInstanceCreateInfo ici = VkInstanceCreateInfo.calloc(s).sType(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO).pApplicationInfo(app).ppEnabledExtensionNames(req);
            PointerBuffer pp = s.mallocPointer(1);
            ck(vkCreateInstance(ici, null, pp), "vkCreateInstance");
            VkInstance inst = new VkInstance(pp.get(0), ici);
            IntBuffer ic = s.mallocInt(1);
            ck(vkEnumeratePhysicalDevices(inst, ic, null), "vkEnumeratePhysicalDevices(count)");
            PointerBuffer pds = s.mallocPointer(ic.get(0));
            ck(vkEnumeratePhysicalDevices(inst, ic, pds), "vkEnumeratePhysicalDevices");
            VkPhysicalDevice pd = new VkPhysicalDevice(pds.get(0), inst);
            VkPhysicalDeviceProperties props = VkPhysicalDeviceProperties.malloc(s);
            vkGetPhysicalDeviceProperties(pd, props);
            Hello.log("VkTest: device " + props.deviceNameString() + " api " + VK_VERSION_MAJOR(props.apiVersion()) + "." + VK_VERSION_MINOR(props.apiVersion()));
            Hello.log("VkTest: presentation support family 0 = " + GLFWVulkan.glfwGetPhysicalDevicePresentationSupport(inst, pd, 0));

            LongBuffer pl = s.mallocLong(1);
            ck(GLFWVulkan.glfwCreateWindowSurface(inst, win, null, pl), "glfwCreateWindowSurface");
            long surface = pl.get(0);

            VkDeviceQueueCreateInfo.Buffer qci = VkDeviceQueueCreateInfo.calloc(1, s).sType(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO).queueFamilyIndex(0).pQueuePriorities(s.floats(1.0f));
            VkDeviceCreateInfo dci = VkDeviceCreateInfo.calloc(s).sType(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO).pQueueCreateInfos(qci).ppEnabledExtensionNames(s.pointers(s.UTF8(VK_KHR_SWAPCHAIN_EXTENSION_NAME)));
            ck(vkCreateDevice(pd, dci, null, pp), "vkCreateDevice");
            VkDevice dev = new VkDevice(pp.get(0), pd, dci);
            vkGetDeviceQueue(dev, 0, 0, pp);
            VkQueue queue = new VkQueue(pp.get(0), dev);

            VkSurfaceCapabilitiesKHR caps = VkSurfaceCapabilitiesKHR.malloc(s);
            ck(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, caps), "surface caps");
            int w = caps.currentExtent().width(), h = caps.currentExtent().height();
            ck(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, ic, null), "surface formats(count)");
            VkSurfaceFormatKHR.Buffer fmts = VkSurfaceFormatKHR.malloc(ic.get(0), s);
            ck(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, ic, fmts), "surface formats");
            int fmt = fmts.get(0).format(), cs = fmts.get(0).colorSpace();
            for (int i = 0; i < fmts.remaining(); i++) if (fmts.get(i).format() == VK_FORMAT_B8G8R8A8_UNORM) { fmt = fmts.get(i).format(); cs = fmts.get(i).colorSpace(); break; }
            Hello.log("VkTest: surface " + w + "x" + h + " images " + caps.minImageCount() + ".." + caps.maxImageCount() + " format " + fmt);

            VkSwapchainCreateInfoKHR sci = VkSwapchainCreateInfoKHR.calloc(s).sType(VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR).surface(surface)
                .minImageCount(Math.max(3, caps.minImageCount())).imageFormat(fmt).imageColorSpace(cs).imageArrayLayers(1)
                .imageUsage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT).imageSharingMode(VK_SHARING_MODE_EXCLUSIVE)
                .preTransform(caps.currentTransform()).compositeAlpha(VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR).presentMode(VK_PRESENT_MODE_FIFO_KHR).clipped(true);
            sci.imageExtent().width(w).height(h);
            ck(vkCreateSwapchainKHR(dev, sci, null, pl), "vkCreateSwapchainKHR");
            long sc = pl.get(0);
            ck(vkGetSwapchainImagesKHR(dev, sc, ic, null), "swapchain images(count)");
            int n = ic.get(0);
            LongBuffer imgs = s.mallocLong(n);
            ck(vkGetSwapchainImagesKHR(dev, sc, ic, imgs), "swapchain images");
            Hello.log("VkTest: swapchain with " + n + " images");

            VkCommandPoolCreateInfo pci = VkCommandPoolCreateInfo.calloc(s).sType(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO).flags(VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT).queueFamilyIndex(0);
            ck(vkCreateCommandPool(dev, pci, null, pl), "vkCreateCommandPool");
            long pool = pl.get(0);
            VkCommandBufferAllocateInfo cai = VkCommandBufferAllocateInfo.calloc(s).sType(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO).commandPool(pool).level(VK_COMMAND_BUFFER_LEVEL_PRIMARY).commandBufferCount(1);
            ck(vkAllocateCommandBuffers(dev, cai, pp), "vkAllocateCommandBuffers");
            VkCommandBuffer cmd = new VkCommandBuffer(pp.get(0), dev);

            VkSemaphoreCreateInfo semi = VkSemaphoreCreateInfo.calloc(s).sType(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
            long[] acq = new long[n], rend = new long[n];
            for (int i = 0; i < n; i++) { ck(vkCreateSemaphore(dev, semi, null, pl), "semaphore"); acq[i] = pl.get(0); ck(vkCreateSemaphore(dev, semi, null, pl), "semaphore"); rend[i] = pl.get(0); }
            VkFenceCreateInfo fi = VkFenceCreateInfo.calloc(s).sType(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO).flags(VK_FENCE_CREATE_SIGNALED_BIT);
            ck(vkCreateFence(dev, fi, null, pl), "vkCreateFence");
            long fence = pl.get(0);

            VkCommandBufferBeginInfo bi = VkCommandBufferBeginInfo.calloc(s).sType(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO).flags(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
            VkClearColorValue clear = VkClearColorValue.calloc(s);
            VkImageSubresourceRange range = VkImageSubresourceRange.calloc(s).aspectMask(VK_IMAGE_ASPECT_COLOR_BIT).levelCount(1).layerCount(1);
            IntBuffer pIdx = s.mallocInt(1), waitStage = s.ints(VK_PIPELINE_STAGE_TRANSFER_BIT);
            LongBuffer pAcq = s.mallocLong(1), pRend = s.mallocLong(1), pSc = s.longs(sc), pFence = s.longs(fence);
            PointerBuffer pCmd = s.pointers(cmd);
            VkSubmitInfo si = VkSubmitInfo.calloc(s).sType(VK_STRUCTURE_TYPE_SUBMIT_INFO).waitSemaphoreCount(1).pWaitSemaphores(pAcq).pWaitDstStageMask(waitStage).pCommandBuffers(pCmd).pSignalSemaphores(pRend);
            VkPresentInfoKHR pi = VkPresentInfoKHR.calloc(s).sType(VK_STRUCTURE_TYPE_PRESENT_INFO_KHR).pWaitSemaphores(pRend).swapchainCount(1).pSwapchains(pSc).pImageIndices(pIdx);

            VkImageMemoryBarrier.Buffer bar = VkImageMemoryBarrier.calloc(1, s).sType(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER).srcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).dstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            bar.subresourceRange().aspectMask(VK_IMAGE_ASPECT_COLOR_BIT).levelCount(1).layerCount(1);
            long t0 = System.nanoTime(), tl = t0; int frames = 0, total = 0, fr = 0;
            while (System.nanoTime() - t0 < 8_000_000_000L) {
                ck(vkWaitForFences(dev, pFence, true, -1L), "wait fence");
                vkResetFences(dev, pFence);
                pAcq.put(0, acq[fr % n]);
                int r = vkAcquireNextImageKHR(dev, sc, -1L, acq[fr % n], VK_NULL_HANDLE, pIdx);
                if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) throw new IllegalStateException("acquire = " + r);
                int idx = pIdx.get(0); long img = imgs.get(idx);
                pRend.put(0, rend[idx]);
                vkResetCommandBuffer(cmd, 0);
                vkBeginCommandBuffer(cmd, bi);
                barrier(bar, cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                float t = total * 0.02f;
                clear.float32(0, 0.5f + 0.5f * (float) Math.sin(t)).float32(1, 0.5f + 0.5f * (float) Math.sin(t + 2.1f)).float32(2, 0.5f + 0.5f * (float) Math.sin(t + 4.2f)).float32(3, 1f);
                vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, clear, range);
                barrier(bar, cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
                ck(vkEndCommandBuffer(cmd), "end");
                ck(vkQueueSubmit(queue, si, fence), "submit");
                r = vkQueuePresentKHR(queue, pi);
                if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) throw new IllegalStateException("present = " + r);
                fr++; frames++; total++;
                long now = System.nanoTime();
                if (now - tl >= 1_000_000_000L) { Hello.log("VkTest: " + frames + " frames/s (total " + total + ")"); frames = 0; tl = now; }
            }
            Hello.log("VkTest: " + total + " frames in " + (System.nanoTime() - t0) / 1e9 + " s = " + total / ((System.nanoTime() - t0) / 1e9) + " fps");
            vkDeviceWaitIdle(dev);
            vkDestroyFence(dev, fence, null);
            for (int i = 0; i < n; i++) { vkDestroySemaphore(dev, acq[i], null); vkDestroySemaphore(dev, rend[i], null); }
            vkDestroyCommandPool(dev, pool, null);
            vkDestroySwapchainKHR(dev, sc, null);
            vkDestroyDevice(dev, null);
            vkDestroySurfaceKHR(inst, surface, null);
            vkDestroyInstance(inst, null);
            Hello.log("VkTest: PASS");
        }
        GLFW.glfwDestroyWindow(win);
    }
}
