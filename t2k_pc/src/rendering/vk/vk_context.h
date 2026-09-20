#pragma once

// ============================================================================
// vk_context.h — Vulkan 1.3 device + swapchain + frames-in-flight for the
// desktop renderer. Platform plumbing only: nothing above this layer sees SDL,
// and nothing here knows what a web or a claw is.
//
// Design (DOCTRINE.md "C with classes"): one plain struct, free functions, no
// virtuals, no exceptions. Every failure is a `false` return with the reason
// on stderr. Init-time std::vector use is fine; nothing here allocates on the
// frame path.
//
// The loader is volk (dlopen of libvulkan.so.1), so the binary carries no link
// dependency on libvulkan: a machine without a Vulkan ICD gets a clear
// message instead of a missing-.so failure at exec time.
// ============================================================================

#include <stdint.h>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

struct SDL_Window;

namespace ts {
namespace vkctx {

constexpr int FRAMES_IN_FLIGHT = 2;

struct FrameSync {
    VkSemaphore     imageAvailable = VK_NULL_HANDLE;
    VkSemaphore     renderFinished = VK_NULL_HANDLE;
    VkFence         inFlight       = VK_NULL_HANDLE;
    VkCommandBuffer cmd            = VK_NULL_HANDLE;
};

// OpenXR (XR_KHR_vulkan_enable2) dictates the instance, the physical device
// and the device: when a session is up, the runtime creates them from OUR
// create-infos (adding what it needs). Plain function pointers + a user
// pointer (no std::function, no vtable -- the Menu::Hooks pattern);
// xr_session.h implements the three. Null = ordinary desktop creation.
struct XrVkHooks {
    void* user = nullptr;
    bool (*createInstance)(void* user, const VkInstanceCreateInfo* ci, VkInstance* out) = nullptr;
    bool (*pickPhysicalDevice)(void* user, VkInstance inst, VkPhysicalDevice* out) = nullptr;
    bool (*createDevice)(void* user, VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, VkDevice* out) = nullptr;
};

struct Desc {
    // Null window = headless (no surface, no swapchain). Used by the offline
    // harnesses (--stereo-dump). Under OpenXR the window is the MIRROR: the
    // headset's swapchain is the render target, the window shows one eye.
    SDL_Window* window = nullptr;
    const XrVkHooks* xr = nullptr;
    // FIFO (vsync-locked, the arcade default) vs MAILBOX (uncapped, lowest
    // latency when the driver has it) vs IMMEDIATE.
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    bool validation = false;   // VK_LAYER_KHRONOS_validation if installed
    // Tight pacing (arcade mode): enable VK_KHR_present_id + VK_KHR_present_wait
    // when the device offers them and block in endFrame until the PREVIOUS
    // frame is on the glass, so the CPU runs at most one frame ahead of the
    // display. Silently a no-op on a device without the extensions.
    bool presentWait = false;
    const char* appName = "T2K";
};

// Per-second frame pacing statistics (endFrame keeps them; printed under
// T2K_DEBUG=1 as "[vk-pace]"). Milliseconds. `cpu` is the wall time between
// beginFrame returning and the present call (game + record); `wait` is the
// time blocked in vkWaitForPresentKHR (0 without present_wait); `interval` is
// the spacing between successive presents reaching the display (with
// present_wait) or between successive present calls (without).
struct PaceStats {
    double cpuMin = 1e9, cpuMax = 0, cpuSum = 0;
    double gpuMin = 1e9, gpuMax = 0, gpuSum = 0;   // GPU timestamps, top -> bottom of the frame
    int    gpuFrames = 0;
    double waitMin = 1e9, waitMax = 0, waitSum = 0;
    double intMin = 1e9, intMax = 0, intSum = 0;
    int    frames = 0;
    int    late = 0;          // intervals > 25 ms (a missed vblank at 60 Hz)
    double windowStartMs = 0; // wall clock when this window began
};

struct Context {
    VkInstance       instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    VkPhysicalDevice phys     = VK_NULL_HANDLE;
    VkDevice         device   = VK_NULL_HANDLE;
    uint32_t         queueFamily = 0;
    VkQueue          queue    = VK_NULL_HANDLE;

    VkPhysicalDeviceProperties       props{};
    VkPhysicalDeviceMemoryProperties memProps{};
    VkPhysicalDeviceFeatures         features{};
    uint32_t maxMultiviewViews = 1;
    bool hasWideLines = false;
    bool hasSamplerAnisotropy = false;

    // Presentation (absent when headless).
    VkSurfaceKHR     surface   = VK_NULL_HANDLE;
    VkSwapchainKHR   swapchain = VK_NULL_HANDLE;
    VkFormat         swapFormat = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR  swapColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D       swapExtent{0, 0};
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkImage>     swapImages;
    std::vector<VkImageView> swapViews;
    // One "rendered" semaphore PER SWAPCHAIN IMAGE (not per frame): the
    // present engine may still be waiting on an image's semaphore when the
    // frame slot that signalled it comes round again.
    std::vector<VkSemaphore> presentSemaphores;
    bool needRecreate = false;
    bool headless = false;

    // VK_KHR_present_wait pacing (Desc::presentWait); see PaceStats.
    bool     hasPresentWait = false;
    int      presentWaitTimeouts = 0; // consecutive VK_TIMEOUTs; 3 disable the wait
    uint64_t presentId = 0;          // id of the last frame presented (1-based)
    bool     paceLog = false;        // T2K_DEBUG set: print [vk-pace] once a second
    double   frameBeginMs = 0;       // wall clock at beginFrame return
    double   lastPresentMs = 0;      // wall clock of the previous frame's present mark
    PaceStats pace;
    // GPU frame time: two timestamps per frame slot (top of pipe at begin,
    // all-commands at end), read back when the slot's fence has been waited.
    VkQueryPool tsPool = VK_NULL_HANDLE;
    bool   tsPending[FRAMES_IN_FLIGHT] = {};
    double tsPeriodNs = 0.0;          // props.limits.timestampPeriod
    double lastGpuMs = 0.0;

    VkCommandPool cmdPool = VK_NULL_HANDLE;
    FrameSync frames[FRAMES_IN_FLIGHT];
    uint32_t frameIndex = 0;     // 0..FRAMES_IN_FLIGHT-1, the slot in use
    uint32_t imageIndex = 0;     // swapchain image acquired this frame
    uint64_t frameCounter = 0;   // monotonically increasing

    SDL_Window* window = nullptr;
};

bool create(Context& c, const Desc& d);
void destroy(Context& c);

// Recreate the swapchain for the window's current size (after a resize or an
// out-of-date result). Waits for the device to go idle. No-op when headless.
bool recreateSwapchain(Context& c);

// Wait for this frame slot's fence, acquire a swapchain image (unless
// headless), and begin the slot's command buffer. Returns false when the frame
// must be skipped (swapchain being recreated); the caller just tries again
// next loop iteration.
bool beginFrame(Context& c, VkCommandBuffer& cmdOut);

// End + submit the command buffer, present the acquired image (unless
// headless), and advance the frame slot. There is no timeline-semaphore hook
// here: the submit signals a plain binary semaphore, and the XR path ends its
// own frame separately through xr::endFrame right after this returns
// (renderer_vk.cpp frameEnd).
void endFrame(Context& c);

// Block until the GPU has finished everything (init-time uploads, teardown).
void waitIdle(Context& c);

// One-shot command buffer for init-time work (uploads, layout transitions).
VkCommandBuffer beginOneShot(Context& c);
void endOneShot(Context& c, VkCommandBuffer cmd);

} // namespace vkctx
} // namespace ts
