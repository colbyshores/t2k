// ============================================================================
// xr_session.cpp — see xr_session.h.
// ============================================================================

#include "xr_session.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>

#include <volk.h>

namespace ts {
namespace xr {

namespace {

const char* resultName(const Session& s, XrResult r) {
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    if (s.instance != XR_NULL_HANDLE && s.fn.resultToString &&
        s.fn.resultToString(s.instance, r, buf) == XR_SUCCESS) return buf;
    std::snprintf(buf, sizeof(buf), "XrResult %d", (int)r);
    return buf;
}

#define XR_FAIL(expr, what) do { XrResult _r = (expr); if (XR_FAILED(_r)) { \
    std::fprintf(stderr, "[xr] %s failed: %s\n", what, resultName(s, _r)); return false; } } while (0)

template <typename T>
bool load(Session& s, const char* name, T& out) {
    PFN_xrVoidFunction f = nullptr;
    const XrResult r = s.fn.getInstanceProcAddr(s.instance, name, &f);
    if (XR_FAILED(r) || !f) {
        std::fprintf(stderr, "[xr] runtime has no %s\n", name);
        return false;
    }
    out = reinterpret_cast<T>(f);
    return true;
}

// Entry points that need no instance (loader-level).
bool loadLoaderFns(Session& s) {
    return load(s, "xrEnumerateInstanceExtensionProperties", s.fn.enumerateInstanceExtensionProperties) &&
           load(s, "xrCreateInstance", s.fn.createInstance);
}

bool loadInstanceFns(Session& s) {
    Fns& f = s.fn;
    return load(s, "xrDestroyInstance", f.destroyInstance) &&
           load(s, "xrGetInstanceProperties", f.getInstanceProperties) &&
           load(s, "xrResultToString", f.resultToString) &&
           load(s, "xrGetSystem", f.getSystem) &&
           load(s, "xrGetSystemProperties", f.getSystemProperties) &&
           load(s, "xrEnumerateViewConfigurationViews", f.enumerateViewConfigurationViews) &&
           load(s, "xrEnumerateEnvironmentBlendModes", f.enumerateEnvironmentBlendModes) &&
           load(s, "xrCreateSession", f.createSession) &&
           load(s, "xrDestroySession", f.destroySession) &&
           load(s, "xrBeginSession", f.beginSession) &&
           load(s, "xrEndSession", f.endSession) &&
           load(s, "xrRequestExitSession", f.requestExitSession) &&
           load(s, "xrCreateReferenceSpace", f.createReferenceSpace) &&
           load(s, "xrDestroySpace", f.destroySpace) &&
           load(s, "xrEnumerateSwapchainFormats", f.enumerateSwapchainFormats) &&
           load(s, "xrCreateSwapchain", f.createSwapchain) &&
           load(s, "xrDestroySwapchain", f.destroySwapchain) &&
           load(s, "xrEnumerateSwapchainImages", f.enumerateSwapchainImages) &&
           load(s, "xrAcquireSwapchainImage", f.acquireSwapchainImage) &&
           load(s, "xrWaitSwapchainImage", f.waitSwapchainImage) &&
           load(s, "xrReleaseSwapchainImage", f.releaseSwapchainImage) &&
           load(s, "xrPollEvent", f.pollEvent) &&
           load(s, "xrWaitFrame", f.waitFrame) &&
           load(s, "xrBeginFrame", f.beginFrame) &&
           load(s, "xrEndFrame", f.endFrame) &&
           load(s, "xrLocateViews", f.locateViews) &&
           load(s, "xrGetVulkanGraphicsRequirements2KHR", f.getVulkanGraphicsRequirements2) &&
           load(s, "xrCreateVulkanInstanceKHR", f.createVulkanInstance) &&
           load(s, "xrGetVulkanGraphicsDevice2KHR", f.getVulkanGraphicsDevice2) &&
           load(s, "xrCreateVulkanDeviceKHR", f.createVulkanDevice);
}

bool openLoader(Session& s) {
    if (s.loader) return true;
    s.loader = dlopen("libopenxr_loader.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!s.loader) s.loader = dlopen("libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL);
    if (!s.loader) {
        std::fprintf(stderr, "[xr] no OpenXR loader (libopenxr_loader.so.1) on this machine: running flat\n");
        return false;
    }
    s.fn.getInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(dlsym(s.loader, "xrGetInstanceProcAddr"));
    if (!s.fn.getInstanceProcAddr) {
        std::fprintf(stderr, "[xr] loader has no xrGetInstanceProcAddr\n");
        return false;
    }
    return true;
}

bool hasInstanceExt(Session& s, const char* name) {
    uint32_t n = 0;
    if (XR_FAILED(s.fn.enumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr))) return false;
    std::vector<XrExtensionProperties> exts(n, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    if (XR_FAILED(s.fn.enumerateInstanceExtensionProperties(nullptr, n, &n, exts.data()))) return false;
    for (const auto& e : exts) if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

} // namespace

bool loaderAvailable() {
    void* h = dlopen("libopenxr_loader.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h) return false;
    dlclose(h);
    return true;
}

bool createInstance(Session& s, const Desc& d) {
    s = Session{};
    if (!openLoader(s)) return false;
    if (!loadLoaderFns(s)) return false;

    // The one extension this needs is the Vulkan bridge. A loader with no
    // active runtime fails right here (XR_ERROR_RUNTIME_UNAVAILABLE), which
    // is the "no headset software installed" case.
    if (!hasInstanceExt(s, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) {
        std::fprintf(stderr, "[xr] runtime lacks %s (or no runtime is active): running flat\n",
                     XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
        return false;
    }
    const char* exts[1] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    std::snprintf(ici.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "%s", d.appName);
    ici.applicationInfo.applicationVersion = 1;
    std::snprintf(ici.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "Tube Shooter");
    ici.applicationInfo.engineVersion = 1;
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = exts;
    const char* layers[1] = {"XR_APILAYER_LUNARG_core_validation"};
    if (d.validation) { ici.enabledApiLayerCount = 1; ici.enabledApiLayerNames = layers; }
    XrResult r = s.fn.createInstance(&ici, &s.instance);
    if (XR_FAILED(r) && d.validation) {
        // The validation layer is optional; retry without it.
        ici.enabledApiLayerCount = 0;
        r = s.fn.createInstance(&ici, &s.instance);
    }
    if (XR_FAILED(r)) {
        std::fprintf(stderr, "[xr] xrCreateInstance failed (%d): no active runtime? running flat\n", (int)r);
        s.instance = XR_NULL_HANDLE;
        return false;
    }
    if (!loadInstanceFns(s)) return false;

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(s.fn.getInstanceProperties(s.instance, &ip)))
        std::snprintf(s.runtimeName, sizeof(s.runtimeName), "%s", ip.runtimeName);

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = s.fn.getSystem(s.instance, &sgi, &s.system);
    if (XR_FAILED(r)) {
        std::fprintf(stderr, "[xr] no head-mounted display available (%s): running flat\n", resultName(s, r));
        return false;
    }
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(s.fn.getSystemProperties(s.instance, s.system, &sp)))
        std::snprintf(s.systemName, sizeof(s.systemName), "%s", sp.systemName);

    // The runtime's Vulkan version window; 1.3 must fit inside it.
    XrGraphicsRequirementsVulkan2KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    XR_FAIL(s.fn.getVulkanGraphicsRequirements2(s.instance, s.system, &req), "xrGetVulkanGraphicsRequirements2KHR");
    const uint32_t want = XR_MAKE_VERSION(1, 3, 0);
    if (want < req.minApiVersionSupported || XR_VERSION_MAJOR(req.maxApiVersionSupported) < 1) {
        std::fprintf(stderr, "[xr] runtime wants Vulkan %u.%u..%u.%u, this renderer is 1.3: running flat\n",
                     (unsigned)XR_VERSION_MAJOR(req.minApiVersionSupported), (unsigned)XR_VERSION_MINOR(req.minApiVersionSupported),
                     (unsigned)XR_VERSION_MAJOR(req.maxApiVersionSupported), (unsigned)XR_VERSION_MINOR(req.maxApiVersionSupported));
        return false;
    }

    // Per-eye size from the primary stereo view configuration.
    uint32_t nv = 0;
    XR_FAIL(s.fn.enumerateViewConfigurationViews(s.instance, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &nv, nullptr),
            "xrEnumerateViewConfigurationViews");
    if (nv < VIEW_COUNT) { std::fprintf(stderr, "[xr] system is not stereo (%u views)\n", nv); return false; }
    std::vector<XrViewConfigurationView> vcv(nv, XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XR_FAIL(s.fn.enumerateViewConfigurationViews(s.instance, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, nv, &nv, vcv.data()),
            "xrEnumerateViewConfigurationViews");
    s.eyeWidth = vcv[0].recommendedImageRectWidth;
    s.eyeHeight = vcv[0].recommendedImageRectHeight;
    // Both eyes share one array image, so both layers are the larger of the two.
    if (vcv[1].recommendedImageRectWidth > s.eyeWidth) s.eyeWidth = vcv[1].recommendedImageRectWidth;
    if (vcv[1].recommendedImageRectHeight > s.eyeHeight) s.eyeHeight = vcv[1].recommendedImageRectHeight;

    uint32_t nb = 0;
    if (XR_SUCCEEDED(s.fn.enumerateEnvironmentBlendModes(s.instance, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &nb, nullptr)) && nb > 0) {
        std::vector<XrEnvironmentBlendMode> modes(nb);
        s.fn.enumerateEnvironmentBlendModes(s.instance, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, nb, &nb, modes.data());
        s.blendMode = modes[0];   // the runtime's preferred; OPAQUE on every HMD
    }
    std::printf("[xr] %s / %s, %ux%u per eye\n", s.runtimeName, s.systemName, s.eyeWidth, s.eyeHeight);
    return true;
}

bool hookCreateVkInstance(void* user, const VkInstanceCreateInfo* ci, VkInstance* out) {
    Session& s = *static_cast<Session*>(user);
    XrVulkanInstanceCreateInfoKHR xci{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xci.systemId = s.system;
    xci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;   // volk's, after volkInitialize
    xci.vulkanCreateInfo = ci;
    VkResult vr = VK_SUCCESS;
    XR_FAIL(s.fn.createVulkanInstance(s.instance, &xci, out, &vr), "xrCreateVulkanInstanceKHR");
    if (vr != VK_SUCCESS) { std::fprintf(stderr, "[xr] runtime's vkCreateInstance failed (%d)\n", (int)vr); return false; }
    return true;
}

bool hookPickVkPhysicalDevice(void* user, VkInstance inst, VkPhysicalDevice* out) {
    Session& s = *static_cast<Session*>(user);
    XrVulkanGraphicsDeviceGetInfoKHR gi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    gi.systemId = s.system;
    gi.vulkanInstance = inst;
    XR_FAIL(s.fn.getVulkanGraphicsDevice2(s.instance, &gi, out), "xrGetVulkanGraphicsDevice2KHR");
    return true;
}

bool hookCreateVkDevice(void* user, VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, VkDevice* out) {
    Session& s = *static_cast<Session*>(user);
    XrVulkanDeviceCreateInfoKHR dci{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    dci.systemId = s.system;
    dci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    dci.vulkanPhysicalDevice = pd;
    dci.vulkanCreateInfo = ci;
    VkResult vr = VK_SUCCESS;
    XR_FAIL(s.fn.createVulkanDevice(s.instance, &dci, out, &vr), "xrCreateVulkanDeviceKHR");
    if (vr != VK_SUCCESS) { std::fprintf(stderr, "[xr] runtime's vkCreateDevice failed (%d)\n", (int)vr); return false; }
    return true;
}

bool createSession(Session& s, VkInstance inst, VkPhysicalDevice pd, VkDevice dev,
                   uint32_t queueFamily, uint32_t queueIndex,
                   const VkFormat* preferred, int preferredCount) {
    XrGraphicsBindingVulkan2KHR gb{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    gb.instance = inst;
    gb.physicalDevice = pd;
    gb.device = dev;
    gb.queueFamilyIndex = queueFamily;
    gb.queueIndex = queueIndex;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = s.system;
    XR_FAIL(s.fn.createSession(s.instance, &sci, &s.session), "xrCreateSession");

    // LOCAL space: the seated origin. The game's camera IS the seat -- the
    // tube's eye position -- so the headset's pose composes on top of it and
    // looking around the cockpit is free. (STAGE would put the origin on the
    // floor; the tube has no floor.)
    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    XR_FAIL(s.fn.createReferenceSpace(s.session, &rsci, &s.space), "xrCreateReferenceSpace");

    // Swapchain format: the first preferred one the runtime offers. The
    // composite writes a DISPLAY value converted to linear (composite.frag),
    // so the format must be sRGB for the runtime's encode to cancel it.
    uint32_t nf = 0;
    XR_FAIL(s.fn.enumerateSwapchainFormats(s.session, 0, &nf, nullptr), "xrEnumerateSwapchainFormats");
    std::vector<int64_t> fmts(nf);
    XR_FAIL(s.fn.enumerateSwapchainFormats(s.session, nf, &nf, fmts.data()), "xrEnumerateSwapchainFormats");
    s.swapFormat = VK_FORMAT_UNDEFINED;
    for (int p = 0; p < preferredCount && s.swapFormat == VK_FORMAT_UNDEFINED; ++p)
        for (int64_t f : fmts) if (f == (int64_t)preferred[p]) { s.swapFormat = preferred[p]; break; }
    if (s.swapFormat == VK_FORMAT_UNDEFINED) {
        std::fprintf(stderr, "[xr] runtime offers no sRGB 8-bit swapchain format\n");
        return false;
    }

    XrSwapchainCreateInfo swci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
    swci.format = (int64_t)s.swapFormat;
    swci.sampleCount = 1;
    swci.width = s.eyeWidth;
    swci.height = s.eyeHeight;
    swci.faceCount = 1;
    swci.arraySize = VIEW_COUNT;      // ONE array image: the multiview target
    swci.mipCount = 1;
    XR_FAIL(s.fn.createSwapchain(s.session, &swci, &s.swapchain), "xrCreateSwapchain");

    uint32_t ni = 0;
    XR_FAIL(s.fn.enumerateSwapchainImages(s.swapchain, 0, &ni, nullptr), "xrEnumerateSwapchainImages");
    std::vector<XrSwapchainImageVulkan2KHR> imgs(ni, XrSwapchainImageVulkan2KHR{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    XR_FAIL(s.fn.enumerateSwapchainImages(s.swapchain, ni, &ni,
                                          reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data())),
            "xrEnumerateSwapchainImages");
    s.images.resize(ni);
    for (uint32_t i = 0; i < ni; ++i) s.images[i] = imgs[i].image;
    std::printf("[xr] session up: %u swapchain images, %ux%u x2 layers\n", ni, s.eyeWidth, s.eyeHeight);
    return true;
}

void pollEvents(Session& s) {
    if (s.instance == XR_NULL_HANDLE || !s.fn.pollEvent) return;
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (s.fn.pollEvent(s.instance, &ev) == XR_SUCCESS) {
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const auto* sc = reinterpret_cast<const XrEventDataSessionStateChanged*>(&ev);
            s.state = sc->state;
            switch (s.state) {
            case XR_SESSION_STATE_READY: {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(s.fn.beginSession(s.session, &bi))) s.running = true;
                break;
            }
            case XR_SESSION_STATE_STOPPING:
                if (s.running) s.fn.endSession(s.session);
                s.running = false;
                break;
            case XR_SESSION_STATE_EXITING:
            case XR_SESSION_STATE_LOSS_PENDING:
                s.running = false;
                s.exitRequested = true;
                break;
            default: break;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            s.exitRequested = true;
            break;
        default: break;
        }
        ev = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    }
}

bool beginFrame(Session& s) {
    if (!s.running || s.session == XR_NULL_HANDLE) return false;
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    s.frameState = XrFrameState{XR_TYPE_FRAME_STATE};
    if (XR_FAILED(s.fn.waitFrame(s.session, &wi, &s.frameState))) return false;
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(s.fn.beginFrame(s.session, &bi))) return false;
    s.frameOpen = true;
    s.viewsValid = false;
    s.imageAcquired = false;
    if (!s.frameState.shouldRender) return false;   // the caller must still endFrame(s, false)

    XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = s.frameState.predictedDisplayTime;
    vli.space = s.space;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    uint32_t nv = 0;
    for (uint32_t i = 0; i < VIEW_COUNT; ++i) s.views[i] = XrView{XR_TYPE_VIEW};
    if (XR_FAILED(s.fn.locateViews(s.session, &vli, &vs, VIEW_COUNT, &nv, s.views)) || nv < VIEW_COUNT) return false;
    // Without a valid pose (tracking lost) the last-known views are kept but
    // the frame still renders; the runtime reprojects.
    s.viewsValid = (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;

    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(s.fn.acquireSwapchainImage(s.swapchain, &ai, &s.imageIndex))) return false;
    XrSwapchainImageWaitInfo swi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    swi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(s.fn.waitSwapchainImage(s.swapchain, &swi))) {
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        s.fn.releaseSwapchainImage(s.swapchain, &ri);
        return false;
    }
    s.imageAcquired = true;
    return true;
}

void endFrame(Session& s, bool rendered) {
    if (!s.frameOpen) return;
    if (s.imageAcquired) {
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        s.fn.releaseSwapchainImage(s.swapchain, &ri);
        s.imageAcquired = false;
    }
    XrCompositionLayerProjectionView pv[VIEW_COUNT];
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader* layers[1] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = s.frameState.predictedDisplayTime;
    ei.environmentBlendMode = s.blendMode;
    if (rendered && s.viewsValid) {
        for (uint32_t i = 0; i < VIEW_COUNT; ++i) {
            pv[i] = XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            pv[i].pose = s.views[i].pose;
            pv[i].fov = s.views[i].fov;
            pv[i].subImage.swapchain = s.swapchain;
            pv[i].subImage.imageRect.offset = {0, 0};
            pv[i].subImage.imageRect.extent = {(int32_t)s.eyeWidth, (int32_t)s.eyeHeight};
            pv[i].subImage.imageArrayIndex = i;    // layer i of the ONE array image
        }
        layer.space = s.space;
        layer.viewCount = VIEW_COUNT;
        layer.views = pv;
        ei.layerCount = 1;
        ei.layers = layers;
    }
    s.fn.endFrame(s.session, &ei);
    s.frameOpen = false;
}

void destroy(Session& s) {
    if (s.session != XR_NULL_HANDLE) {
        if (s.frameOpen) endFrame(s, false);
        if (s.swapchain != XR_NULL_HANDLE) s.fn.destroySwapchain(s.swapchain);
        if (s.space != XR_NULL_HANDLE) s.fn.destroySpace(s.space);
        if (s.running) s.fn.endSession(s.session);
        s.fn.destroySession(s.session);
    }
    if (s.instance != XR_NULL_HANDLE && s.fn.destroyInstance) s.fn.destroyInstance(s.instance);
    if (s.loader) dlclose(s.loader);
    s = Session{};
}

// ---- math ---------------------------------------------------------------------

void projectionFromFov(const XrFovf& fov, float n, float f, float* m) {
    // Asymmetric frustum from the four half-angles, in the same clip
    // conventions as renderer_vk.cpp's perspectiveVk: Vulkan y down, depth
    // [0,1], camera looking down -Z. Column-major (glm layout), m[col*4+row].
    const float tl = std::tan(fov.angleLeft), tr = std::tan(fov.angleRight);
    const float tu = std::tan(fov.angleUp),   td = std::tan(fov.angleDown);
    for (int i = 0; i < 16; ++i) m[i] = 0.0f;
    m[0 * 4 + 0] = 2.0f / (tr - tl);
    m[2 * 4 + 0] = (tr + tl) / (tr - tl);
    m[1 * 4 + 1] = -2.0f / (tu - td);
    m[2 * 4 + 1] = -(tu + td) / (tu - td);
    m[2 * 4 + 2] = f / (n - f);
    m[2 * 4 + 3] = -1.0f;
    m[3 * 4 + 2] = -(f * n) / (f - n);
}

void viewFromPose(const XrPosef& p, float unitsPerMetre, float* m) {
    // eye-from-space = R^T * T(-pos): the inverse of the rigid pose.
    const float x = p.orientation.x, y = p.orientation.y, z = p.orientation.z, w = p.orientation.w;
    // Rotation matrix R (column-major) from the unit quaternion.
    const float r00 = 1 - 2 * (y * y + z * z), r01 = 2 * (x * y - z * w),     r02 = 2 * (x * z + y * w);
    const float r10 = 2 * (x * y + z * w),     r11 = 1 - 2 * (x * x + z * z), r12 = 2 * (y * z - x * w);
    const float r20 = 2 * (x * z - y * w),     r21 = 2 * (y * z + x * w),     r22 = 1 - 2 * (x * x + y * y);
    const float px = p.position.x * unitsPerMetre, py = p.position.y * unitsPerMetre, pz = p.position.z * unitsPerMetre;
    // R^T rows become columns; translation = -R^T * pos.
    m[0 * 4 + 0] = r00; m[0 * 4 + 1] = r01; m[0 * 4 + 2] = r02; m[0 * 4 + 3] = 0.0f;
    m[1 * 4 + 0] = r10; m[1 * 4 + 1] = r11; m[1 * 4 + 2] = r12; m[1 * 4 + 3] = 0.0f;
    m[2 * 4 + 0] = r20; m[2 * 4 + 1] = r21; m[2 * 4 + 2] = r22; m[2 * 4 + 3] = 0.0f;
    m[3 * 4 + 0] = -(r00 * px + r10 * py + r20 * pz);
    m[3 * 4 + 1] = -(r01 * px + r11 * py + r21 * pz);
    m[3 * 4 + 2] = -(r02 * px + r12 * py + r22 * pz);
    m[3 * 4 + 3] = 1.0f;
}

} // namespace xr
} // namespace ts
