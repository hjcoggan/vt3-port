/*
 * Vulkan probe for the Android port: creates a swapchain on the NativeActivity window and clears
 * it to a colour that cycles. It checks the packaging, the Vulkan driver and the present path on
 * the device. The runtime's Vulkan renderer replaces it.
 */
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <vulkan/vulkan.h>

#include <stdint.h>
#include <string.h>
#include <time.h>

#define TAG "vt3probe"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define VK_TRY(expr, what)                                  \
    do {                                                    \
        VkResult r_ = (expr);                               \
        if (r_ != VK_SUCCESS) {                             \
            LOGE("%s failed: VkResult %d", what, (int)r_);  \
            return -1;                                      \
        }                                                   \
    } while (0)

typedef struct probe {
    ANativeWindow *window;
    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice phys;
    uint32_t qfam;
    VkDevice dev;
    VkQueue queue;
    VkSwapchainKHR swapchain;
    VkExtent2D extent;
    uint32_t nimages;
    VkImage images[8];
    VkCommandPool pool;
    VkCommandBuffer cmds[8];
    VkSemaphore acquire[8], done[8];   /* one pair per swapchain image: a semaphore is reused only after its wait */
    VkFence fence;
    uint64_t frames;
} probe;

static int init_vulkan(probe *p)
{
    const char *inst_ext[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME };
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "vt3probe";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = inst_ext;
    VK_TRY(vkCreateInstance(&ici, NULL, &p->instance), "vkCreateInstance");

    VkAndroidSurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR };
    sci.window = p->window;
    VK_TRY(vkCreateAndroidSurfaceKHR(p->instance, &sci, NULL, &p->surface), "vkCreateAndroidSurfaceKHR");

    uint32_t ndev = 0;
    vkEnumeratePhysicalDevices(p->instance, &ndev, NULL);
    if (ndev == 0) { LOGE("no Vulkan device"); return -1; }
    VkPhysicalDevice devs[4];
    if (ndev > 4) ndev = 4;
    vkEnumeratePhysicalDevices(p->instance, &ndev, devs);
    p->phys = devs[0];

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(p->phys, &props);
    LOG("device: %s, Vulkan %u.%u.%u", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
        VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(p->phys, &nq, NULL);
    VkQueueFamilyProperties qf[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(p->phys, &nq, qf);
    int found = -1;
    for (uint32_t i = 0; i < nq && found < 0; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(p->phys, i, p->surface, &present);
        if (present && (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) found = (int)i;
    }
    if (found < 0) { LOGE("no graphics+present queue"); return -1; }
    p->qfam = (uint32_t)found;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = p->qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char *dev_ext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_ext;
    VK_TRY(vkCreateDevice(p->phys, &dci, NULL, &p->dev), "vkCreateDevice");
    vkGetDeviceQueue(p->dev, p->qfam, 0, &p->queue);

    VkSurfaceCapabilitiesKHR caps;
    VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(p->phys, p->surface, &caps), "surface caps");
    p->extent = caps.currentExtent;
    if (p->extent.width == 0xFFFFFFFFu) {
        p->extent.width = (uint32_t)ANativeWindow_getWidth(p->window);
        p->extent.height = (uint32_t)ANativeWindow_getHeight(p->window);
    }
    uint32_t nfmt = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(p->phys, p->surface, &nfmt, NULL);
    VkSurfaceFormatKHR fmts[16];
    if (nfmt > 16) nfmt = 16;
    vkGetPhysicalDeviceSurfaceFormatsKHR(p->phys, p->surface, &nfmt, fmts);
    VkSurfaceFormatKHR pick = fmts[0];
    for (uint32_t i = 0; i < nfmt; i++) {
        if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM || fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
            pick = fmts[i];
            break;
        }
    }
    LOG("surface %ux%u, format %d", p->extent.width, p->extent.height, (int)pick.format);

    uint32_t min_images = caps.minImageCount;
    if (caps.maxImageCount && min_images > caps.maxImageCount) min_images = caps.maxImageCount;
    VkSwapchainCreateInfoKHR sc = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    sc.surface = p->surface;
    sc.minImageCount = min_images;
    sc.imageFormat = pick.format;
    sc.imageColorSpace = pick.colorSpace;
    sc.imageExtent = p->extent;
    sc.imageArrayLayers = 1;
    sc.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sc.preTransform = caps.currentTransform;
    sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    VkPresentModeKHR modes[8];
    uint32_t nmodes = 8;
    vkGetPhysicalDeviceSurfacePresentModesKHR(p->phys, p->surface, &nmodes, modes);
    sc.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < nmodes; i++) {
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) sc.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    }
    LOG("present mode %d of %u modes", (int)sc.presentMode, (unsigned)nmodes);
    sc.clipped = VK_TRUE;
    VK_TRY(vkCreateSwapchainKHR(p->dev, &sc, NULL, &p->swapchain), "vkCreateSwapchainKHR");
    vkGetSwapchainImagesKHR(p->dev, p->swapchain, &p->nimages, NULL);
    if (p->nimages > 8) p->nimages = 8;
    vkGetSwapchainImagesKHR(p->dev, p->swapchain, &p->nimages, p->images);

    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;   /* each frame re-records its buffer */
    cpi.queueFamilyIndex = p->qfam;
    VK_TRY(vkCreateCommandPool(p->dev, &cpi, NULL, &p->pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = p->pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = p->nimages;
    VK_TRY(vkAllocateCommandBuffers(p->dev, &cbai, p->cmds), "vkAllocateCommandBuffers");

    VkSemaphoreCreateInfo sci2 = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < p->nimages; i++) {
        VK_TRY(vkCreateSemaphore(p->dev, &sci2, NULL, &p->acquire[i]), "semaphore");
        VK_TRY(vkCreateSemaphore(p->dev, &sci2, NULL, &p->done[i]), "semaphore");
    }
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VK_TRY(vkCreateFence(p->dev, &fci, NULL, &p->fence), "fence");
    LOG("swapchain: %u images", (unsigned)p->nimages);
    return 0;
}

static void hsv_to_rgb(float h, float *r, float *g, float *b)
{
    float k = h * 6.0f;
    int i = (int)k % 6;
    float f = k - (float)(int)k;
    float q = 1.0f - f, t = f;
    switch (i) {
    case 0: *r = 1; *g = t; *b = 0; break;
    case 1: *r = q; *g = 1; *b = 0; break;
    case 2: *r = 0; *g = 1; *b = t; break;
    case 3: *r = 0; *g = q; *b = 1; break;
    case 4: *r = t; *g = 0; *b = 1; break;
    default: *r = 1; *g = 0; *b = q; break;
    }
}

#define TRACE_STAGE(name) do { if (p->frames < 0) LOG("frame %llu: %s", (unsigned long long)p->frames, name); } while (0)

static int frame(probe *p)
{
    TRACE_STAGE("wait fence");
    VkResult fw = vkWaitForFences(p->dev, 1, &p->fence, VK_TRUE, 2000000000ull);
    if (fw != VK_SUCCESS) { LOGE("fence wait at frame %llu: %d", (unsigned long long)p->frames, (int)fw); return -1; }
    vkResetFences(p->dev, 1, &p->fence);
    uint32_t idx = 0;
    TRACE_STAGE("acquire");
    VkSemaphore acq = p->acquire[p->frames % p->nimages];
    VkResult ar = vkAcquireNextImageKHR(p->dev, p->swapchain, 2000000000ull, acq, VK_NULL_HANDLE, &idx);
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) { LOGE("acquire at frame %llu: %d", (unsigned long long)p->frames, (int)ar); return -1; }
    TRACE_STAGE("record");

    float r, g, b;
    hsv_to_rgb((float)(p->frames % 360) / 360.0f, &r, &g, &b);
    VkCommandBuffer cb = p->cmds[idx];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cb, &bi);
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageMemoryBarrier to_dst = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = p->images[idx];
    to_dst.subresourceRange = range;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &to_dst);
    VkClearColorValue col = { { r, g, b, 1.0f } };
    vkCmdClearColorImage(cb, p->images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &range);
    VkImageMemoryBarrier to_present = to_dst;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_present.dstAccessMask = 0;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                         0, NULL, 0, NULL, 1, &to_present);
    vkEndCommandBuffer(cb);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acq;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &p->done[idx];
    TRACE_STAGE("submit");
    vkQueueSubmit(p->queue, 1, &si, p->fence);
    TRACE_STAGE("present");

    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &p->done[idx];
    pi.swapchainCount = 1;
    pi.pSwapchains = &p->swapchain;
    pi.pImageIndices = &idx;
    TRACE_STAGE("queue present call");
    VkResult pr = vkQueuePresentKHR(p->queue, &pi);
    TRACE_STAGE("queue present returned");
    vkQueueWaitIdle(p->queue);   /* test: serialise the queue to tell a sync bug from a driver stall */
    if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) { LOGE("present at frame %llu: %d", (unsigned long long)p->frames, (int)pr); return -1; }
    p->frames++;
    if (p->frames % 30 == 0) LOG("frame %llu", (unsigned long long)p->frames);
    return 0;
}

static void destroy_vulkan(probe *p)
{
    if (p->dev) {
        vkDeviceWaitIdle(p->dev);
        if (p->fence) vkDestroyFence(p->dev, p->fence, NULL);
        for (uint32_t i = 0; i < p->nimages; i++) {
            if (p->acquire[i]) vkDestroySemaphore(p->dev, p->acquire[i], NULL);
            if (p->done[i]) vkDestroySemaphore(p->dev, p->done[i], NULL);
        }
        if (p->pool) vkDestroyCommandPool(p->dev, p->pool, NULL);
        if (p->swapchain) vkDestroySwapchainKHR(p->dev, p->swapchain, NULL);
        vkDestroyDevice(p->dev, NULL);
    }
    if (p->surface) vkDestroySurfaceKHR(p->instance, p->surface, NULL);
    if (p->instance) vkDestroyInstance(p->instance, NULL);
    memset(p, 0, sizeof *p);
}

static probe g_probe;
static int g_ready;

static void on_cmd(struct android_app *app, int32_t cmd)
{
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        if (app->window) {
            g_probe.window = app->window;
            if (init_vulkan(&g_probe) == 0) g_ready = 1;
            else { LOGE("Vulkan init failed"); destroy_vulkan(&g_probe); }
        }
        break;
    case APP_CMD_TERM_WINDOW:
        g_ready = 0;
        destroy_vulkan(&g_probe);
        break;
    default:
        break;
    }
}

void android_main(struct android_app *app)
{
    app->onAppCmd = on_cmd;
    for (;;) {
        int events = 0;
        struct android_poll_source *source = NULL;
        int timeout = g_ready ? 0 : -1;
        while (ALooper_pollOnce(timeout, NULL, &events, (void **)&source) >= 0) {
            if (source) source->process(app, source);
            if (app->destroyRequested) { destroy_vulkan(&g_probe); return; }
            timeout = g_ready ? 0 : -1;
        }
        if (g_ready && frame(&g_probe) != 0) {
            LOGE("frame failed; stopping");
            g_ready = 0;
        }
        if (g_ready) {
            struct timespec pause = { 0, 16000000 };   /* test: cap the loop at about 60 frames a second */
            nanosleep(&pause, NULL);
        }
    }
}
