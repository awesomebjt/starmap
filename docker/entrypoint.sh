#!/usr/bin/env bash
# =============================================================================
# docker/entrypoint.sh — configure + build (+ test) starmap inside the container
# =============================================================================
# Installed in the image as /usr/local/bin/starmap-build and used as the
# ENTRYPOINT (see Dockerfile). Expected mounts:
#
#   /src    the starmap source tree, read-only is fine (nothing writes to it)
#   /build  a HOST directory: the CMake binary dir. Everything the build
#           produces lands here, so it survives the container (--rm) and is
#           reused next time: the second run is an incremental Ninja build.
#
# Environment knobs (docker run -e NAME=value):
#   BUILD_TYPE   Release (default) | Debug | RelWithDebInfo | MinSizeRel
#   RUN_TESTS    1 (default) runs ctest after the build, 0 skips it
#   WERROR       1 turns on STARMAP_WARNINGS_AS_ERRORS (0 by default)
#   JOBS         parallel compile jobs (default: Ninja's choice, #cores + 2)
#
# Arguments:
#   * none, or arguments starting with '-': they are appended to the cmake
#     configure command line, e.g.  ... starmap-build -DSTARMAP_BUILD_PROBE=OFF
#   * anything else is executed instead of the build, so
#     `docker run -it ... starmap-build bash` gives you a shell in the image.
# =============================================================================

# -e: stop at the first failing command; -u: unset variables are errors;
# -o pipefail: a pipeline fails if any stage fails, not just the last one.
set -euo pipefail

# "docker run IMAGE bash" -> run bash instead of building. The usual pattern
# for images whose ENTRYPOINT is a script: it keeps the image useful for
# poking around (inspecting headers, running cmake by hand, ...).
if [[ $# -gt 0 && "$1" != -* ]]; then
    exec "$@"
fi

SRC=/src
BUILD=/build
BUILD_TYPE="${BUILD_TYPE:-Release}"
RUN_TESTS="${RUN_TESTS:-1}"
WERROR="${WERROR:-0}"

# --- sanity checks with actionable messages ----------------------------------
# The three classic ways this setup fails are all mount problems, and CMake's
# own errors for them are cryptic, so check them up front.
if [[ ! -f "$SRC/CMakeLists.txt" ]]; then
    echo "error: $SRC/CMakeLists.txt not found." >&2
    echo "       Mount the starmap source tree:  -v /path/to/starmap:/src:ro,z" >&2
    exit 2
fi
if ! touch "$BUILD/.write-test" 2>/dev/null; then
    echo "error: cannot write to $BUILD as uid $(id -u)." >&2
    echo "       * create the host directory yourself BEFORE docker run (if Docker" >&2
    echo "         creates it, it is owned by root);" >&2
    echo "       * pass --user \"\$(id -u):\$(id -g)\" so the container runs as you;" >&2
    echo "       * on SELinux hosts (openSUSE Tumbleweed, Fedora) add :Z to the" >&2
    echo "         mount:  -v ~/starmap-build:/build:Z" >&2
    exit 2
fi
rm -f "$BUILD/.write-test"

# A CMake build directory is tied to the source path it was configured with
# (CMAKE_HOME_DIRECTORY in CMakeCache.txt). A directory configured by a HOST
# cmake (source at /home/you/starmap) cannot be reused here (source at /src)
# and vice versa: CMake refuses with "does not match the source ... used to
# generate cache". Say so plainly instead.
if [[ -f "$BUILD/CMakeCache.txt" ]]; then
    home_dir=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$BUILD/CMakeCache.txt")
    if [[ -n "$home_dir" && "$home_dir" != "$SRC" ]]; then
        echo "error: $BUILD was configured for source '$home_dir', not '$SRC'." >&2
        echo "       Use an empty host directory for container builds." >&2
        exit 2
    fi
    old_type=$(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' "$BUILD/CMakeCache.txt")
    if [[ -n "$old_type" && "$old_type" != "$BUILD_TYPE" ]]; then
        # Works (CMake just reconfigures), but everything recompiles, and
        # flipping back and forth recompiles again each time. One directory
        # per build type is the idiom for single-config generators like Ninja.
        echo "note: $BUILD was a $old_type build; switching it to $BUILD_TYPE rebuilds everything." >&2
        echo "      Tip: use one host directory per build type." >&2
    fi
fi

werror_flag=OFF
[[ "$WERROR" == 1 ]] && werror_flag=ON

# --- configure ------------------------------------------------------------------
#   -C prefetched.cmake  pre-loads FETCHCONTENT_SOURCE_DIR_<NAME> for every
#                        dependency baked into the image (cmake/Prefetch.cmake),
#                        so no download happens: builds work with --network none.
#   -G Ninja             Ninja tracks header dependencies precisely and starts
#                        fast, which is what makes the second run incremental
#                        and quick. (Also set via CMAKE_GENERATOR in the image.)
#   CC/CXX               come from the image's ENV (gcc-14 / g++-14). CMake only
#                        reads them on the FIRST configure of a directory; to
#                        switch compilers, use a new, empty build directory.
# Re-running the configure step on an existing build dir is cheap (it only
# re-checks what changed), so we always do it: that also picks up new
# -D options passed as arguments.
echo "== configure ($BUILD_TYPE, $(${CXX:-c++} --version | head -1))"
cmake -S "$SRC" -B "$BUILD" -G Ninja \
      -C /opt/starmap-deps/prefetched.cmake \
      -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
      -DSTARMAP_WARNINGS_AS_ERRORS="$werror_flag" \
      "$@"

# --- build ------------------------------------------------------------------------
echo "== build"
build_args=()
[[ -n "${JOBS:-}" ]] && build_args+=(--parallel "$JOBS")
cmake --build "$BUILD" "${build_args[@]}"

# --- test -------------------------------------------------------------------------
if [[ "$RUN_TESTS" == 1 ]]; then
    echo "== ctest"
    # The catalog integration tests read the real database. CMake bakes its
    # default location in as <source>/../starcatalog/stars.db, which inside
    # the container is /starcatalog/stars.db (or the STARMAP_DB variable).
    # Without it those tests print a message and pass (skip): say so here,
    # so a green ctest isn't mistaken for a full run.
    if [[ -z "${STARMAP_DB:-}" && ! -f /starcatalog/stars.db ]]; then
        echo "note: no catalog mounted, the stars.db integration tests will SKIP." >&2
        echo "      To run them: -v /path/to/starcatalog:/starcatalog:ro,z" >&2
    fi
    ctest --test-dir "$BUILD" --output-on-failure
fi

# --- report -----------------------------------------------------------------------
# The newest GLIBC_x.y symbol version the binary imports is the OLDEST glibc
# it can run on (glibc is backward compatible, never forward). objdump -T
# lists the dynamic symbols with their version tags; sort -V sorts versions
# numerically (2.9 < 2.34). Same idea for libstdc++'s GLIBCXX_ tags.
if [[ -x "$BUILD/starmap" ]]; then
    glibc=$(objdump -T "$BUILD/starmap" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
    glibcxx=$(objdump -T "$BUILD/starmap" | grep -o 'GLIBCXX_[0-9.]*' | sort -uV | tail -1)
    echo "== done: $BUILD/starmap needs >= $glibc and libstdc++ >= $glibcxx"
    echo "   Runtime files next to it: shaders/glsl, shaders/spirv (found via the"
    echo "   executable's directory). On the host run, e.g.:"
    echo "     <host build dir>/starmap /path/to/stars.db"
fi
