# =============================================================================
# starmap — top-level CMake build
# =============================================================================
#
# WHAT GETS BUILT
#   starcatalog        static library: stars.db -> entt::registry (SQLite, EnTT, glm)
#   catalog_probe      console tool that loads the catalog and prints statistics
#   starcatalog_tests  doctest unit tests for the library
#   starmap_core       static library: camera maths, colour schemes, CLI parsing (no SDL/GPU)
#   starmap_render     static library: SDL3 window, bgfx renderer, input, HUD, App frame loop
#   starmap            the graphical star map executable (main.cpp only)
#   starmap_app_tests  unit tests for starmap_core
#   starmap_shaders    custom target: compiles shaders/*.sc with bgfx's shaderc
#
# HOW TO BUILD (any platform; needs CMake >= 3.24 and a C++20 compiler)
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release     (add -G Ninja if you have it)
#   cmake --build build --parallel
#   ctest --test-dir build
# The first configure downloads all dependencies (FetchContent, pinned + hashed).
#
# MODERN-CMAKE PRIMER (target-based)
#   Everything is expressed as properties of TARGETS, never global flags:
#     target_link_libraries(A PUBLIC B)   A uses B, and A's users also get B's
#                                         include dirs, definitions and link line
#                                         (the "usage requirements" propagate).
#     ... PRIVATE B                       A uses B internally; users of A don't see it.
#   That is why the app targets below only list their direct dependencies:
#   starmap_render -> starmap_core -> starcatalog -> EnTT/glm, transitively.
# =============================================================================

# 3.24: FetchContent_Declare(... DOWNLOAD_EXTRACT_TIMESTAMP) and a stable
# FetchContent_MakeAvailable. Declaring the minimum also opts in to the CMake
# "policies" (behaviour changes) introduced up to that version.
cmake_minimum_required(VERSION 3.24)
# LANGUAGES C CXX: SQLite (sqlite3.c) and parts of SDL are C, so enable both.
project(starmap VERSION 0.1.0 LANGUAGES C CXX)

# C++20 for std::span, consteval, designated initializers and u8string.
# EXTENSIONS OFF gives -std=c++20 rather than -std=gnu++20, so GNU-only
# extensions are caught on Linux before MSVC rejects them.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Writes build/compile_commands.json, which clangd / VS Code / CLion use for
# code navigation. Worth having when learning a big API like bgfx.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Single-config generators (Ninja, Makefiles) default to an EMPTY build type,
# meaning no optimisation at all. Default to Release instead. Multi-config
# generators (Visual Studio, Xcode) pick the configuration at build time, so
# CMAKE_CONFIGURATION_TYPES is set and this does nothing there.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# User-facing switches: cmake -DSTARMAP_BUILD_APP=OFF ... etc.
option(STARMAP_USE_SYSTEM_SQLITE "Use find_package(SQLite3) instead of downloading the amalgamation" OFF)
option(STARMAP_BUILD_TESTS "Build the doctest test executable" ON)
option(STARMAP_BUILD_PROBE "Build the catalog_probe console tool" ON)
option(STARMAP_WARNINGS_AS_ERRORS "Treat warnings in project code as errors" OFF)
option(STARMAP_BUILD_APP "Build the graphical starmap app (downloads SDL3 + bgfx)" ON)
# A CACHE variable (not option) because it holds a path. Tests read it
# through a compile definition (see below).
set(STARMAP_TEST_DB "${CMAKE_CURRENT_SOURCE_DIR}/../starcatalog/stars.db"
    CACHE FILEPATH "stars.db used by the tests (env var STARMAP_DB overrides at run time)")

include(cmake/Dependencies.cmake)   # EnTT, glm, SQLite, doctest (FetchContent, pinned)
include(cmake/Warnings.cmake)       # starmap_set_warnings(<target>)
if(STARMAP_BUILD_APP)
    include(cmake/AppDependencies.cmake)   # SDL3, bgfx (+ bx, bimg, shaderc), Dear ImGui
endif()

# ---------------------------------------------------------------- library
# The data layer. It knows nothing about windows or GPUs, so it can be reused
# by tools, tests and (later) the bigger game.
add_library(starcatalog
    src/catalog/Sqlite.cpp
    src/catalog/Indexes.cpp
    src/catalog/CatalogLoader.cpp
    src/catalog/CatalogStats.cpp
    # Milestone 3: NameSearch = ranked prefix/substring search over the
    # NameIndex; StarDetails = the lazy one-star query (full row + planets)
    # run when a star is selected. Both are UI-free, hence in this library.
    src/catalog/NameSearch.cpp
    src/catalog/StarDetails.cpp
)
# The namespaced alias lets consumers write starmap::starcatalog. A typo in a
# name containing "::" is a configure error, whereas a typo in a plain name
# silently becomes a linker flag "-lstarcatlog".
add_library(starmap::starcatalog ALIAS starcatalog)
# PUBLIC include dir: users write #include "catalog/CatalogLoader.h" relative to src/.
target_include_directories(starcatalog PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# EnTT and glm are PUBLIC because our public headers include them
# (Components.h uses glm::vec3, Indexes.h uses entt::entity). SQLite is
# PRIVATE because Sqlite.h hides <sqlite3.h> behind forward declarations.
target_link_libraries(starcatalog PUBLIC EnTT::EnTT glm::glm PRIVATE SQLite::SQLite3)
target_compile_features(starcatalog PUBLIC cxx_std_20)  # consumers must also compile as C++20
starmap_set_warnings(starcatalog)

# ---------------------------------------------------------------- probe tool
if(STARMAP_BUILD_PROBE)
    add_executable(catalog_probe apps/catalog_probe.cpp)
    target_link_libraries(catalog_probe PRIVATE starcatalog)
    starmap_set_warnings(catalog_probe)
endif()

# ---------------------------------------------------------------- tests
if(STARMAP_BUILD_TESTS)
    # enable_testing() must come before add_test(), and at the top-level
    # directory, for `ctest` to find the tests from the build root.
    enable_testing()
    add_executable(starcatalog_tests
        tests/test_main.cpp
        tests/test_sqlite.cpp
        tests/test_name_index.cpp
        tests/test_catalog.cpp
        tests/test_search.cpp      # milestone 3: normalisation, ranking, real-db queries, details
    )
    # The tests also exercise the RAII sqlite wrapper (catalog/Sqlite.h) with an
    # in-memory database; that header hides sqlite3.h, so no extra link is needed.
    target_link_libraries(starcatalog_tests PRIVATE starcatalog doctest::doctest)
    # Bakes the path into the binary as a string literal macro (-DSTARMAP_TEST_DB="...").
    target_compile_definitions(starcatalog_tests PRIVATE STARMAP_TEST_DB="${STARMAP_TEST_DB}")
    starmap_set_warnings(starcatalog_tests)
    # Registers the executable with CTest. Exit code 0 counts as pass.
    add_test(NAME starcatalog_tests COMMAND starcatalog_tests)
endif()

# ---------------------------------------------------------------- graphical app
# Three layers, so each can be understood (and tested) on its own:
#   starmap_core   : scene state, colour maps, camera maths, CLI parsing,
#                    filter predicates + FilterSystem, range-slider maths.
#                    No SDL, no ImGui, no GPU calls (only bx's matrix helpers)
#                    -> unit-testable.
#   starmap_render : everything that touches SDL3, bgfx or Dear ImGui (window,
#                    GPU resources, input, HUD, the filter panel and its
#                    widgets, the ImGui->bgfx renderer, the App frame loop).
#   starmap        : the executable = main.cpp only.
# The split also keeps compile times down while iterating: editing a shader
# or the renderer does not rebuild the maths and tests.
if(STARMAP_BUILD_APP)
    include(cmake/Shaders.cmake)
    starmap_compile_shaders()   # defines target starmap_shaders, sets STARMAP_SHADER_DIR

    add_library(starmap_core STATIC
        src/app/CommandLine.cpp
        src/scene/ColorScheme.cpp
        src/render/Colormaps.cpp
        src/systems/CameraSystem.cpp
        src/systems/ColorSystem.cpp
        src/systems/SizeSystem.cpp
        # Milestone 2: filtering. The predicates (Filters.cpp), the ECS pass
        # that applies them (FilterSystem.cpp) and the pure value<->handle
        # maths of the two-handle slider (RangeSliderMath.cpp) have no UI or
        # GPU dependency, so they live here where the unit tests can reach them.
        src/scene/Filters.cpp
        src/scene/Tags.cpp
        src/systems/FilterSystem.cpp
        src/systems/TagSystem.cpp
        src/ui/RangeSliderMath.cpp
        # Milestone 3: selection, picking and camera flights. All pure ECS +
        # maths (no SDL, no bgfx, no ImGui), so the doctest suite can drive
        # them with synthetic registries: select/clear invariants, pick maths
        # on hand-placed stars, easing and frame-rate independence.
        src/systems/SelectionSystem.cpp
        src/systems/PickingSystem.cpp
        src/systems/FlightSystem.cpp
        src/systems/InteractionSystem.cpp
    )
    target_include_directories(starmap_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    # bx is bgfx's base library. starmap_core uses only its maths
    # (bx::mtxLookAt / bx::mtxProj), which are pure CPU functions, so the core
    # library stays testable without a GPU.
    target_link_libraries(starmap_core PUBLIC starcatalog bx)
    starmap_set_warnings(starmap_core)

    add_library(starmap_render STATIC
        src/platform/Window.cpp
        src/render/Graphics.cpp
        src/render/ScreenshotCallback.cpp
        src/render/ShaderLoader.cpp
        src/render/StarRenderer.cpp
        src/render/GuideRenderer.cpp
        src/systems/StarRenderSystem.cpp
        src/systems/HudSystem.cpp
        src/input/InputHandler.cpp
        src/app/App.cpp
        # Milestone 2: Dear ImGui integration. ImGuiRenderer = our bgfx
        # renderer backend; ImGuiLayer = context + SDL3 platform backend +
        # renderer lifetime; RangeSlider = the custom two-handle widget;
        # FilterPanel = the right-hand panel built from them.
        src/render/ImGuiRenderer.cpp
        src/ui/ImGuiLayer.cpp
        src/ui/RangeSlider.cpp
        src/ui/FilterPanel.cpp
        # Milestone 3: the side panel became search box + tabs. UiUtil =
        # helpers shared by the sections (moved out of FilterPanel.cpp);
        # SearchBox = live suggestions; StarDetailsView = the "Star" tab;
        # SidePanel = the window that hosts all three.
        src/ui/UiUtil.cpp
        src/ui/SearchBox.cpp
        src/ui/StarDetailsView.cpp
        src/ui/SidePanel.cpp
        # Right-click tag menu. ImGui only; the palette and the component
        # live in starmap_core (scene/Tags) so the filter and colour passes
        # can see them without a window.
        src/ui/TagMenu.cpp
    )
    # SDL3::SDL3 is an alias SDL creates for whichever library it built (static here).
    # bimg: bgfx's image library, used to encode screenshot PNGs.
    # starmap_imgui: Dear ImGui + its SDL3 backend (cmake/AppDependencies.cmake).
    target_link_libraries(starmap_render PUBLIC starmap_core bgfx bimg SDL3::SDL3 starmap_imgui)
    # Fallback location of the compiled shaders (the build tree), used when the
    # executable is run from somewhere else. See ShaderLoader::default_roots().
    target_compile_definitions(starmap_render PRIVATE STARMAP_SHADER_DIR="${STARMAP_SHADER_DIR}")
    starmap_set_warnings(starmap_render)

    add_executable(starmap apps/starmap/main.cpp)
    target_link_libraries(starmap PRIVATE starmap_render)
    # Target-level dependency: the shaders are not linked into the executable
    # (they are data files loaded at run time), but building `starmap` must
    # still bring them up to date.
    add_dependencies(starmap starmap_shaders)   # build shaders whenever the app is built
    starmap_set_warnings(starmap)
    # Visual Studio: make "starmap" the default F5 target instead of ALL_BUILD.
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT starmap)

    if(STARMAP_BUILD_TESTS)
        # Links only starmap_core (no SDL, no bgfx renderer), so these tests
        # run on headless CI machines with no display or GPU.
        add_executable(starmap_app_tests tests/test_main.cpp tests/test_app_core.cpp tests/test_filters.cpp
                                         tests/test_selection.cpp)  # M3: picking, flights, selection
        target_link_libraries(starmap_app_tests PRIVATE starmap_core doctest::doctest)
        starmap_set_warnings(starmap_app_tests)
        add_test(NAME starmap_app_tests COMMAND starmap_app_tests)
    endif()
endif()
