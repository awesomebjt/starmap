# =============================================================================
# cmake/Dependencies.cmake — third-party code for the catalog LIBRARY and tests
# =============================================================================
# All third-party code comes from FetchContent with pinned versions + hashes,
# so the project builds on Windows/macOS/Linux without system packages.
# (The graphics dependencies for the app, SDL3 and bgfx, are declared
# separately in AppDependencies.cmake.)
#
# HOW FETCHCONTENT WORKS
#   FetchContent_Declare(name URL ... URL_HASH ...) only RECORDS where to get
#   a dependency. FetchContent_MakeAvailable(name) downloads it (once, into
#   build/_deps/name-src) and, if it contains a CMakeLists.txt, runs
#   add_subdirectory() on it. Its targets (EnTT::EnTT, glm::glm, ...) then
#   exist in our build just like our own targets.
#   Tips:
#     * -DFETCHCONTENT_SOURCE_DIR_<NAME>=/path uses a local checkout instead
#       of downloading (handy offline, or to share one download between
#       several build directories).
#     * -DFETCHCONTENT_FULLY_DISCONNECTED=ON skips the network entirely once
#       everything is downloaded.
#     * The Docker build environment (Dockerfile, docker/entrypoint.sh) uses
#       the first tip: cmake/Prefetch.cmake downloads every archive into the
#       image and writes a `cmake -C` cache file that sets
#       FETCHCONTENT_SOURCE_DIR_<NAME> for each one, so container builds are
#       offline and every build dir shares one read-only copy of the sources.
#   Alternatives: vcpkg or Conan (package managers, great for big projects),
#   git submodules (need a manual `git submodule update`), or system packages
#   (versions differ per OS). FetchContent needs nothing but CMake.
#
# WHY URL + HASH INSTEAD OF GIT_REPOSITORY + GIT_TAG?
#   A tag can be moved, while a hash can't lie. Release tarballs also
#   download faster than a clone.
# =============================================================================
include(FetchContent)
# The URLs and hashes themselves live in DependencyPins.cmake (one list shared
# with the Docker prefetch script, see that file for why). include() with a
# path relative to CMAKE_CURRENT_LIST_DIR works no matter which directory
# includes this file. AppDependencies.cmake reads the same variables; it is
# included after this file, so it doesn't need to include the pins again.
include(${CMAKE_CURRENT_LIST_DIR}/DependencyPins.cmake)
set(FETCHCONTENT_QUIET ON)  # hide the per-dependency download logs unless something fails

# Treat a dependency's include dirs as SYSTEM headers (no warnings from them).
# (FetchContent_Declare(... SYSTEM) needs CMake 3.25; this works with 3.24.)
function(starmap_mark_system target)
    get_target_property(_inc ${target} INTERFACE_INCLUDE_DIRECTORIES)
    if(_inc)
        set_target_properties(${target} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_inc}")
    endif()
endfunction()

# --- EnTT (header-only ECS) ---------------------------------------------------
# Entity-Component-System library: entt::registry, views, ctx(). Header-only,
# so "building" it only makes its include path available through EnTT::EnTT.
# DOWNLOAD_EXTRACT_TIMESTAMP TRUE keeps the archive's file timestamps and
# silences policy warning CMP0135.
FetchContent_Declare(entt
    URL      ${STARMAP_PIN_entt_URL}
    URL_HASH ${STARMAP_PIN_entt_HASH}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

# --- glm (header-only math) -----------------------------------------------------
# GLSL-style vectors and matrices (glm::vec3, glm::mat4). The Position
# component and the camera use it, and it looks just like shader code.
# These cache variables must be set BEFORE MakeAvailable, because glm's
# CMakeLists reads them as options. FORCE overrides any stale cache value.
set(GLM_BUILD_LIBRARY OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glm
    URL      ${STARMAP_PIN_glm_URL}
    URL_HASH ${STARMAP_PIN_glm_HASH}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_MakeAvailable(entt glm)
starmap_mark_system(EnTT)
starmap_mark_system(glm-header-only)

# --- SQLite -------------------------------------------------------------------
if(STARMAP_USE_SYSTEM_SQLITE)
    find_package(SQLite3 REQUIRED)            # provides SQLite::SQLite3
else()
    # Official amalgamation (one sqlite3.c). The pinned hash is the SHA3-256
    # published on sqlite.org/download.html (see DependencyPins.cmake).
    # The "amalgamation" is all of SQLite concatenated into one C file. It
    # has no CMakeLists.txt of its own, so we define a small library target
    # for it ourselves. Compile-time options are documented at
    # https://sqlite.org/compile.html.
    FetchContent_Declare(sqlite_amalgamation
        URL      ${STARMAP_PIN_sqlite_amalgamation_URL}
        URL_HASH ${STARMAP_PIN_sqlite_amalgamation_HASH}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(sqlite_amalgamation)   # no CMakeLists inside: just downloads
    add_library(starmap_sqlite3 STATIC ${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c)
    target_include_directories(starmap_sqlite3 SYSTEM PUBLIC ${sqlite_amalgamation_SOURCE_DIR})
    target_compile_definitions(starmap_sqlite3 PRIVATE
        SQLITE_DQS=0                 # no double-quoted string literals
        SQLITE_DEFAULT_MEMSTATUS=0
        SQLITE_OMIT_LOAD_EXTENSION=1 # no dlopen -> no libdl dependency
        SQLITE_THREADSAFE=1)
    # PIC lets the static library also be linked into a shared library later.
    set_target_properties(starmap_sqlite3 PROPERTIES POSITION_INDEPENDENT_CODE ON)
    # THREADSAFE=1 uses pthread mutexes on Unix, so link the platform threads
    # library (Threads::Threads is empty on Windows).
    find_package(Threads REQUIRED)
    target_link_libraries(starmap_sqlite3 PUBLIC Threads::Threads)
    # It's not our code: silence its warnings on every compiler.
    # $<IF:cond,a,b> is a generator expression, evaluated at build-generation
    # time. Here it picks MSVC's or GCC/Clang's "no warnings" flag.
    target_compile_options(starmap_sqlite3 PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/W0,-w>)
    # An ALIAS with the same name find_package(SQLite3) would provide, so the
    # rest of the build doesn't care which branch was taken.
    add_library(SQLite::SQLite3 ALIAS starmap_sqlite3)
endif()

# --- doctest (tests only) -----------------------------------------------------
# Tiny single-header test framework (TEST_CASE / CHECK). tests/test_main.cpp
# defines DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN once, and every other test file
# just includes the header.
if(STARMAP_BUILD_TESTS)
    set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
    set(DOCTEST_NO_INSTALL ON  CACHE BOOL "" FORCE)
    set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(doctest
        URL      ${STARMAP_PIN_doctest_URL}
        URL_HASH ${STARMAP_PIN_doctest_HASH}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(doctest)
    starmap_mark_system(doctest)
endif()
