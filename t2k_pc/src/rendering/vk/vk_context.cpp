// ============================================================================
// vk_context.cpp — see vk_context.h.
// ============================================================================

#include "vk_context.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <chrono>

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

#include <volk.h>

namespace ts {
namespace vkctx {

namespace {

const char* resultName(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        default: return "VK_ERROR_?";
    }
}

#define VK_FAIL(expr, what) do { VkResult _r = (expr); if (_r != VK_SUCCESS) { \
    std::fprintf(stderr, "[vk] %s failed: %s\n", what, resultName(_r)); return false; } } while (0)

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr, "[vk-validation] %s\n", data->pMessage);
    return VK_FALSE;
}

bool hasLayer(const char* name) {
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> layers(n);
    vkEnumerateInstanceLayerProperties(&n, layers.data());
    for (const auto& l : layers) if (std::strcmp(l.layerName, name) == 0) return true;
    return false;
}

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

bool hasDeviceExt(VkPhysicalDevice pd, const char* name) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, exts.data());
    for (const auto& e : exts) if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

// Score a physical device: discrete > integrated > cpu; must be 1.3, support
// the queue we need and (if presenting) the surface.
int scoreDevice(VkPhysicalDevice pd, VkSurfaceKHR surface, uint32_t& familyOut) {
    VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd, &p);
    if (p.apiVersion < VK_API_VERSION_1_3) return -1;
    if (surface != VK_NULL_HANDLE && !hasDeviceExt(pd, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) return -1;

    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f11.pNext = &f13;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; f2.pNext = &f11;
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    if (!f13.dynamicRendering || !f13.synchronization2 || !f11.multiview) return -1;

    uint32_t n = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, nullptr);
    std::vector<VkQueueFamilyProperties> fams(n);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, fams.data());
    bool found = false;
    for (uint32_t i = 0; i < n; ++i) {
        if (!(fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        if (surface != VK_NULL_HANDLE) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &present);
            if (!present) continue;
        }
        familyOut = i; found = true; break;
    }
    if (!found) return -1;
    switch (p.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 300;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 200;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 100;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 10;
        default: return 50;
    }
}

bool createSwapchain(Context& c) {
    VkSurfaceCapabilitiesKHR caps;
    VK_FAIL(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(c.phys, c.surface, &caps), "surface caps");

    // Format: prefer an sRGB 8-bit swapchain so the composite shader writes
    // linear light and the display transfer does the gamma. Fall back to
    // whatever is first.
    uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(c.phys, c.surface, &nf, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(nf);
    vkGetPhysicalDeviceSurfaceFormatsKHR(c.phys, c.surface, &nf, fmts.data());
    VkSurfaceFormatKHR chosen = fmts.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : fmts[0];
    for (const auto& f : fmts) {
        if ((f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = f; break; }
    }
    c.swapFormat = chosen.format;
    c.swapColorSpace = chosen.colorSpace;

    // Present mode: the requested one if the surface offers it, else FIFO
    // (always available).
    uint32_t nm = 0; vkGetPhysicalDeviceSurfacePresentModesKHR(c.phys, c.surface, &nm, nullptr);
    std::vector<VkPresentModeKHR> modes(nm);
    vkGetPhysicalDeviceSurfacePresentModesKHR(c.phys, c.surface, &nm, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (auto m : modes) if (m == c.presentMode) mode = m;
    c.presentMode = mode;

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        int w = 0, h = 0;
        SDL_Vulkan_GetDrawableSize(c.window, &w, &h);
        extent.width  = std::clamp<uint32_t>((uint32_t)w, caps.minImageExtent.width,  caps.maxImageExtent.width);
        extent.height = std::clamp<uint32_t>((uint32_t)h, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) return false;   // minimised
    c.swapExtent = extent;

    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && count > caps.maxImageCount) count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = c.surface;
    sci.minImageCount = count;
    sci.imageFormat = c.swapFormat;
    sci.imageColorSpace = c.swapColorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = mode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = c.swapchain;
    VkSwapchainKHR sc;
    VK_FAIL(vkCreateSwapchainKHR(c.device, &sci, nullptr, &sc), "vkCreateSwapchainKHR");
    if (c.swapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(c.device, c.swapchain, nullptr);
    c.swapchain = sc;

    uint32_t ni = 0; vkGetSwapchainImagesKHR(c.device, c.swapchain, &ni, nullptr);
    c.swapImages.resize(ni);
    vkGetSwapchainImagesKHR(c.device, c.swapchain, &ni, c.swapImages.data());
    for (auto v : c.swapViews) vkDestroyImageView(c.device, v, nullptr);
    c.swapViews.assign(ni, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < ni; ++i) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = c.swapImages[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = c.swapFormat;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_FAIL(vkCreateImageView(c.device, &vci, nullptr, &c.swapViews[i]), "swapchain view");
    }
    for (auto s : c.presentSemaphores) vkDestroySemaphore(c.device, s, nullptr);
    c.presentSemaphores.assign(ni, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < ni; ++i) {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_FAIL(vkCreateSemaphore(c.device, &si, nullptr, &c.presentSemaphores[i]), "present semaphore");
    }
    c.needRecreate = false;
    c.presentId = 0;          // present ids are per swapchain
    c.lastPresentMs = 0.0;
    c.presentWaitTimeouts = 0;
    return true;
}

} // namespace

bool create(Context& c, const Desc& d) {
    c = Context{};
    c.window = d.window;
    c.headless = (d.window == nullptr);
    c.presentMode = d.presentMode;
    c.paceLog = std::getenv("T2K_DEBUG") != nullptr;

    if (volkInitialize() != VK_SUCCESS) {
        std::fprintf(stderr, "[vk] no Vulkan loader (libvulkan.so.1) on this machine\n");
        return false;
    }
    const uint32_t loaderVersion = volkGetInstanceVersion();
    if (loaderVersion < VK_API_VERSION_1_3) {
        std::fprintf(stderr, "[vk] loader reports %u.%u, need 1.3\n",
                     VK_API_VERSION_MAJOR(loaderVersion), VK_API_VERSION_MINOR(loaderVersion));
        return false;
    }

    // --- instance ---
    std::vector<const char*> exts;
    if (!c.headless) {
        unsigned n = 0;
        if (!SDL_Vulkan_GetInstanceExtensions(d.window, &n, nullptr)) {
            std::fprintf(stderr, "[vk] SDL_Vulkan_GetInstanceExtensions: %s\n", SDL_GetError());
            return false;
        }
        exts.resize(n);
        SDL_Vulkan_GetInstanceExtensions(d.window, &n, exts.data());
    }
    std::vector<const char*> layers;
    const bool useValidation = d.validation && hasLayer("VK_LAYER_KHRONOS_validation");
    if (useValidation) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = d.appName;
    ai.applicationVersion = 1;
    ai.pEngineName = "Tube Shooter";
    ai.engineVersion = 1;
    ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = (uint32_t)exts.size();
    ici.ppEnabledExtensionNames = exts.data();
    ici.enabledLayerCount = (uint32_t)layers.size();
    ici.ppEnabledLayerNames = layers.data();
    if (d.xr && d.xr->createInstance) {
        // The OpenXR runtime creates the instance from this create-info.
        if (!d.xr->createInstance(d.xr->user, &ici, &c.instance)) return false;
    } else {
        VK_FAIL(vkCreateInstance(&ici, nullptr, &c.instance), "vkCreateInstance");
    }
    volkLoadInstanceOnly(c.instance);

    if (useValidation) {
        VkDebugUtilsMessengerCreateInfoEXT dci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dci.pfnUserCallback = debugCallback;
        vkCreateDebugUtilsMessengerEXT(c.instance, &dci, nullptr, &c.debugMessenger);
    }

    // --- surface ---
    if (!c.headless) {
        if (!SDL_Vulkan_CreateSurface(d.window, c.instance, &c.surface)) {
            std::fprintf(stderr, "[vk] SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
            return false;
        }
    }

    // --- physical device ---
    uint32_t nd = 0; vkEnumeratePhysicalDevices(c.instance, &nd, nullptr);
    if (nd == 0) { std::fprintf(stderr, "[vk] no Vulkan devices\n"); return false; }
    std::vector<VkPhysicalDevice> devs(nd);
    vkEnumeratePhysicalDevices(c.instance, &nd, devs.data());
    int best = -1;
    if (d.xr && d.xr->pickPhysicalDevice) {
        // The runtime names the GPU the headset is attached to; it must still
        // meet this renderer's floor (1.3 + dynamic rendering + sync2 +
        // multiview) and present to the mirror window.
        VkPhysicalDevice pd = VK_NULL_HANDLE;
        if (!d.xr->pickPhysicalDevice(d.xr->user, c.instance, &pd)) return false;
        uint32_t fam = 0;
        best = scoreDevice(pd, c.surface, fam);
        if (best >= 0) { c.phys = pd; c.queueFamily = fam; }
    } else {
        for (auto pd : devs) {
            uint32_t fam = 0;
            const int s = scoreDevice(pd, c.surface, fam);
            if (s > best) { best = s; c.phys = pd; c.queueFamily = fam; }
        }
    }
    if (best < 0) {
        std::fprintf(stderr, "[vk] no device offers Vulkan 1.3 + dynamic rendering + sync2 + multiview\n");
        return false;
    }
    vkGetPhysicalDeviceProperties(c.phys, &c.props);
    vkGetPhysicalDeviceMemoryProperties(c.phys, &c.memProps);
    vkGetPhysicalDeviceFeatures(c.phys, &c.features);
    {
        VkPhysicalDeviceVulkan11Properties p11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; p2.pNext = &p11;
        vkGetPhysicalDeviceProperties2(c.phys, &p2);
        c.maxMultiviewViews = p11.maxMultiviewViewCount;
    }
    c.hasWideLines = c.features.wideLines == VK_TRUE;
    c.hasSamplerAnisotropy = c.features.samplerAnisotropy == VK_TRUE;
    std::printf("[vk] %s (Vulkan %u.%u.%u), multiview x%u\n", c.props.deviceName,
                VK_API_VERSION_MAJOR(c.props.apiVersion), VK_API_VERSION_MINOR(c.props.apiVersion),
                VK_API_VERSION_PATCH(c.props.apiVersion), c.maxMultiviewViews);

    // --- device ---
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = c.queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = &f13;
    f12.shaderOutputLayer = VK_TRUE;   // optimistic; the query below overwrites it
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f11.pNext = &f12;
    f11.multiview = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f11;
    f2.features.samplerAnisotropy = c.hasSamplerAnisotropy ? VK_TRUE : VK_FALSE;
    f2.features.wideLines = VK_FALSE;              // never used: lines are SDF quads
    f2.features.independentBlend = c.features.independentBlend;
    f2.features.shaderClipDistance = VK_FALSE;
    f2.features.fragmentStoresAndAtomics = c.features.fragmentStoresAndAtomics;
    // shaderOutputLayer is only used if VR ever needs per-layer output from a
    // vertex shader; multiview covers our case. Clear it if unsupported -- a
    // driver does NOT ignore a feature bit it lacks that was requested here:
    // vkCreateDevice fails with VK_ERROR_FEATURE_NOT_PRESENT. So this query,
    // not the request above, is what actually decides the bit.
    {
        VkPhysicalDeviceVulkan12Features q12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 q2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; q2.pNext = &q12;
        vkGetPhysicalDeviceFeatures2(c.phys, &q2);
        f12.shaderOutputLayer = q12.shaderOutputLayer;
    }

    std::vector<const char*> dexts;
    if (!c.headless) dexts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    // Arcade pacing: present_id + present_wait, only when both are offered
    // AND the features are supported; otherwise plain FIFO with no wait.
    VkPhysicalDevicePresentIdFeaturesKHR   fPid{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
    VkPhysicalDevicePresentWaitFeaturesKHR fPwait{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
    c.hasPresentWait = false;
    if (d.presentWait && !c.headless &&
        hasDeviceExt(c.phys, VK_KHR_PRESENT_ID_EXTENSION_NAME) &&
        hasDeviceExt(c.phys, VK_KHR_PRESENT_WAIT_EXTENSION_NAME)) {
        VkPhysicalDevicePresentIdFeaturesKHR   qPid{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
        VkPhysicalDevicePresentWaitFeaturesKHR qPwait{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
        qPid.pNext = &qPwait;
        VkPhysicalDeviceFeatures2 q2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; q2.pNext = &qPid;
        vkGetPhysicalDeviceFeatures2(c.phys, &q2);
        if (qPid.presentId && qPwait.presentWait) {
            dexts.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
            dexts.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
            fPid.presentId = VK_TRUE;
            fPwait.presentWait = VK_TRUE;
            fPwait.pNext = f13.pNext;   // chain after the 1.3 block
            f13.pNext = &fPid;
            fPid.pNext = &fPwait;
            c.hasPresentWait = true;
        }
    }
    if (d.presentWait)
        std::printf("[vk] pacing: FIFO, %d frames in flight, present_wait %s\n", FRAMES_IN_FLIGHT,
                    c.hasPresentWait ? "on" : "unavailable (plain vsync)");

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)dexts.size();
    dci.ppEnabledExtensionNames = dexts.data();
    if (d.xr && d.xr->createDevice) {
        if (!d.xr->createDevice(d.xr->user, c.phys, &dci, &c.device)) return false;
    } else {
        VK_FAIL(vkCreateDevice(c.phys, &dci, nullptr, &c.device), "vkCreateDevice");
    }
    volkLoadDevice(c.device);
    vkGetDeviceQueue(c.device, c.queueFamily, 0, &c.queue);

    // --- command pool + per-frame sync ---
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = c.queueFamily;
    VK_FAIL(vkCreateCommandPool(c.device, &pci, nullptr, &c.cmdPool), "vkCreateCommandPool");
    for (int i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        FrameSync& f = c.frames[i];
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = c.cmdPool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VK_FAIL(vkAllocateCommandBuffers(c.device, &cai, &f.cmd), "vkAllocateCommandBuffers");
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_FAIL(vkCreateSemaphore(c.device, &si, nullptr, &f.imageAvailable), "semaphore");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_FAIL(vkCreateFence(c.device, &fi, nullptr, &f.inFlight), "fence");
    }

    // GPU timestamps for the pace readout (only when the queue supports them).
    c.tsPeriodNs = c.props.limits.timestampPeriod;
    if (c.props.limits.timestampComputeAndGraphics && c.tsPeriodNs > 0.0) {
        VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = 2 * FRAMES_IN_FLIGHT;
        if (vkCreateQueryPool(c.device, &qpi, nullptr, &c.tsPool) != VK_SUCCESS) c.tsPool = VK_NULL_HANDLE;
    }

    if (!c.headless && !createSwapchain(c)) return false;
    return true;
}

void destroy(Context& c) {
    if (c.device) vkDeviceWaitIdle(c.device);
    for (int i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        FrameSync& f = c.frames[i];
        if (f.imageAvailable) vkDestroySemaphore(c.device, f.imageAvailable, nullptr);
        if (f.inFlight) vkDestroyFence(c.device, f.inFlight, nullptr);
    }
    for (auto s : c.presentSemaphores) vkDestroySemaphore(c.device, s, nullptr);
    for (auto v : c.swapViews) vkDestroyImageView(c.device, v, nullptr);
    if (c.tsPool) vkDestroyQueryPool(c.device, c.tsPool, nullptr);
    if (c.swapchain) vkDestroySwapchainKHR(c.device, c.swapchain, nullptr);
    if (c.cmdPool) vkDestroyCommandPool(c.device, c.cmdPool, nullptr);
    if (c.device) vkDestroyDevice(c.device, nullptr);
    if (c.surface) vkDestroySurfaceKHR(c.instance, c.surface, nullptr);
    if (c.debugMessenger) vkDestroyDebugUtilsMessengerEXT(c.instance, c.debugMessenger, nullptr);
    if (c.instance) vkDestroyInstance(c.instance, nullptr);
    c = Context{};
}

bool recreateSwapchain(Context& c) {
    if (c.headless) return true;
    vkDeviceWaitIdle(c.device);
    return createSwapchain(c);
}

bool beginFrame(Context& c, VkCommandBuffer& cmdOut) {
    FrameSync& f = c.frames[c.frameIndex];
    vkWaitForFences(c.device, 1, &f.inFlight, VK_TRUE, UINT64_MAX);

    if (!c.headless) {
        if (c.needRecreate) {
            if (!recreateSwapchain(c)) return false;
        }
        VkResult r = vkAcquireNextImageKHR(c.device, c.swapchain, UINT64_MAX,
                                           f.imageAvailable, VK_NULL_HANDLE, &c.imageIndex);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) { c.needRecreate = true; return false; }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            std::fprintf(stderr, "[vk] vkAcquireNextImageKHR: %s\n", resultName(r));
            return false;
        }
    }
    vkResetFences(c.device, 1, &f.inFlight);
    vkResetCommandBuffer(f.cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmd, &bi);
    if (c.tsPool) {
        const uint32_t q = 2 * c.frameIndex;
        if (c.tsPending[c.frameIndex]) {
            // The fence wait above guarantees this slot's previous frame
            // (and its timestamps) finished on the GPU.
            uint64_t t[2] = {0, 0};
            if (vkGetQueryPoolResults(c.device, c.tsPool, q, 2, sizeof(t), t, sizeof(uint64_t),
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] >= t[0])
                c.lastGpuMs = (double)(t[1] - t[0]) * c.tsPeriodNs * 1e-6;
            c.tsPending[c.frameIndex] = false;
        }
        vkCmdResetQueryPool(f.cmd, c.tsPool, q, 2);
        vkCmdWriteTimestamp2(f.cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, c.tsPool, q);
    }
    cmdOut = f.cmd;
    c.frameBeginMs = nowMs();
    return true;
}

namespace {

// Fold one frame's timings into the per-second window; print + reset at 1 s.
void paceRecord(Context& c, double cpuMs, double waitMs, double presentMarkMs) {
    PaceStats& p = c.pace;
    if (p.frames == 0 && p.windowStartMs == 0) p.windowStartMs = presentMarkMs;
    p.cpuMin = std::min(p.cpuMin, cpuMs);   p.cpuMax = std::max(p.cpuMax, cpuMs);   p.cpuSum += cpuMs;
    if (c.lastGpuMs > 0.0) {
        p.gpuMin = std::min(p.gpuMin, c.lastGpuMs); p.gpuMax = std::max(p.gpuMax, c.lastGpuMs);
        p.gpuSum += c.lastGpuMs; ++p.gpuFrames;
    }
    p.waitMin = std::min(p.waitMin, waitMs); p.waitMax = std::max(p.waitMax, waitMs); p.waitSum += waitMs;
    if (c.lastPresentMs > 0) {
        const double iv = presentMarkMs - c.lastPresentMs;
        p.intMin = std::min(p.intMin, iv); p.intMax = std::max(p.intMax, iv); p.intSum += iv;
        if (iv > 25.0) ++p.late;
    }
    c.lastPresentMs = presentMarkMs;
    ++p.frames;
    if (presentMarkMs - p.windowStartMs >= 1000.0) {
        if (c.paceLog && p.frames > 0) {
            const double n = (double)p.frames;
            const double g = p.gpuFrames > 0 ? (double)p.gpuFrames : 1.0;
            std::fprintf(stderr, "[vk-pace] %d frames/s  cpu %.2f/%.2f/%.2f  gpu %.2f/%.2f/%.2f  wait %.2f/%.2f/%.2f  "
                                 "interval %.2f/%.2f/%.2f ms (min/avg/max)  late=%d  present_wait=%s\n",
                         p.frames, p.cpuMin, p.cpuSum / n, p.cpuMax,
                         p.gpuFrames > 0 ? p.gpuMin : 0.0, p.gpuSum / g, p.gpuMax,
                         p.waitMin, p.waitSum / n, p.waitMax,
                         p.frames > 1 ? p.intMin : 0.0, p.frames > 1 ? p.intSum / (n - 1.0) : 0.0,
                         p.frames > 1 ? p.intMax : 0.0,
                         p.late, c.hasPresentWait ? "on" : "off");
        }
        p = PaceStats{};
        p.windowStartMs = presentMarkMs;
    }
}

} // namespace

void endFrame(Context& c) {
    FrameSync& f = c.frames[c.frameIndex];
    if (c.tsPool) {
        vkCmdWriteTimestamp2(f.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, c.tsPool, 2 * c.frameIndex + 1);
        c.tsPending[c.frameIndex] = true;
    }
    vkEndCommandBuffer(f.cmd);

    VkSemaphore renderDone = c.headless ? VK_NULL_HANDLE : c.presentSemaphores[c.imageIndex];

    VkCommandBufferSubmitInfo csi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    csi.commandBuffer = f.cmd;
    VkSemaphoreSubmitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waitInfo.semaphore = f.imageAvailable;
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signalInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signalInfo.semaphore = renderDone;
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &csi;
    if (!c.headless) {
        si.waitSemaphoreInfoCount = 1;
        si.pWaitSemaphoreInfos = &waitInfo;
        si.signalSemaphoreInfoCount = 1;
        si.pSignalSemaphoreInfos = &signalInfo;
    }
    VkResult r = vkQueueSubmit2(c.queue, 1, &si, f.inFlight);
    if (r != VK_SUCCESS) std::fprintf(stderr, "[vk] vkQueueSubmit2: %s\n", resultName(r));

    if (!c.headless) {
        const double cpuMs = nowMs() - c.frameBeginMs;
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &renderDone;
        pi.swapchainCount = 1;
        pi.pSwapchains = &c.swapchain;
        pi.pImageIndices = &c.imageIndex;
        VkPresentIdKHR pid{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
        const uint64_t thisId = c.presentId + 1;
        if (c.hasPresentWait) {
            pid.swapchainCount = 1;
            pid.pPresentIds = &thisId;
            pi.pNext = &pid;
        }
        r = vkQueuePresentKHR(c.queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) c.needRecreate = true;
        else if (r != VK_SUCCESS) std::fprintf(stderr, "[vk] vkQueuePresentKHR: %s\n", resultName(r));
        double waitMs = 0.0;
        double markMs = nowMs();
        if (c.hasPresentWait && r != VK_ERROR_OUT_OF_DATE_KHR) {
            c.presentId = thisId;
            // Block until the PREVIOUS frame is on the glass: the CPU never
            // gets more than one frame ahead of the display, so input-to-
            // photon latency is bounded at ~2 frames and the sim's 16 ms
            // steps land one per vblank. A 100 ms timeout guards a stalled
            // compositor; it is not an error, the next frame simply catches up.
            if (thisId >= 2) {
                const double t0 = nowMs();
                const VkResult wr = vkWaitForPresentKHR(c.device, c.swapchain, thisId - 1, 100000000ull);
                markMs = nowMs();
                waitMs = markMs - t0;
                // A presentation path that never signals present ids (seen:
                // Mesa's software WSI under Xvfb) would turn every frame into
                // a 100 ms timeout. Three in a row and the wait is dropped for
                // the life of this Context -- plain FIFO vsync, as without the
                // extension. Logged once, and NOTHING re-arms it: hasPresentWait
                // is set true only in create(), and a swapchain recreate resets
                // presentWaitTimeouts alone, which is inert once the wait is
                // off. So the fallback lasts the rest of the run, resize
                // included.
                if (wr == VK_TIMEOUT || waitMs >= 95.0) {   // result code OR the whole budget spent
                    if (++c.presentWaitTimeouts >= 3) {
                        c.hasPresentWait = false;
                        std::fprintf(stderr, "[vk] present_wait: 3 consecutive 100 ms timeouts, "
                                             "falling back to plain FIFO pacing\n");
                    }
                } else {
                    c.presentWaitTimeouts = 0;
                }
            }
        }
        paceRecord(c, cpuMs, waitMs, markMs);
    }
    c.frameIndex = (c.frameIndex + 1) % FRAMES_IN_FLIGHT;
    ++c.frameCounter;
}

void waitIdle(Context& c) {
    if (c.device) vkDeviceWaitIdle(c.device);
}

VkCommandBuffer beginOneShot(Context& c) {
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = c.cmdPool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(c.device, &cai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

void endOneShot(Context& c, VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(c.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(c.queue);
    vkFreeCommandBuffers(c.device, c.cmdPool, 1, &cmd);
}

} // namespace vkctx
} // namespace ts
