# =============================================================================
# cmake/DependencyPins.cmake — THE single list of third-party versions
# =============================================================================
# Every external dependency is pinned here, and only here, as a URL plus a
# cryptographic hash. Two different consumers read this file:
#
#   1. cmake/Dependencies.cmake and cmake/AppDependencies.cmake, which pass
#      these values to FetchContent_Declare() during a normal configure;
#   2. cmake/Prefetch.cmake, a script (`cmake -P`) that the Dockerfile runs at
#      IMAGE build time to download every archive once and bake it into the
#      image, so container builds need no network.
#
# WHY A SEPARATE FILE?
#   If the Dockerfile kept its own copy of the URLs, the two copies would
#   drift apart one day: someone bumps SDL here, forgets the Dockerfile, and
#   the container silently keeps compiling the OLD SDL (the prefetched source
#   dir overrides the download). One list means that cannot happen. As a side
#   effect, Docker only re-downloads when THIS file changes, because the
#   Dockerfile copies nothing else from the tree before the prefetch step
#   (Docker's layer cache is keyed on the files a step can see).
#
# NAMING
#   STARMAP_PIN_<name>_URL / _HASH, where <name> is EXACTLY the name used in
#   FetchContent_Declare(<name> ...). FetchContent derives its override
#   variable FETCHCONTENT_SOURCE_DIR_<NAME-in-uppercase> from that name, and
#   Prefetch.cmake relies on the match to write those overrides.
#
# This file only sets variables. It must stay valid in BOTH project mode and
# script mode (cmake -P), so no targets, no project(), no FetchContent calls.
# =============================================================================

# Every pinned dependency, in the order Prefetch.cmake downloads them.
set(STARMAP_PINNED_DEPS entt glm sqlite_amalgamation doctest SDL3 bgfx imgui)

# --- catalog library + tests (cmake/Dependencies.cmake) ----------------------
set(STARMAP_PIN_entt_URL  https://github.com/skypjack/entt/archive/refs/tags/v3.16.0.tar.gz)
set(STARMAP_PIN_entt_HASH SHA256=7d7b4037b737992342049ffab14f22fa10243e01664f8c3a0657aa247ac52f71)

set(STARMAP_PIN_glm_URL  https://github.com/g-truc/glm/archive/refs/tags/1.0.3.tar.gz)
set(STARMAP_PIN_glm_HASH SHA256=6775e47231a446fd086d660ecc18bcd076531cfedd912fbd66e576b118607001)

# SQLite publishes SHA3-256 (not SHA256) on sqlite.org/download.html; CMake
# understands both, so we use the hash the upstream page actually shows.
set(STARMAP_PIN_sqlite_amalgamation_URL  https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip)
set(STARMAP_PIN_sqlite_amalgamation_HASH SHA3_256=628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e)

set(STARMAP_PIN_doctest_URL  https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz)
set(STARMAP_PIN_doctest_HASH SHA256=174ebc4e769928959614789c5b4e9c3d0a0f81a62bb608756b127bfebfb21331)

# --- graphical app (cmake/AppDependencies.cmake) -----------------------------
set(STARMAP_PIN_SDL3_URL  https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz)
set(STARMAP_PIN_SDL3_HASH SHA256=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68)

set(STARMAP_PIN_bgfx_URL  https://github.com/bkaradzic/bgfx.cmake/releases/download/v1.161.9510-579/bgfx.cmake.v1.161.9510-579.tar.gz)
set(STARMAP_PIN_bgfx_HASH SHA256=2b489206be79d0841009c15853aa8717749373528a694933b926b797b4cfc992)

set(STARMAP_PIN_imgui_URL  https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz)
set(STARMAP_PIN_imgui_HASH SHA256=21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99)
