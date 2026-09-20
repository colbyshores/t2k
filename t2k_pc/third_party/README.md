# t2k_pc/third_party — desktop-only vendored dependencies

Pinned copies, checked in (the same policy t2k_core/third_party applies to glm
and nlohmann: the build never searches the system for these).

| dir       | what                                   | upstream                                   | licence      |
|-----------|----------------------------------------|--------------------------------------------|--------------|
| `vulkan/` | Vulkan C headers (`vulkan/*.h`, `vk_video/*.h`) — VK_HEADER_VERSION 360 | github.com/KhronosGroup/Vulkan-Headers | Apache-2.0 / MIT |
| `volk/`   | Vulkan meta-loader (dlopen + function pointer table; no link to libvulkan) | github.com/zeux/volk | MIT |
| `openxr/` | OpenXR 1.1 C headers only (the loader is dlopen'd at run time and optional) | github.com/KhronosGroup/OpenXR-SDK | Apache-2.0 |

Only the C headers of Vulkan-Headers are vendored; the C++ bindings
(`vulkan.hpp`, 20 MB) are deliberately left out — the renderer is "C with
classes" and uses the flat C API through volk.
