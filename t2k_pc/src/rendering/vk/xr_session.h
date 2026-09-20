#pragma once

// ============================================================================
// xr_session.h — OpenXR for the desktop renderer: the headset as a runtime
// MODE of the one Vulkan renderer (never a second renderer).
//
// Platform plumbing only, like vk_context.h: nothing here knows what a web
// or a claw is, and nothing above the renderer sees an XrSession. Design
// (DOCTRINE.md "C with classes"): one plain struct, free functions, no
// virtuals, no exceptions; every failure is a `false` return with the reason
// on stderr. Init-time std::vector is fine; the frame path allocates nothing.
//
// THE LOADER IS NOT LINKED. Exactly as volk dlopens libvulkan.so.1, this
// dlopens libopenxr_loader.so.1 at runtime and pulls every entry point
// through xrGetInstanceProcAddr (XR_NO_PROTOTYPES). A machine with no
// loader -- or a loader with no active runtime -- gets a one-line reason and
// the game runs flat; the binary never fails to exec over VR.
//
// Vulkan is created THROUGH the runtime (XR_KHR_vulkan_enable2): the runtime
// dictates the VkInstance / VkPhysicalDevice / VkDevice, so vk_context.h
// takes three hooks (vkctx::XrVkHooks) that this file implements. The order
// is: createInstance (XR) -> vkctx::create (calls back into the hooks) ->
// createSession (needs the finished VkDevice + queue).
//
// The frame loop is the OpenXR one and nothing else paces a VR frame:
//   pollEvents -> waitFrame (blocks to the headset's display rate)
//   -> beginFrame (locate both views, acquire + wait the swapchain image)
//   -> [render both eyes in one multiview pass] -> endFrame (release, submit
//   ONE projection layer with two views).
// The swapchain is ONE array image (2 layers, sRGB), the multiview target.
// ============================================================================

#include <stdint.h>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#define XR_NO_PROTOTYPES
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace ts {
namespace xr {

constexpr uint32_t VIEW_COUNT = 2;   // XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO

// Every loader / runtime entry point this file uses, resolved once.
struct Fns {
    PFN_xrGetInstanceProcAddr                 getInstanceProcAddr = nullptr;
    PFN_xrEnumerateInstanceExtensionProperties enumerateInstanceExtensionProperties = nullptr;
    PFN_xrCreateInstance                      createInstance = nullptr;
    PFN_xrDestroyInstance                     destroyInstance = nullptr;
    PFN_xrGetInstanceProperties               getInstanceProperties = nullptr;
    PFN_xrResultToString                      resultToString = nullptr;
    PFN_xrGetSystem                           getSystem = nullptr;
    PFN_xrGetSystemProperties                 getSystemProperties = nullptr;
    PFN_xrEnumerateViewConfigurationViews     enumerateViewConfigurationViews = nullptr;
    PFN_xrEnumerateEnvironmentBlendModes      enumerateEnvironmentBlendModes = nullptr;
    PFN_xrCreateSession                       createSession = nullptr;
    PFN_xrDestroySession                      destroySession = nullptr;
    PFN_xrBeginSession                        beginSession = nullptr;
    PFN_xrEndSession                          endSession = nullptr;
    PFN_xrRequestExitSession                  requestExitSession = nullptr;
    PFN_xrCreateReferenceSpace                createReferenceSpace = nullptr;
    PFN_xrDestroySpace                        destroySpace = nullptr;
    PFN_xrEnumerateSwapchainFormats           enumerateSwapchainFormats = nullptr;
    PFN_xrCreateSwapchain                     createSwapchain = nullptr;
    PFN_xrDestroySwapchain                    destroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages            enumerateSwapchainImages = nullptr;
    PFN_xrAcquireSwapchainImage               acquireSwapchainImage = nullptr;
    PFN_xrWaitSwapchainImage                  waitSwapchainImage = nullptr;
    PFN_xrReleaseSwapchainImage               releaseSwapchainImage = nullptr;
    PFN_xrPollEvent                           pollEvent = nullptr;
    PFN_xrWaitFrame                           waitFrame = nullptr;
    PFN_xrBeginFrame                          beginFrame = nullptr;
    PFN_xrEndFrame                            endFrame = nullptr;
    PFN_xrLocateViews                         locateViews = nullptr;
    // XR_KHR_vulkan_enable2
    PFN_xrGetVulkanGraphicsRequirements2KHR   getVulkanGraphicsRequirements2 = nullptr;
    PFN_xrCreateVulkanInstanceKHR             createVulkanInstance = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR         getVulkanGraphicsDevice2 = nullptr;
    PFN_xrCreateVulkanDeviceKHR               createVulkanDevice = nullptr;
};

struct Desc {
    const char* appName = "T2K";
    bool validation = false;   // XR_APILAYER_LUNARG_core_validation if installed
};

struct Session {
    void*       loader = nullptr;          // dlopen handle
    Fns         fn;
    XrInstance  instance = XR_NULL_HANDLE;
    XrSystemId  system = XR_NULL_SYSTEM_ID;
    XrSession   session = XR_NULL_HANDLE;
    XrSpace     space = XR_NULL_HANDLE;     // LOCAL: seated origin, the tube's seat
    XrSwapchain swapchain = XR_NULL_HANDLE;
    VkFormat    swapFormat = VK_FORMAT_UNDEFINED;
    uint32_t    eyeWidth = 0, eyeHeight = 0; // the runtime's recommended per-eye size
    std::vector<VkImage> images;            // the array images (2 layers each)
    uint32_t    imageIndex = 0;             // acquired this frame
    bool        imageAcquired = false;

    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;                  // between xrBeginSession and xrEndSession
    bool exitRequested = false;            // EXITING / LOSS_PENDING seen
    bool frameOpen = false;                // between xrBeginFrame and xrEndFrame
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    XrView views[VIEW_COUNT];
    bool viewsValid = false;
    XrEnvironmentBlendMode blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    char runtimeName[XR_MAX_RUNTIME_NAME_SIZE] = {};
    char systemName[XR_MAX_SYSTEM_NAME_SIZE] = {};
};

// True when libopenxr_loader.so.1 can be dlopened (says nothing about a runtime).
bool loaderAvailable();

// Phase 1: loader + instance + system + Vulkan graphics requirements. On any
// failure (no loader, no runtime, no headset, no vulkan_enable2) returns
// false with the reason on stderr; `s` is left destroyable.
bool createInstance(Session& s, const Desc& d);

// The three vkctx::XrVkHooks entry points (user = Session*). The instance /
// device create-infos are the renderer's own; the runtime appends what it
// needs and creates the object.
bool hookCreateVkInstance(void* user, const VkInstanceCreateInfo* ci, VkInstance* out);
bool hookPickVkPhysicalDevice(void* user, VkInstance inst, VkPhysicalDevice* out);
bool hookCreateVkDevice(void* user, VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, VkDevice* out);

// Phase 2: session + reference space + the 2-layer swapchain, once the device
// exists. `preferred` formats are tried in order against the runtime's list.
bool createSession(Session& s, VkInstance inst, VkPhysicalDevice pd, VkDevice dev,
                   uint32_t queueFamily, uint32_t queueIndex,
                   const VkFormat* preferred, int preferredCount);

// Per frame, in this order. pollEvents drives session state (READY ->
// xrBeginSession, STOPPING -> xrEndSession, EXITING -> exitRequested).
void pollEvents(Session& s);
// xrWaitFrame + xrBeginFrame + xrLocateViews + acquire/wait the swapchain
// image. Returns false when no frame should be rendered this iteration (the
// session is not running, or the runtime says shouldRender == false -- in
// that case the frame has still been opened and endFrame(s, false) must
// follow so the wait/begin/end pairing holds).
bool beginFrame(Session& s);
// Release the image (if acquired) and submit one projection layer of both
// views (`rendered`), or an empty frame.
void endFrame(Session& s, bool rendered);

void destroy(Session& s);

// The eye's asymmetric FOV -> a Vulkan projection (y down, depth [0,1]),
// column-major like glm. `out` is 16 floats.
void projectionFromFov(const XrFovf& fov, float nearZ, float farZ, float* out16);
// The eye pose (position in metres, orientation) -> the eye-from-space view
// matrix, with `unitsPerMetre` scaling the translation into world units.
void viewFromPose(const XrPosef& pose, float unitsPerMetre, float* out16);

} // namespace xr
} // namespace ts
