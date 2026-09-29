# syntax=docker/dockerfile:1
# =============================================================================
# Dockerfile — a reproducible BUILD ENVIRONMENT for starmap
# =============================================================================
# This image doesn't contain starmap. It contains everything needed to BUILD
# it: compiler, CMake, Ninja, the development headers SDL3 needs, and every
# third-party source archive (prefetched, so builds need no network). You
# mount your source tree and an output directory, and the binary lands on
# your host:
#
#   docker build -t starmap-build .                       # once, from the source root
#   mkdir -p ~/starmap-build                              # create it yourself (see below)
#   docker run --rm --user "$(id -u):$(id -g)" \
#       -v "$PWD":/src:ro,z -v ~/starmap-build:/build:Z starmap-build
#   ~/starmap-build/starmap /path/to/stars.db             # runs on the host
#
# Why each piece of that command matters:
#   --rm                  the container is disposable; all state worth keeping
#                         is in the two mounts.
#   --user uid:gid        run as YOU, so files written to ~/starmap-build are
#                         owned by you, not root. The uid doesn't need to
#                         exist in the image's /etc/passwd: nothing here looks
#                         the user up, and HOME points at a world-writable dir.
#   /src:ro               the source is only read (CMake writes nothing into
#                         the source tree), so mount it read-only: the build
#                         can't damage your checkout.
#   /build                the CMake binary dir, on the host: survives the
#                         container, so the next run is an incremental build.
#   :z / :Z (SELinux)     on SELinux hosts (openSUSE Tumbleweed since 2025,
#                         Fedora, RHEL) a container may only touch files
#                         labelled for containers. :z relabels a path as
#                         SHARED between containers (right for the source,
#                         several builds may read it), :Z relabels it
#                         PRIVATE to this container (right for a build dir).
#                         On hosts without SELinux (or with the daemon's
#                         SELinux support off) the suffixes are harmless.
#                         Never use :Z on a system dir or all of $HOME.
#   mkdir first           if the host dir doesn't exist, the Docker daemon
#                         creates it AS ROOT and the build then can't write.
#
# BASE IMAGE CHOICE: ubuntu:24.04 (glibc 2.39) + GCC 14
#   The binary is built against the image's glibc and runs on any system
#   with the SAME OR NEWER glibc (glibc keeps old symbol versions forever,
#   but a binary built against 2.44 can't run on 2.39). So the rule is:
#   build on the OLDEST glibc you want to support.
#     * opensuse/tumbleweed would match an openSUSE host exactly, but it is a
#       rolling release: its glibc moves ahead of the host's (e.g. Slowroll
#       snapshots lag Tumbleweed), and `docker pull` a month later gives a
#       different toolchain. The binary would only run on equally new systems.
#     * ubuntu:24.04 is an LTS (fixed package set until 2029), its glibc 2.39
#       is older than every current rolling/desktop distro (Tumbleweed 2.44,
#       Fedora, Debian 13 = 2.41), and it ships GCC 14 (C++20 complete) as
#       the gcc-14 package. The resulting binary needs only GLIBC_2.38 (the
#       entrypoint prints the exact requirement after each build).
#   libstdc++ follows the same rule: the binary asks for GLIBCXX_3.4.31
#   (GCC 13-era symbols; GCC 14's libstdc++ provides up to 3.4.33), which
#   every current distro's libstdc++ satisfies. For an even more portable
#   binary, add -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc".
#
# WHAT ENDS UP ON THE HOST (the /build mount)
#   starmap              the app (links only libc, libm, libstdc++, libgcc_s)
#   shaders/glsl, shaders/spirv
#                        compiled bgfx shaders. REQUIRED at runtime; the app
#                        looks for <dir of the executable>/shaders, so keep
#                        them next to the binary (copy the whole folder if
#                        you move it).
#   catalog_probe        the console tool from milestone 1
#   starcatalog_tests, starmap_app_tests, CTestTestfile.cmake
#                        doctest executables (run by ctest in the container).
#                        The catalog integration tests look for
#                        /src/../starcatalog/stars.db = /starcatalog/stars.db
#                        and skip themselves if it's missing; add
#                        -v /path/to/starcatalog:/starcatalog:ro,z to run them.
#   _deps/, CMakeFiles/, *.a, build.ninja ...
#                        build intermediates, needed for incremental rebuilds
#   Not in the build dir, found on the host at runtime:
#     stars.db           pass it as the first argument (or put it or a symlink
#                        next to the binary: <exe dir>/stars.db is searched)
#     DejaVuSans.ttf     optional fallback font for Greek letters and symbols,
#                        read from the host's /usr/share/fonts (openSUSE:
#                        package dejavu-fonts); without it the UI still works
#     libGL/libEGL, libX11, libwayland-client, ...
#                        loaded with dlopen() at runtime (see below); every
#                        desktop Linux has them.
# =============================================================================

FROM ubuntu:24.04

# Keep apt from asking questions (tzdata etc.). ARG, not ENV: it only exists
# while the image builds and doesn't leak into containers started from it.
ARG DEBIAN_FRONTEND=noninteractive

# -----------------------------------------------------------------------------
# Toolchain + development headers, in ONE layer
# -----------------------------------------------------------------------------
# One RUN = one layer: update, install and clean the apt lists together, so
# the ~40 MB of package lists never get stored in the image.
# --no-install-recommends: Ubuntu's "recommended" packages (docs, extra
# tools) would roughly double the image for nothing.
#
# HOW SDL3 USES THESE HEADERS (the key idea)
#   SDL3 is compiled from source (FetchContent), and its configure step
#   enables each backend (X11, Wayland, PulseAudio, ...) only if the
#   backend's headers are present NOW, at build time. But with SDL's default
#   SDL_<BACKEND>_SHARED=ON, it does NOT link those libraries: it records
#   their sonames ("libX11.so.6", "libwayland-client.so.0") and dlopen()s
#   them at run time. That's why the finished binary links nothing but
#   libc/libstdc++ (check: readelf -d starmap | grep NEEDED), and why ONE
#   binary works on an X11-only, Wayland-only or headless machine: a missing
#   library just means that backend is skipped. Missing headers here, on the
#   other hand, would silently compile that backend OUT of the binary.
#
#   The bgfx renderer does the same with OpenGL/Vulkan: it dlopen()s libGL /
#   libvulkan when the renderer starts, so the GL headers only have to exist
#   at compile time too.
RUN apt-get update && apt-get install -y --no-install-recommends \
        # --- toolchain -------------------------------------------------------
        # ca-certificates: CMake downloads over HTTPS (prefetch step below)
        # and must be able to verify github.com / sqlite.org certificates.
        ca-certificates \
        # GCC 14: the project's reference compiler (C++20). Ubuntu 24.04's
        # default gcc is 13; the versioned package installs gcc-14/g++-14
        # side by side (selected below through CC/CXX). It pulls in
        # binutils (ld, objdump) and libc6-dev (the glibc headers).
        gcc-14 g++-14 \
        # CMake 3.28 (project needs >= 3.24) and Ninja (fast incremental builds).
        cmake ninja-build \
        # pkg-config: how SDL's configure step finds Wayland, libdecor,
        # PipeWire, D-Bus... (it asks `pkg-config --cflags wayland-client`).
        pkg-config \
        # --- X11 (SDL video backend) ----------------------------------------
        # Core Xlib plus the extensions SDL uses: Xext (shape, XDBE), Xrandr
        # (monitors/modes), Xcursor (cursors), Xfixes (pointer barriers),
        # Xi (XInput2: raw mouse motion, touch), Xss (screensaver inhibit),
        # Xtst (XTest, for synthetic input).
        libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev \
        libxi-dev libxss-dev libxtst-dev \
        # xkbcommon: keyboard layouts (mandatory for Wayland, also used on X11).
        libxkbcommon-dev \
        # --- Wayland (SDL video backend, default on GNOME/KDE sessions) -----
        # libwayland-dev also brings wayland-scanner, which SDL runs at build
        # time to turn protocol XML files into C code. libdecor draws window
        # decorations (title bar) on compositors without server-side
        # decorations, notably GNOME; without it windows there have no frame.
        libwayland-dev wayland-protocols libdecor-0-dev \
        # --- OpenGL / EGL / GLES headers (SDL + bgfx) -----------------------
        # Headers and the GL vendor-neutral dispatch stubs (libglvnd). bgfx's
        # OpenGL renderer and SDL's GL/EGL support compile against these.
        libgl-dev libegl-dev libgles-dev \
        # --- KMS/DRM (SDL video backend for running without X11/Wayland) ----
        # Lets SDL drive a display directly from a text console. Optional,
        # but cheap; handy on a kiosk-style machine.
        libdrm-dev libgbm-dev \
        # --- system services ------------------------------------------------
        # D-Bus: screensaver inhibition, desktop portal and IME (fcitx)
        # integration. libudev: input/joystick device discovery + hotplug.
        libdbus-1-dev libudev-dev \
        # --- audio (not used by starmap today) ------------------------------
        # starmap calls SDL_Init(SDL_INIT_VIDEO) only, so audio backends are
        # never touched. They're installed anyway because SDL's audio
        # subsystem is still COMPILED, and a later milestone adding sound
        # should not require rebuilding this image. Without them SDL just
        # builds its dummy/disk audio drivers; nothing fails.
        libasound2-dev libpulse-dev libpipewire-0.3-dev \
    && rm -rf /var/lib/apt/lists/*
# Deliberately NOT installed:
#   * libibus-1.0-dev (IBus input method): pulls in GLib's dev stack (~60 MB)
#     for a feature this app doesn't need (the search box takes ASCII input).
#   * liburing-dev, libusb-1.0-dev: SDL would use them for async file I/O and
#     HIDAPI gamepads; liburing is LINKED rather than dlopen()ed, which would
#     add a runtime dependency to the binary for no benefit here.
#   * git, python: nothing in the build needs them (FetchContent downloads
#     release tarballs, not git repos; bgfx's shaderc is plain C++).
#   * clang-19: the project also builds with it (CI-style warnings check),
#     but one compiler keeps the image smaller. To add it: install clang-19
#     and run with -e CC=clang-19 -e CXX=clang++-19 in a NEW build dir.

# -----------------------------------------------------------------------------
# Environment for every container started from this image
# -----------------------------------------------------------------------------
#   CC/CXX           which compiler CMake picks on the first configure.
#   CMAKE_GENERATOR  default generator for every cmake invocation (also for
#                    FetchContent's helper sub-builds in the prefetch step).
#   HOME=/tmp        with --user <your uid>, the uid usually has no passwd
#                    entry, and HOME would otherwise be "/" (not writable).
#                    Some tools (CMake's user package registry, ctest) like a
#                    writable HOME; /tmp is world-writable in every image.
ENV CC=gcc-14 \
    CXX=g++-14 \
    CMAKE_GENERATOR=Ninja \
    HOME=/tmp

# -----------------------------------------------------------------------------
# Prefetch all third-party sources into the image
# -----------------------------------------------------------------------------
# Copy ONLY the two files that describe the dependencies. Docker caches each
# step and re-runs it only if the files it copied changed, so editing any
# .cpp never invalidates this (slow, network-bound) layer; bumping a version
# in DependencyPins.cmake does, exactly as it should.
#
# cmake/Prefetch.cmake downloads each pinned archive with FetchContent (hash
# verified), extracts it to /opt/starmap-deps/<name>, and writes
# /opt/starmap-deps/prefetched.cmake, a `cmake -C` initial-cache file setting
# FETCHCONTENT_SOURCE_DIR_<NAME> for each dependency. The entrypoint passes
# it to every configure, so the container never downloads anything and
# works with `docker run --network none`.
#
# The tree stays owned by root and read-only for your uid. That's a feature:
# if any dependency's build tried to write into its SOURCE dir, the build
# would fail loudly instead of silently mutating the shared copy.
COPY cmake/DependencyPins.cmake cmake/Prefetch.cmake /opt/starmap-prefetch/
RUN cmake -DDEST=/opt/starmap-deps -P /opt/starmap-prefetch/Prefetch.cmake \
    # bgfx.cmake's release archive carries bgfx's ~50 sample programs with
    # their prebuilt shaders, meshes and textures (~120 MB). We build with
    # BGFX_BUILD_EXAMPLES=OFF, so that data is dead weight in every image.
    # (examples/common stays: bgfx's tools reference it.)
    && rm -rf /opt/starmap-deps/bgfx/bgfx/examples/runtime \
              /opt/starmap-deps/bgfx/bgfx/examples/assets

# -----------------------------------------------------------------------------
# The build driver
# -----------------------------------------------------------------------------
# docker/entrypoint.sh: configure (Release by default) -> build -> ctest ->
# print the binary's glibc requirement. Knobs: -e BUILD_TYPE=Debug,
# -e RUN_TESTS=0, -e WERROR=1, -e JOBS=N, extra -D... arguments after the
# image name; `docker run -it ... starmap-build bash` opens a shell instead.
# COPY --chmod needs BuildKit (the default builder since Docker 23).
COPY --chmod=755 docker/entrypoint.sh /usr/local/bin/starmap-build

# WORKDIR makes a shell (`... starmap-build bash`) start in the build dir.
# There is deliberately no VOLUME instruction for /src or /build: if you
# forgot the -v option, VOLUME would silently create an anonymous volume and
# the build would vanish into it. Without VOLUME, the entrypoint's checks
# catch the missing mount with a clear message.
WORKDIR /build
ENTRYPOINT ["/usr/local/bin/starmap-build"]
