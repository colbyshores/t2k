cmake_minimum_required(VERSION 3.16)
project(t2k LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Two source trees: ../t2k_core/src is the platform-agnostic game (logic,
# data, shared geometry builders, seam-only UI); ./src is this target's
# platform layer (SDL2 window/input/audio sinks + the Vulkan renderer behind
# the render.h seam). The 3DS target (../t2k_3ds) consumes the same core tree.
set(CORE "${CMAKE_CURRENT_SOURCE_DIR}/../t2k_core")
get_filename_component(CORE "${CORE}" ABSOLUTE)

# Default to an OPTIMIZED build. With no CMAKE_BUILD_TYPE set at all, CMake's
# own default is unoptimized (no -O flag), while t2k_3ds/Makefile always
# builds -O2. RelWithDebInfo (-O2 -g) keeps it debuggable. See the
# -fno-strict-aliasing note below for why the two builds must optimise alike.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(WITH_AUDIO "Enable audio via SDL2_mixer" ON)
# NB there is deliberately no build option for the validation layer: it is
# selected AT RUNTIME with --validate (main.cpp g_validate -> vkctx::Desc
# .validation -> vk_context.cpp, which also checks the layer is actually
# installed). A `T2K_VALIDATION` option used to sit here and define
# T2K_VK_VALIDATION=1, which NO SOURCE FILE EVER READ -- so switching it on
# enabled nothing while its help text promised the validation layer. Removed
# 2026-09-09 rather than wired up: the runtime flag already works, and a knob
# that lies is worse than no knob (DOCTRINE.md, "a dead config knob is a bug").

# =============================================================================
# Dependencies
# =============================================================================

# --- SDL2 (window, input, audio device; Vulkan surface creation) ---
find_package(SDL2 REQUIRED)

# --- SDL2_mixer (optional) ---
if(WITH_AUDIO)
    find_package(SDL2_mixer QUIET)
    if(NOT SDL2_mixer_FOUND)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(SDL2_MIXER SDL2_mixer)
        endif()
    endif()
    if(NOT SDL2_mixer_FOUND AND NOT SDL2_MIXER_FOUND)
        message(STATUS "SDL2_mixer not found — building without audio (WITH_AUDIO=OFF)")
        set(WITH_AUDIO OFF)
    endif()
endif()

# --- GLM / nlohmann: the VENDORED copies in t2k_core/third_party, always.
# (Searching the system first once resolved to whatever conda had active and
# linked a different nlohmann than the 3DS vendors -- see git history.)
set(GLM_INCLUDE_DIR "${CORE}/third_party/glm")
set(NLOHMANN_JSON_INCLUDE_DIR "${CORE}/third_party")
if(NOT EXISTS "${GLM_INCLUDE_DIR}/glm/glm.hpp" OR NOT EXISTS "${NLOHMANN_JSON_INCLUDE_DIR}/nlohmann/json.hpp")
    message(FATAL_ERROR "vendored glm / nlohmann missing under ${CORE}/third_party")
endif()

# --- Vulkan: vendored headers + volk (no link against libvulkan; volk dlopens
# the loader so a machine without an ICD gets a message, not a missing .so).
set(VK_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vulkan/include")
set(VOLK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/volk")
set(OPENXR_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openxr/include")

# --- glslang (GLSL -> SPIR-V). Searched in the SDK / distro locations and the
# user-local prebuilt (~/.local/opt/glslang/bin).
find_program(GLSLANG NAMES glslang glslangValidator
    HINTS $ENV{VULKAN_SDK}/bin $ENV{HOME}/.local/opt/glslang/bin /usr/local/bin /usr/bin)
if(NOT GLSLANG)
    message(FATAL_ERROR "glslang/glslangValidator not found (install glslang-tools, the Vulkan SDK, or the prebuilt into ~/.local/opt/glslang)")
endif()
message(STATUS "glslang: ${GLSLANG}")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# =============================================================================
# Level data — ../t2k_core/data/levels.json is THE level data (the game parses
# it at boot). Embed the file verbatim as the fallback + first-boot seed;
# tools/bin2h.py validates it first, so a broken levels.json fails the build
# instead of a boot. Only src/data/webs_runtime.cpp includes the header.
# =============================================================================

set(GEN_DIR "${CMAKE_BINARY_DIR}/generated")
set(LEVELS_EMBED_HDR "${GEN_DIR}/levels_json.h")
add_custom_command(
    OUTPUT "${LEVELS_EMBED_HDR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GEN_DIR}"
    COMMAND ${Python3_EXECUTABLE} "${CORE}/tools/bin2h.py"
            "${CORE}/data/levels.json" "${LEVELS_EMBED_HDR}"
    DEPENDS "${CORE}/data/levels.json"
            "${CORE}/tools/bin2h.py"
    COMMENT "Embedding t2k_core/data/levels.json (validated)")

# The ALBUM MAP: t2k_core/data/albums_manifest.json embedded as the fallback
# for a card that has music but no albums.json beside it (that file is
# gitignored -- it lives with the audio -- so a hand-copied music folder has
# no map at all and every album vanishes from the menu). The card's copy still
# wins when present. gen_albums.py validates the manifest first, so a manifest
# with a duplicated track fails the build.
set(ALBUMS_EMBED_HDR "${GEN_DIR}/albums_json.h")
add_custom_command(
    OUTPUT "${ALBUMS_EMBED_HDR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GEN_DIR}"
    COMMAND ${Python3_EXECUTABLE} "${CORE}/tools/gen_albums.py"
            --emit-header "${ALBUMS_EMBED_HDR}"
    DEPENDS "${CORE}/data/albums_manifest.json"
            "${CORE}/tools/gen_albums.py"
    COMMENT "Embedding t2k_core/data/albums_manifest.json (validated)")

# Build tag (git rev + date) for the title screen: regenerated every build,
# written only when it changes (see t2k_core/tools/gen_build_rev.py).
set(BUILD_REV_HDR "${GEN_DIR}/build_rev.h")
add_custom_target(t2k_build_rev ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GEN_DIR}"
    COMMAND ${Python3_EXECUTABLE} "${CORE}/tools/gen_build_rev.py" "${BUILD_REV_HDR}"
    BYPRODUCTS "${BUILD_REV_HDR}"
    COMMENT "Build tag")

# =============================================================================
# Shaders — every .vert/.frag under shaders/ is compiled to SPIR-V and embedded
# as a uint32_t array header (tools/spv2h.py). The .spv depends on the
# includes (common.glsl, texgen_sets.glsl) explicitly, so editing an include
# rebuilds every shader that uses it -- the stale-generated-header trap the
# port-docs vault records for xxd'd shaders cannot happen.
# =============================================================================

set(SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(SHADER_INCLUDES "${SHADER_DIR}/common.glsl" "${SHADER_DIR}/texgen_sets.glsl")
set(SHADERS
    seg.vert seg.frag
    tri.vert tri.frag tri_tex.frag
    grid.vert grid.frag
    fullscreen.vert bloom_down.frag bloom_up.frag composite.frag
    river.vert river.frag
    star.vert
    texgen.frag
)
set(SHADER_HEADERS)
foreach(sh ${SHADERS})
    string(REPLACE "." "_" sym "${sh}")
    set(spv "${GEN_DIR}/${sh}.spv")
    set(hdr "${GEN_DIR}/shader_${sym}.h")
    add_custom_command(
        OUTPUT "${spv}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GEN_DIR}"
        COMMAND ${GLSLANG} -V --target-env vulkan1.3 -I"${SHADER_DIR}" -o "${spv}" "${SHADER_DIR}/${sh}"
        DEPENDS "${SHADER_DIR}/${sh}" ${SHADER_INCLUDES}
        COMMENT "GLSL ${sh}")
    add_custom_command(
        OUTPUT "${hdr}"
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/spv2h.py" "${spv}" "${hdr}" "spv_${sym}"
        DEPENDS "${spv}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/spv2h.py"
        COMMENT "SPV2H ${sh}")
    list(APPEND SHADER_HEADERS "${hdr}")
endforeach()

# =============================================================================
# Source Files
# =============================================================================

set(SOURCES
    # Platform layer (this tree)
    src/main.cpp
    src/input/input_handler.cpp
    src/audio/sound_manager.cpp
    src/audio/music_sdl.cpp
    # The Vulkan renderer behind the render.h seam (RENDERER_VK)
    src/rendering/vk/vk_context.cpp
    src/rendering/vk/vk_resources.cpp
    src/rendering/vk/vk_pipeline.cpp
    src/rendering/vk/xr_session.cpp
    src/rendering/png_write.cpp
    src/rendering/renderer_vk.cpp
    src/rendering/vk_streams.cpp
    src/rendering/vk_font.cpp
    src/rendering/vk_scene.cpp
    src/rendering/vk_post.cpp
    src/rendering/vk_warp.cpp
    src/rendering/vk_title.cpp
    src/rendering/vk_chamber.cpp
    third_party/volk/volk.c

    # ---- t2k_core: the game, compiled into both targets ----
    ${CORE}/src/game/engine.cpp
    ${CORE}/src/game/camera.cpp
    ${CORE}/src/game/config_apply.cpp
    ${CORE}/src/game/enemies.cpp
    # Per-family enemy TUs (src/game/enemies/, see enemy_family.h). This list
    # is explicit, so every new family must be added here AND picked up by
    # t2k_3ds/Makefile's wildcard — a family that lands on one target only is
    # INCOMPLETE, not "phase one".
    ${CORE}/src/game/enemies/reflector.cpp
    ${CORE}/src/game/enemies/arcade_flipper.cpp
    ${CORE}/src/game/enemies/arcade_spiker.cpp
    ${CORE}/src/game/enemies/arcade_tanker.cpp
    ${CORE}/src/game/enemies/arcade_fuseball.cpp
    ${CORE}/src/game/enemies/arcade_mirror.cpp
    ${CORE}/src/game/enemies/arcade_pulsar.cpp
    ${CORE}/src/game/enemies/arcade_adroid.cpp
    ${CORE}/src/game/player.cpp
    ${CORE}/src/game/weapons.cpp
    ${CORE}/src/game/collision.cpp
    ${CORE}/src/game/warp.cpp
    ${CORE}/src/game/game_step.cpp
    ${CORE}/src/game/math_lut.cpp
    ${CORE}/src/game/web_preview.cpp
    # Shared geometry builders (the parity contract with the 3DS).
    ${CORE}/src/rendering/grid_geometry.cpp
    ${CORE}/src/rendering/entity_geometry.cpp
    ${CORE}/src/rendering/line_geometry.cpp
    ${CORE}/src/rendering/rail_geometry.cpp
    ${CORE}/src/rendering/warp_geometry.cpp
    ${CORE}/src/rendering/logo_geometry.cpp
    ${CORE}/src/rendering/gameover_geometry.cpp
    ${CORE}/src/rendering/hud_icons.cpp
    ${CORE}/src/rendering/font_data.cpp
    ${CORE}/src/rendering/shatter.cpp
    ${CORE}/src/rendering/textures.cpp
    ${CORE}/src/rendering/starfield.cpp
    # Shared audio core: analyser, FFT, replayer, decoder, SFX bank.
    ${CORE}/src/audio/fft.cpp
    ${CORE}/src/audio/audio_analysis.cpp
    ${CORE}/src/audio/sfx_data.cpp
    ${CORE}/src/audio/mod_player.cpp
    ${CORE}/src/audio/music_core.cpp
    # Data
    ${CORE}/src/data/save_load.cpp
    ${CORE}/src/data/webs_runtime.cpp
    # UI (seam-only)
    ${CORE}/src/ui/highscores.cpp
    ${CORE}/src/ui/level_select.cpp
    ${CORE}/src/ui/menu.cpp
    ${CORE}/src/ui/touch_panel.cpp
)

# =============================================================================
# Executable
# =============================================================================

add_executable(t2k ${SOURCES} "${LEVELS_EMBED_HDR}" "${ALBUMS_EMBED_HDR}" ${SHADER_HEADERS})

target_include_directories(t2k PRIVATE
    "${CORE}/src"
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    "${GEN_DIR}"
    "${GLM_INCLUDE_DIR}"
    "${NLOHMANN_JSON_INCLUDE_DIR}"
    "${VK_INCLUDE_DIR}"
    "${VOLK_DIR}"
    "${OPENXR_INCLUDE_DIR}"
)

# Rendering backend selection (desktop = Vulkan 1.3). The 3DS/Citro3D backend
# is selected by t2k_3ds/Makefile with -DRENDERER_C3D.
target_compile_definitions(t2k PRIVATE RENDERER_VK VK_NO_PROTOTYPES)
add_dependencies(t2k t2k_build_rev)

# SDL2
if(TARGET SDL2::SDL2)
    target_link_libraries(t2k PRIVATE SDL2::SDL2 SDL2::SDL2main)
else()
    target_include_directories(t2k PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(t2k PRIVATE ${SDL2_LIBRARIES})
endif()

# SDL2_mixer (optional). TS_WITH_AUDIO is consumed in src/ -- renaming it on
# one side only builds green and silently compiles audio out (DOCTRINE.md).
if(WITH_AUDIO)
    target_compile_definitions(t2k PRIVATE TS_WITH_AUDIO)
    if(TARGET SDL2_mixer::SDL2_mixer)
        target_link_libraries(t2k PRIVATE SDL2_mixer::SDL2_mixer)
    elseif(SDL2_MIXER_FOUND)
        target_include_directories(t2k PRIVATE ${SDL2_MIXER_INCLUDE_DIRS})
        target_link_libraries(t2k PRIVATE ${SDL2_MIXER_LIBRARIES})
    else()
        target_link_libraries(t2k PRIVATE SDL2_mixer)
    endif()
endif()

# Threads — the audio analyzer's worker (src/audio/audio_thread.h maps it to
# std::thread here, to libctru's threadCreate on the 3DS).
find_package(Threads REQUIRED)
target_link_libraries(t2k PRIVATE Threads::Threads)

# Math + dlopen (volk, and the optional OpenXR loader)
if(UNIX AND NOT APPLE)
    target_link_libraries(t2k PRIVATE m dl)
endif()

# =============================================================================
# Compiler Warnings / Flags
# =============================================================================

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(t2k PRIVATE -Wall -Wextra -Wno-unused-parameter)
    # Matches t2k_3ds/Makefile's own COMMONFLAGS. Non-optional now that this
    # build is optimized: commit e4c897b found -O2 without this flag
    # miscompiling the shared type-punning grid/engine code on the 3DS into
    # nondeterministic GameEngine corruption -- the exact class of bug an
    # optimized-but-unprotected desktop build would be exposed to as well.
    target_compile_options(t2k PRIVATE -fno-strict-aliasing)
    # Matches t2k_3ds/Makefile: the engine is written exception-free / RTTI-free
    # (no try/catch/throw/dynamic_cast/typeid in t2k_core), so enforce the same
    # contract on the desktop build rather than only asserting it in the README.
    target_compile_options(t2k PRIVATE -fno-exceptions -fno-rtti)
    set_source_files_properties(third_party/volk/volk.c PROPERTIES COMPILE_FLAGS "-w")
endif()
