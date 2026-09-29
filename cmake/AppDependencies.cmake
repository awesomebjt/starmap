# =============================================================================
# cmake/AppDependencies.cmake — third-party code needed only by the `starmap`
# graphical application (NOT by the catalog library, probe or tests).
#
# Role in the architecture
# ------------------------
#   starcatalog (library)   <- EnTT, glm, SQLite          (cmake/Dependencies.cmake)
#   starmap     (app)       <- starcatalog + SDL3 + bgfx  (this file)
#
# Keeping these separate means someone who only wants the data layer (or CI
# running the unit tests) never has to download ~150 MB of graphics code:
# configure with -DSTARMAP_BUILD_APP=OFF.
#
# Why FetchContent with a URL + hash instead of git clones or system packages?
#   * URL_HASH makes the download reproducible and tamper-evident: if the
#     archive on the server ever changes, the configure step fails loudly.
#   * Release archives download much faster than a git clone with history.
#   * System packages (apt install libsdl3-dev) differ per distro/OS and are
#     often older; pinning here gives every platform the exact same versions.
#
# The two libraries
# -----------------
# SDL3  — "Simple DirectMedia Layer" v3. Creates the OS window, gives us
#         keyboard/mouse events, high-DPI information and — crucially for bgfx —
#         the *native* window handle (HWND on Windows, NSWindow* on macOS,
#         an X11 Window id or a Wayland wl_surface* on Linux).
#         SDL can also render, but we do NOT use SDL's renderer: bgfx draws.
# bgfx  — a cross-platform rendering library. One C++ API on top of Direct3D
#         11/12, Metal, Vulkan and OpenGL. It owns the GPU device/context and
#         the swap chain of the window SDL created. Shaders are written once in
#         bgfx's GLSL-like ".sc" dialect and compiled offline by the `shaderc`
#         tool into one binary per backend (see cmake/Shaders.cmake).
#
# bgfx itself is built with GENie/make upstream; `bgfx.cmake` is the community
# CMake port (maintained under the same GitHub owner). Its release archives
# already contain the bgfx, bx (base library: math, allocators, file I/O) and
# bimg (image library: we use it to write PNG screenshots) submodules, so no
# recursive git checkout is needed.
# =============================================================================

include(FetchContent)
# STARMAP_PIN_<name>_URL/_HASH come from cmake/DependencyPins.cmake, already
# included by cmake/Dependencies.cmake (which the top-level CMakeLists.txt
# includes first). Including it again here would be harmless (it only sets
# variables) but unnecessary.

# -----------------------------------------------------------------------------
# SDL3 — pinned stable release (3.4.x is the stable series; odd minor numbers
# are development snapshots in SDL's versioning scheme).
# -----------------------------------------------------------------------------
# Build SDL as a *static* library so the final starmap executable has no
# SDL3.dll/.dylib/.so to ship next to it. The cache variables below must be set
# BEFORE FetchContent_MakeAvailable, because SDL's CMakeLists reads them while
# it is being configured (this is the standard "configure a subproject by
# pre-seeding its options" pattern; CMP0077 makes option() honour them).
set(SDL_SHARED        OFF CACHE BOOL "" FORCE)
set(SDL_STATIC        ON  CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY  OFF CACHE BOOL "" FORCE)  # SDL's own test framework: not needed
set(SDL_TESTS         OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES      OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL       OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
    URL      ${STARMAP_PIN_SDL3_URL}
    URL_HASH ${STARMAP_PIN_SDL3_HASH}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

# -----------------------------------------------------------------------------
# bgfx.cmake — pinned release. The tag encodes bgfx's API version (1.161),
# the bgfx commit count (9510) and the bgfx.cmake release number (579).
# -----------------------------------------------------------------------------
set(BGFX_BUILD_EXAMPLES       OFF CACHE BOOL "" FORCE)  # ~50 sample apps: big and slow to build
set(BGFX_BUILD_TESTS          OFF CACHE BOOL "" FORCE)
set(BGFX_INSTALL              OFF CACHE BOOL "" FORCE)
set(BGFX_CUSTOM_TARGETS       OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_TOOLS          ON  CACHE BOOL "" FORCE)  # we need shaderc ...
set(BGFX_BUILD_TOOLS_SHADER   ON  CACHE BOOL "" FORCE)  # ... = the offline shader compiler
set(BGFX_BUILD_TOOLS_GEOMETRY OFF CACHE BOOL "" FORCE)  # mesh converter: not needed
set(BGFX_BUILD_TOOLS_TEXTURE  OFF CACHE BOOL "" FORCE)  # texture converter: not needed
set(BGFX_BUILD_TOOLS_BIN2C    OFF CACHE BOOL "" FORCE)  # we load shader binaries from disk instead
set(BGFX_CONFIG_VIDEO         OFF CACHE BOOL "" FORCE)  # hardware video decode: not needed
# BGFX_CONFIG_MULTITHREADED stays at its default (ON). We still run bgfx
# single-threaded by calling bgfx::renderFrame() before bgfx::init() — see
# src/render/Graphics.cpp for why that is the simpler choice with SDL.
FetchContent_Declare(bgfx
    URL      ${STARMAP_PIN_bgfx_URL}
    URL_HASH ${STARMAP_PIN_bgfx_HASH}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_MakeAvailable(SDL3 bgfx)

# Third-party headers must not trigger OUR high warning level (-Wconversion,
# -Wold-style-cast ... in cmake/Warnings.cmake). Marking their include
# directories SYSTEM makes the compiler use -isystem (GCC/Clang) or
# /external:I (MSVC), which suppresses warnings originating in those headers.
starmap_mark_system(SDL3_Headers)
starmap_mark_system(SDL3-static)
foreach(_t IN ITEMS bgfx bx bimg)
    starmap_mark_system(${_t})
endforeach()

# bgfx.cmake ships helper functions (bgfx_compile_shaders & friends) in
# bgfxToolUtils.cmake but only include()s them when building its examples.
# The file defines the helpers only if the bgfx::shaderc target exists, so it
# must be included AFTER FetchContent_MakeAvailable above.
include(${bgfx_SOURCE_DIR}/cmake/bgfxToolUtils.cmake)

# bgfx_compile_shaders() passes ${BGFX_SHADER_INCLUDE_PATH} to shaderc as an
# include directory; that is where bgfx_shader.sh (the header every .sc file
# includes: mul(), mtxFromCols(), built-in uniforms like u_viewProj ...) lives.
set(BGFX_SHADER_INCLUDE_PATH ${bgfx_SOURCE_DIR}/bgfx/src)

# -----------------------------------------------------------------------------
# Dear ImGui — immediate-mode GUI (the filter panel, the HUD overlay).
# -----------------------------------------------------------------------------
# "Immediate mode" means there is no retained widget tree: every frame the
# code simply CALLS ImGui::Checkbox("Hide...", &flag) and ImGui draws it and
# reports interaction in the same call. The UI state (is the flag set?) lives
# in OUR variables (here: FilterSettings in registry.ctx()), never inside a
# widget object, so there is nothing to keep in sync. Perfect for tools and
# debug UIs, and it pairs naturally with an ECS where all state is plain data.
#
# ImGui itself is renderer- and platform-agnostic: it outputs triangle lists
# (ImDrawData) and consumes input you feed it. The "backends" glue it to a
# platform and a renderer:
#   platform: backends/imgui_impl_sdl3.cpp (official) -> mouse, keyboard,
#             clipboard, cursors, DPI information from SDL3
#   renderer: src/render/ImGuiRenderer.cpp (ours) -> draws ImDrawData with bgfx.
#             There is no official bgfx backend, and writing one is a great
#             exercise in vertex layouts, transient buffers, textures, scissor.
#
# Version: v1.92.9b, a release tag of the master branch (the docking branch
# adds dockable windows and multi-viewport; we don't need either). 1.92
# introduced the dynamic font atlas: glyphs are rasterised on demand at the
# size/DPI in use, and the RENDERER backend must support texture
# create/update/destroy requests (ImGuiBackendFlags_RendererHasTextures).
# Our backend implements that protocol — see ImGuiRenderer.cpp.
#
# ImGui ships no CMakeLists.txt (it is meant to be compiled into your
# project), so FetchContent only downloads it and we define the library here.
FetchContent_Declare(imgui
    URL      ${STARMAP_PIN_imgui_URL}
    URL_HASH ${STARMAP_PIN_imgui_HASH}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(imgui)

add_library(starmap_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    # The demo window (ImGui::ShowDemoWindow) is the best ImGui reference
    # there is: every widget with the code next to it. starmap opens it with
    # F12. Costs ~300 KB of code; drop this line to save it.
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp)
# SYSTEM: ImGui's headers must not trigger our -Wconversion/-Wold-style-cast.
target_include_directories(starmap_imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
# PUBLIC definitions reach every target that links starmap_imgui, which is
# required: all translation units must agree on ImGui's configuration macros
# (otherwise struct layouts differ -> IMGUI_CHECKVERSION() asserts).
#   IMGUI_DISABLE_OBSOLETE_FUNCTIONS: compile errors instead of silently using
#   APIs slated for removal, so this code stays current.
target_compile_definitions(starmap_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(starmap_imgui PUBLIC SDL3::SDL3)   # imgui_impl_sdl3 calls SDL
target_compile_options(starmap_imgui PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>)   # not our code
