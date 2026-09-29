# starmap — interactive 3D star map (C++20, SDL3 + bgfx + EnTT + Dear ImGui)

Milestone 1 ("first pixels"): every star within 200 ly (91,998 of them, from
`../starcatalog/stars.db`) is drawn as a soft, camera-facing glow in galactic
coordinates. You get an orbit camera, six colour schemes and distance rings.

Milestone 2 ("filters"): a Dear ImGui panel on the right lets you thin the
star population per colour scheme: hide stars with no data for the field,
trim a range with a two-handle slider, or tick spectral classes on and off.
Filters are remembered per scheme and can be combined (AND). The HUD moved
from bgfx debug text to ImGui overlays.

Milestone 3 ("selection"): click a star to select it (screen-space picking
that mirrors the star shader's size *and* brightness rules), or find it by
name in a search box with live, ranked suggestions. The camera flies to the
selection with an eased, frame-rate-independent animation. A details tab
shows everything the catalogue knows about the star, with per-field sources,
all its names grouped by catalogue, and its planets. A reticle marks the
selection, and a dashed line joins it to Sol with the distance.

The code is **teaching material** for SDL3, bgfx, EnTT and Dear ImGui:
every file starts with a header comment explaining its role, and the comments
explain *why* (API order constraints, pitfalls, alternatives, the maths).
A good reading order:

1. `apps/starmap/main.cpp` → `src/app/App.h/.cpp` (startup order + the frame loop)
2. `src/platform/Window.cpp` (SDL3 window, native handles) → `src/render/Graphics.cpp` (bgfx init)
3. `src/render/StarRenderer.cpp` + `shaders/vs_star.sc`, `fs_star.sc` (instancing, billboards, glow)
4. `src/systems/CameraSystem.cpp` + `src/scene/OrbitCamera.h` (view/projection maths)
5. `src/systems/ColorSystem.cpp`, `src/render/Colormaps.cpp` (EnTT views, blackbody, viridis)
6. `src/catalog/*` (SQLite → EnTT registry)
7. Milestone 2, filtering: `src/scene/Filters.h/.cpp` (the filter state and
   predicates) → `src/systems/FilterSystem.cpp` (the ECS pass, tag components,
   `entt::exclude`) → `src/systems/StarRenderSystem.cpp` (the view that skips
   filtered stars)
8. Milestone 2, Dear ImGui: `src/ui/ImGuiLayer.cpp` (context, SDL3 backend,
   input arbitration) → `src/render/ImGuiRenderer.cpp` + `shaders/vs_imgui.sc`
   (our bgfx renderer backend) → `src/ui/RangeSliderMath.cpp` +
   `src/ui/RangeSlider.cpp` (a custom widget) → `src/ui/FilterPanel.cpp`
9. Milestone 3, selection: `src/scene/Selection.h` (tag vs ctx) →
   `src/systems/PickingSystem.h/.cpp` (why screen space, the ray maths for
   reference, visibility-aware hit radii) → `src/input/InputHandler.cpp`
   (click vs drag) → `src/systems/InteractionSystem.cpp` →
   `src/scene/CameraFlight.h` + `src/systems/FlightSystem.cpp` (easing,
   interpolating parameters instead of matrices) → `src/catalog/NameSearch.*`
   (normalisation + ranking) → `src/catalog/StarDetails.*` (lazy loading) →
   `src/ui/SearchBox.cpp`, `StarDetailsView.cpp`, `SidePanel.cpp`

```
starmap/
├── CMakeLists.txt                 # targets: starcatalog, starmap_core, starmap_render, starmap, tests
├── Dockerfile, .dockerignore      # build environment image (see "Building with Docker")
├── docker/entrypoint.sh           # in-container configure + build + ctest driver
├── cmake/
│   ├── DependencyPins.cmake       # THE list of third-party URLs + hashes (shared with the Docker prefetch)
│   ├── Prefetch.cmake             # `cmake -P` script: download all pins + write a `cmake -C` override file
│   ├── Dependencies.cmake         # EnTT 3.16.0, glm 1.0.3, SQLite 3.53.4, doctest 2.5.3
│   ├── AppDependencies.cmake      # SDL3 3.4.16, bgfx.cmake v1.161.9510-579 (bgfx + bx + bimg + shaderc),
│   │                              # Dear ImGui v1.92.9b (+ its SDL3 platform backend)
│   ├── Shaders.cmake              # shaderc at build time: glsl+spirv (Linux), dxbc+spirv+glsl (Windows), metal (macOS)
│   └── Warnings.cmake
├── shaders/                       # bgfx .sc dialect
│   ├── varying.def.sc             # vertex attributes, instance attributes, varyings
│   ├── vs_star.sc / fs_star.sc    # instanced billboard + gaussian glow
│   ├── vs_line.sc / fs_line.sc    # guide lines (rings, spokes)
│   └── vs_imgui.sc / fs_imgui.sc  # Dear ImGui triangles (varying_imgui.def.sc)
├── apps/
│   ├── starmap/main.cpp           # entry point (SDL_main), CLI -> App
│   └── catalog_probe.cpp          # console tool for the loader
├── src/
│   ├── catalog/                   # SQLite RAII wrapper, loader, name/id indexes, stats,
│   │                              # NameSearch (ranked search), StarDetails (lazy one-star query)
│   ├── ecs/Components.h           # per-star components (Position, Physical, DisplayColor, ...)
│   ├── app/                       # App (owns everything, frame loop), CommandLine (CLI, default db lookup)
│   ├── platform/Window            # SDL3 window + native handle extraction (Win32/Cocoa/X11/Wayland)
│   ├── render/
│   │   ├── Graphics               # bgfx init/shutdown/reset, renderFrame() single-thread trick
│   │   ├── ShaderLoader           # picks shaders/<glsl|spirv|dxbc|metal>/*.bin for the running backend
│   │   ├── StarRenderer           # unit quad + transient instance buffers, blend state
│   │   ├── GuideRenderer          # 10/50/100/200 ly rings, spokes, galactic-centre line, labels
│   │   ├── ScreenshotCallback     # bgfx::CallbackI -> PNG via bimg
│   │   ├── ImGuiRenderer          # our Dear ImGui renderer backend for bgfx (textures, transient buffers, scissor)
│   │   └── Colormaps              # blackbody (Helland/Charity), viridis, diverging, spectral palette
│   ├── scene/                     # OrbitCamera, ColorScheme + ColorSettings, ViewSettings, render components,
│   │                              # Filters (FilterSettings, FilteredOut tag, predicates, CLI filter options),
│   │                              # Selection (SelectionState/Details, PointerInput, HoverState), CameraFlight
│   ├── systems/                   # CameraSystem, ColorSystem, SizeSystem, FilterSystem, StarRenderSystem, HudSystem,
│   │                              # PickingSystem, SelectionSystem, FlightSystem, InteractionSystem (M3)
│   ├── ui/                        # ImGuiLayer, RangeSliderMath (pure), RangeSlider (widget), FilterPanel,
│   │                              # UiUtil, SearchBox, StarDetailsView, SidePanel (M3)
│   └── input/InputHandler         # SDL events -> camera / settings changes
├── tests/                         # doctest: loader + app core (camera, colours, CLI) + filters/slider maths
│                                  # + search/details (real db) + picking/flights/selection (synthetic)
└── screenshots/                   # PNGs produced with --screenshot (not part of the source tarball)
```

## The app

### Build

Linux needs the X11/Wayland/GL development headers, because SDL3 is built
from source:

```sh
sudo apt install build-essential cmake ninja-build pkg-config \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev \
  libgl-dev libegl-dev libwayland-dev libxkbcommon-dev wayland-protocols libdrm-dev libgbm-dev
# optional (SDL warns but builds without them): libasound2-dev libpulse-dev libdecor-0-dev
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build-release --output-on-failure
./build-release/starmap                        # finds ../starcatalog/stars.db automatically
```

Windows (VS 2022): `cmake -S . -B build -G "Visual Studio 17 2022" -A x64`, then
`cmake --build build --config Release`, then `build\Release\starmap.exe`.
macOS: `cmake -S . -B build -G Ninja` (or `-G Xcode`), then build. Metal shaders are compiled automatically.
The first configure downloads roughly 150 MB of sources (Dear ImGui adds ~2 MB). The first build
compiles bgfx and shaderc, which takes a few minutes. Use
`-DSTARMAP_BUILD_APP=OFF` to build only the catalog library and tests.

### Building with Docker

The `Dockerfile` builds an image that contains a complete build ENVIRONMENT
(no starmap inside): Ubuntu 24.04, GCC 14, CMake 3.28, Ninja, the X11 /
Wayland / GL / audio development headers SDL3 needs, and every third-party
source archive, prefetched so builds work offline. You mount your source tree
read-only at `/src` and an empty host directory at `/build`. The build
happens in the container, and the binary lands on your host, owned by you.

```sh
cd starmap                                   # the source root (where the Dockerfile is)
docker build -t starmap-build .              # once; ~3 min cold, seconds when cached

mkdir -p ~/starmap-build                     # create it YOURSELF (else Docker makes it root-owned)
docker run --rm --user "$(id -u):$(id -g)" \
    -v "$PWD":/src:ro,z \
    -v ~/starmap-build:/build:Z \
    starmap-build                            # configure (Release) + build + ctest

~/starmap-build/starmap /path/to/starcatalog/stars.db
```

* **SELinux** (openSUSE Tumbleweed, Fedora, RHEL): `:z` relabels the source
  mount as shared between containers, and `:Z` relabels the build dir as
  private to this container. Without them, an enforcing SELinux host denies
  the container access to the mounts ("Permission denied"). On hosts without
  SELinux, or with a Docker daemon that has SELinux support turned off, the
  suffixes are harmless no-ops, so keep them in scripts.
  Don't put `:Z` on system directories or on your whole home directory.
* **Ownership**: `--user "$(id -u):$(id -g)"` makes every output file yours.
  The image sets `HOME=/tmp`, so an arbitrary uid works without a passwd entry.
  With *rootless* Docker or Podman, drop `--user`: container root already
  maps to you. (With Podman, `--userns=keep-id` is the equivalent.)
* **Incremental**: `/build` persists, so the next `docker run` is a Ninja
  incremental build (about 5 s with nothing to do, about 7 s after editing
  one .cpp file). Use one host directory per build type or compiler.
* **Knobs**: `-e BUILD_TYPE=Debug`, `-e WERROR=1` (warnings as errors),
  `-e RUN_TESTS=0`, `-e JOBS=4`, extra `-D...` CMake options after the image
  name, or `starmap-build bash` (with `-it`) for a shell in the image.
* **Offline**: `--network none` works, because the dependency sources are
  baked into the image (`cmake/Prefetch.cmake` writes a `cmake -C` file that
  sets `FETCHCONTENT_SOURCE_DIR_<NAME>` for each one). The versions come from
  `cmake/DependencyPins.cmake`, the same list the normal build uses, so the
  two can't drift. Bumping a pin re-runs the prefetch layer on the next
  `docker build`.
* **Catalog tests**: the stars.db integration tests skip themselves unless
  the catalog is mounted. Add `-v /path/to/starcatalog:/starcatalog:ro,z`
  to run them.
* **What lands in `~/starmap-build`**: `starmap`, `shaders/glsl` and
  `shaders/spirv` (required at runtime; the app looks for `shaders/` next to
  the executable, so move them together), `catalog_probe`, the two test
  executables, and the build intermediates.
  **Not** in there: `stars.db` (pass its path, or symlink it next to the
  binary) and the optional DejaVuSans fallback font. The font is read from
  the host at runtime (`/usr/share/fonts/truetype/DejaVuSans.ttf` on openSUSE,
  package `dejavu-fonts`).
* **Portability**: the binary links only libc, libm, libstdc++ and libgcc_s,
  and requires **GLIBC_2.38** and **GLIBCXX_3.4.31** (the entrypoint prints
  both after every build). So it runs on Ubuntu 24.04+, Debian 13,
  Fedora 39+, and openSUSE Tumbleweed/Slowroll (glibc 2.44). X11, Wayland,
  libGL/EGL and the audio libraries are `dlopen()`ed at run time, so they must
  exist on the machine running the app (every desktop has them), but not at
  link time.
* A build directory remembers its source path (`/src`), so a container build
  dir can't be reused by a host `cmake` and vice versa. The entrypoint
  detects that and tells you.

### Command line

```
starmap [path/to/stars.db] [options]
  --max-ly N            only stars within N light years (default 200)
  --min-snr N           drop Gaia stars with parallax/error < N
  --max-ruwe N          drop Gaia stars with RUWE > N
  --scheme NAME         temperature | spectral | diameter | age | metallicity | planets
  --renderer NAME       auto | opengl | vulkan | d3d11 | d3d12 | metal
  --size WxH            window size in points (default 1280x720)
  --no-vsync            render as fast as possible (for timing)
  --exposure X          star brightness multiplier (default 1)
  --no-hud / --no-guides
  --focus NAME          orbit around a named star ("Sirius")
  --camera D,YAW,PITCH  initial camera distance (pc) and angles (degrees)
  --screenshot FILE     render --frames N frames (default 5), save a PNG, exit
filters (the same state the panel edits; repeatable):
  --filter S=LO:HI      keep stars whose value for scheme S is in [LO, HI]; either side may be
                        empty ("temperature=:4000"). S: temperature|diameter|age|metallicity|planets
                        or distance, in light years from Sol ("distance=10:40"). Distance always
                        applies, together with the scheme filters, whether or not --combine is set.
  --hide-missing[=S]    hide stars with no data for scheme S (bare: the --scheme one)
  --classes G,K         spectral classes to keep
  --combine             apply every scheme's filter together (AND), not only the active one
  --no-panel            start with the side panel hidden
selection (milestone 3; mainly for reproducible screenshots):
  --select NAME         select a star through the name search (best hit) and jump to it
                        (no animation). With --camera, its distance/angles are kept.
  --pick X,Y            simulate a left click at window point X,Y on frame 2
  --search TEXT         pre-fill the search box, so its suggestion list is visible
```

Default database, first match wins: `<exe dir>/stars.db`, `./stars.db`,
`../starcatalog/stars.db` (relative to the working directory), then
`<exe dir>/../starcatalog/stars.db` and `<exe dir>/../../starcatalog/stars.db`.
If none exists, the app prints the paths it tried and a usage hint (see
`find_default_db`).

Examples:
`starmap --scheme spectral --focus Sirius --camera 4,20,15 --screenshot sirius.png --frames 5`,
`starmap --renderer opengl --no-vsync`,
`starmap --filter temperature=5000:6500 --hide-missing --screenshot sunlike.png`,
`starmap --filter distance=0:25 --screenshot nearby.png`,
`starmap --scheme age --hide-missing --filter temperature=4000:6000 --classes G,K --combine`,
`starmap --select "eps Eri" --screenshot epseri.png --frames 10`,
`starmap --camera 6,-30,20 --pick 373,433 --screenshot pick.png --frames 70`,
`starmap --search ran --filter temperature=6000: --screenshot search.png`.

In `--screenshot` mode camera flights advance by a fixed 1/60 s per frame,
so a flight of 0.8 s is complete after 48 frames on any machine.

### Controls

| input | action |
|---|---|
| left-click | select the star under the cursor and fly to it; on empty space: deselect |
| right-click a star | open the tag menu (16 colours). The camera stays put. Right-click empty space closes it |
| left-drag | orbit (yaw / pitch, pitch clamped to ±89°) |
| hover | tooltip with the star's name, distance, spectral type and tag, if it has one |
| mouse wheel | zoom (exponential: each notch multiplies the distance); cancels a flight |
| right-drag or middle-drag | pan the orbit target in the view plane; cancels a flight. A right-click that moves less than 4 points is the tag menu, not a pan |
| double-click on empty space | fly back to Sol |
| Ctrl+F or / | focus the search box (shows the panel if hidden) |
| F | fly to the selection (to Sol if nothing is selected) |
| H or Home | fly to Sol, keeping the zoom |
| R | fly back to the start-up view (target, distance and angles) |
| Esc | close the tag menu if it is open; otherwise deselect; with nothing selected, quit |
| Q | quit |
| 1–6 | colour scheme: temperature, spectral class, diameter, age, metallicity, planets (or the panel's combo box) |
| P or Tab | show / hide the side panel |
| G | toggle guides (rings, spokes, Sol line) |
| F1 | toggle the help overlay (was H before milestone 3) |
| + / − | brightness (exposure) |
| F12 | Dear ImGui's demo window (the widget gallery, handy while learning ImGui) |

In the search box: type to see suggestions, Up/Down to move the highlight,
Enter (or a click on a row) to select, Esc to clear the text.

Mouse and keys go to the panel first. While the pointer is over the panel
(or a drag started on it), the camera ignores the mouse. While a number
field has keyboard focus, the hotkeys are ignored, so typing "5000" does not
switch schemes.

### The side panel

Since milestone 3 the right-hand panel has a search box at the top and two
tabs below it: **Filters** and **Star**
(details of the selection). Selecting a star switches to the Star tab once;
you can switch back freely. The panel is 430 points wide by default (was
380) because the details tables need the room; drag its left edge to resize.

#### Filters tab

The Filters tab always refers to the **active colour scheme**:

* **Header:** a scheme selector (same as keys 1–6), "Showing N of M stars"
  with a meter, and the colour legend. Continuous schemes get a gradient bar
  with tick labels; spectral class gets its coloured class letters.
* **Hide stars with no … data:** removes stars that have no value for the
  field. For planets, "no data" means *no known planets* (planet_count 0 is
  not a measurement, so only hosts remain). For spectral class, it means the
  class is unknown (`?`).
* **Range schemes** (temperature K, diameter R☉, age Gyr, metallicity dex,
  planets): a two-handle slider across the full catalog range. The selected
  span is highlighted and the handles can't cross. Next to it are exact
  min/max fields, **Reset**, and **Clamp to 1–99 %** (the orange marks on
  the track are the 1st/99th percentiles).
  * Diameter and planet count use a log scale, like their colormaps. Planet
    counts snap to whole numbers.
  * A range only judges stars that *have* a value. Stars without one stay
    (grey) unless the toggle above is on.
* **Spectral class:** one checkbox per class present in the catalog, with
  its palette swatch and star count, plus All / None. Unknown `?` is
  deliberately not a checkbox; the "Hide stars of unknown class" toggle
  handles it, so the unknowns follow the same rule as every other scheme.
* **Distance from Sol:** a two-handle slider in light years, under the
  scheme filter. It always applies, ANDed with whatever scheme filter is
  active (and with the others, when Combine is on). The full catalog span
  shows every distance. Min/max fields, Reset and Clamp to 1–99 % work as
  they do for a scheme. `--filter distance=10:40` sets the same range.
* **Tags:** right-click a star and pick one of 16 colours (or Remove tag).
  The choice is remembered on the star. **Show only tagged stars** hides
  everyone else and draws the tagged stars in the colour you picked instead
  of the colour scheme. Turning it off restores the scheme colours and
  keeps the tags. The Star tab shows the tag of the selected star and can
  remove it. Sol stays visible in every filter, including this one.
* **Combine filters from all schemes:** off by default. Only the active
  scheme's filter applies; the others are remembered and come back when you
  switch to them. When on, every scheme's non-default filter applies
  together (AND), and the filters from the other schemes are listed, each
  with a **clear** button.
* **Sol is always shown.** It is the origin and reference point of the map;
  a map that can lose "you are here" is confusing.
* The panel's left edge can be dragged to resize it. The 3D view always
  uses the area left of the panel, so nothing hides behind it.

How it works (ECS): `FilterSettings` lives in `registry.ctx()` with a
`dirty` flag. The panel and the CLI only edit that data. `FilterSystem`
runs when the flag is set (or the active scheme changed) and adds or removes
an empty `FilteredOut` tag on each star whose visibility changed.
`StarRenderSystem` iterates
`view<Position, DisplayColor, DisplaySize>(entt::exclude<FilteredOut>)`.
The trade-offs (tag churn vs. a per-frame branch vs. EnTT groups) are
discussed in `FilterSystem.h`.

Timing (Release, GCC 14, this VM): a full filter pass over the 91,998 stars
takes about 2.8–3.6 ms, whether it runs while dragging a slider or from the
CLI. It is 0.004 ms when no filter is active (the tag storage is simply
cleared). The panel footer shows the last pass time. A Debug build takes
about 45 ms because EnTT is unoptimised.

### Selection, search, fly-to and details (milestone 3)

**Click vs drag.** The left button both orbits (drag) and selects (click).
A press is only recorded. It becomes a drag once the pointer moves more than
4 points, and the accumulated movement is then applied to the orbit, so
nothing is lost. A release that never became a drag, within 500 ms of the
press, is a click. The input handler does not pick. It leaves
`PointerInput::click_px` for `InteractionSystem`, which runs once per frame
after all events, using **last frame's** matrices: exactly the picture the
user clicked on.

**Picking** (`PickingSystem`) is screen-space nearest-point, not a 3D ray
test. Stars are billboards whose size is chosen in *pixels* (1.6–40 px), so
a ray/sphere test against their physical radius would almost never hit. The
ray maths is sketched in the header for later use: unproject with the
inverse view-projection; z_near is −1 or 0 depending on bgfx's
`homogeneousDepth`.

The picker projects every non-`FilteredOut` star exactly as `vs_star.sc`
does and computes its drawn radius and its drawn *brightness*. It then
chooses the smallest normalised distance d / r:
* a visible star is hit within max(60 % of the quad radius, 4 px) + 2 px;
* a star drawn fainter than 30 % needs a direct hit (2.5 px).

The brightness rule came from driving the live app. With a plain 8 px
minimum, the thousands of far-away 10 %-brightness specks tiled the whole
screen, and clicking "empty space" selected an invisible star 190 ly away.
Clicks outside the 3D viewport (on the panel strip) pick nothing.

**Selection model.** Exactly one entity carries the empty `ecs::Selected`
tag, mirrored by `SelectionState{entity, version}` in `registry.ctx()`. The
tag serves ECS queries (`view<Selected>`); the ctx copy gives O(1) "what is
selected" and a version counter that lets the lazy details loader, the
filter pass and the panel's tab switch notice a change. All mutation goes
through `systems::select` / `clear_selection`, which keep both in sync (the
trade-off is discussed in `scene/Selection.h`). The **reticle** (ring and
ticks sized from the drawn billboard, plus the name) and the Sol-line label
are drawn with ImGui's background draw list. The **dashed Sol → selection
line** is 3D geometry in the guides view (a bgfx *transient* vertex buffer,
rebuilt each frame). The distance label is shown only when the line is at
least 160 points long on screen.

**Search** (`catalog::NameSearch`) normalises every name once at start-up
(~17 ms for 116,916 names):
* ASCII case folding; punctuation becomes a space; spaces collapse;
* the first word "Gliese"/"Gl" becomes "GJ";
* a compact, space-free key is kept too, so "hip16537" finds "HIP 16537".

Each keystroke scans all keys and puts each name in a tier: **exact >
prefix > word prefix > substring**. Only the best name per star is kept. The
top 20 are then ordered by tier, then primary display name, then brightness
(apparent magnitude), then distance. Tested queries: "Epsilon Eridani", "eps
eri", "Ran", "Sirius", "Gliese 581" (→ Gl 581), "HIP 16537", "HD 22049",
"Gaia DR3 5164707970261890560".

The suggestions are an inline list, not an ImGui popup: a popup would steal
keyboard focus from the text field.

A hit that the current filters hide is labelled **(filtered)**. Choosing it
still selects it. **Policy:** the selected star is exempt from filtering
(like Sol), so it stays drawn. The Star tab then says it is "hidden by the
current filters; shown because it is selected" and offers **Clear all
filters**. This way the user never gets an invisible selection, and filters
never change silently.

**Fly-to** (`FlightSystem`) interpolates the orbit *parameters*, not
matrices:
* the target linearly;
* the distance geometrically (constant zoom *rate*, so 2000 pc → 8 pc does
  not rush through the first 99 %);
* yaw along the shorter arc; pitch linearly.

Lerping two view matrices would shear and scale the rotation in between.
Parameters also always produce a valid orbit camera. Progress is `elapsed
seconds / 0.8 s`, eased with ease-in-out cubic, so the flight takes the same
time at 30 or 144 fps.
* Selecting flies the target to the star and zooms in to 8 pc, but never
  zooms out.
* F refocuses, H/Home returns to Sol (keeping the zoom), and R flies back to
  the start-up pose, angles included.
* A new flight starts from the *current* pose, so retargeting mid-flight
  never jumps.
* Wheel or pan cancels a flight. An orbit drag cancels only an
  angle-animating flight (R); during a select flight you may orbit while
  the target glides.

**Details** (`catalog::load_star_details`) are loaded **lazily** from SQLite
when the selection changes: one query by primary key plus one on
`idx_planets_star`, ~0.2–0.4 ms. The loader could put names, planets and
provenance into components for all 92k stars, but they would sit in RAM for
the one star you look at. The names are already in the `NameIndex`, which
search needs anyway. The Star tab shows:
* display and Bayer names;
* distance in ly and pc with its source, RA/Dec (h m s / ° ′ ″), galactic
  l/b and parallax ± error;
* spectral type, temperature, radius, mass, luminosity, age and metallicity,
  each with its source (hover a source for the raw code);
* planets: period, a, M/R in Earth units, method and year;
* magnitudes and colours;
* RUWE (warning above 1.4), parallax S/N, catalogue ids and contributing
  sources;
* all names grouped by catalogue.

Missing values show a dim "n/a". Sol's details get the eight planets from
the NASA fact sheet, because the exoplanet archive, by definition, lists
none.

**Timings** (Release, GCC 14, this VM):

| operation | time |
|---|---|
| click pick, brute force over 91,998 stars | 0.76–1.1 ms (a slow frame: 1.45 ms) |
| hover pick | same cost, only on frames where the mouse moved |
| search, one keystroke (full scan) | 2–5 ms |
| search, worst case seen (one letter) | ~6 ms |
| search index build (at start-up) | 16–18 ms |
| details load | 0.2–0.4 ms |

At these costs, no spatial acceleration structure (grid or BVH) was
justified. Debug builds: pick ~36–54 ms and search ~15–22 ms, because EnTT
and the STL are unoptimised.

### How a frame works

```
startup (App constructor, order matters):
  load_catalog -> registry (entities + StarIndex/NameIndex in ctx)
  CatalogStats -> ColorRanges; ctx: OrbitCamera, ColorSettings{dirty=true}, ViewSettings
  SizeSystem: DisplaySize for every star (once); Sol gets the Marked tag
  Window (SDL3) -> Graphics (bgfx::renderFrame(), then bgfx::init with the native handle)
  ShaderLoader -> StarRenderer / GuideRenderer (GPU buffers, programs, uniforms)

  ImGuiLayer: ImGui context -> imgui_impl_sdl3 -> ImGuiRenderer (font texture created lazily);
              DejaVuSans merged in as a fallback font (Greek letters, ☉, ⊕) when present
  M3 ctx: NameSearch (normalised keys), SelectionState, SelectionDetails, PointerInput, HoverState,
          CameraFlight; a second read-only SQLite connection for details; --select / --search applied

every frame (App::run):
  1. SDL_PollEvent loop -> ImGui first, then InputHandler edits ctx (camera, scheme, dirty flags,
     PointerInput click/hover, flight cancel) unless io.WantCaptureMouse / WantCaptureKeyboard;
     PIXEL_SIZE_CHANGED -> bgfx::reset
  2. InteractionSystem::process_pointer: click -> PickingSystem (last frame's matrices) ->
     select + fly_to, or deselect / fly home; right-click -> TagMenuRequest (no select, no fly);
     hover -> HoverState for the tooltip
  3. SelectionSystem::update_details: lazy SQLite load if SelectionState.version changed
  4. ImGui::NewFrame; SidePanel: SearchBox (a commit calls select_and_fly), Filters tab edits
     ColorSettings / FilterSettings (including distance and the tagged-only view), Star tab
     shows SelectionDetails; draw_tag_menu opens the 16-colour popup from a TagMenuRequest
     (also when the panel is hidden); returns the panel width
     -> the 3D viewport is window width - panel width (points * DisplayFramebufferScale = pixels)
  5. FlightSystem::update_flight(dt seconds; fixed 1/60 s in --screenshot mode)
  6. CameraSystem::update  -> view + projection matrices for the 3D viewport
  7. ColorSystem::update_colors (if ColorSettings.dirty; tagged stars are then overwritten
     with their tag colour while the tagged-only view is on); FilterSystem (if
     FilterSettings.dirty, the scheme or the selection changed): FilteredOut tags from the
     scheme filters, the distance range and the tagged-only view (selection exempt) + visible count
  8. view 0: clear (whole window); view 1: guides + dashed Sol->selection line (transient VB);
     view 2: StarRenderSystem packs view<const Position, const DisplayColor, const DisplaySize>
     (exclude<FilteredOut>) into transient instance buffers -> one instanced draw per batch
     HudSystem: ImGui overlays (status, controls, ring labels, selection reticle, Sol-line label,
     hover tooltip); ImGui::Render -> ImGuiRenderer -> view 3 (orthographic, scissor per command)
  9. bgfx::frame()  (single-threaded: renders and presents right here)
```

### Rendering choices (details in the code comments)

* **Coordinates:** galactic, parsecs, Sun at the origin. +x points to the
  Galactic Centre, +y along the rotation, +z to the North Galactic Pole. The
  camera treats **+z as up**, so the galactic plane is the horizontal "floor".
* **Instancing:** one 4-vertex unit quad. Each star is one 48-byte instance
  (`i_data0` = position + radius, `i_data1` = rgb + intensity, `i_data2` =
  flags). The vertex shader expands the quad in clip space, so each billboard
  always faces the camera and has a minimum and maximum size in pixels.
* **Blending:** additive (`BLEND_ADD`). Depth test and depth write are both
  off: glows are translucent light, and with additive blending the draw order
  doesn't matter, so no sorting is needed.
* **Colour schemes:**
  * Temperature: blackbody from Teff.
  * Spectral class: OBAFGKM + L/T/Y + D palette.
  * Diameter: log radius on viridis.
  * Age: viridis.
  * Metallicity: diverging blue–white–orange.
  * Planets: hosts are emphasised on a log colour scale.
  * Continuous ranges come from `CatalogStats` p01/p99. Unknown values are grey and dimmed.

### Dear ImGui on bgfx (details in `ImGuiRenderer.h/.cpp`)

* The platform side is the official `imgui_impl_sdl3` backend: events, time,
  cursors, clipboard, DPI. The renderer side is our own backend
  (`render::ImGuiRenderer`), because ImGui ships no bgfx backend.
  * ImGui 1.92's texture protocol (`ImGuiBackendFlags_RendererHasTextures`):
    ImGui asks the backend to create, update (sub-rectangles) and destroy
    textures, so fonts are rasterised on demand at the size in use.
  * Vertices and indices go into **transient** buffers. They are rebuilt
    every frame, and transient memory is bgfx's cheapest path for that: no
    handles to manage, freed automatically after the frame.
  * The UI renders in its **own bgfx view** (id 3): a pixel-space
    orthographic projection, sequential mode so draw order = submission
    order, drawn after the 3D views so it is on top. Each draw command sets
    its scissor rectangle.
* HiDPI: sizes and fonts are scaled by the display's content scale.
  `DisplayFramebufferScale` converts ImGui points to back-buffer pixels for
  the scissor rectangles and the 3D viewport.

### Platform status

Tested on Linux (GCC 14 and Clang 19, X11, OpenGL 4.3 on Mesa llvmpipe
software rendering). The Dear ImGui panel was exercised interactively
with xdotool (slider drags, checkboxes, text entry, scheme switching, panel
resize, P/Tab). HiDPI scaling (>1 pixel per point) is written but untested
on this 1:1 display. The Windows (D3D11/12), macOS (Metal), Vulkan and
Wayland code paths are written and compiled for Linux where applicable, but
**not yet run**. The shaders are compiled for those backends on their own
platforms only.

## The catalog library

### Building (library only)

Requirements: CMake ≥ 3.24, a C++20 compiler (MSVC 2022 17.4+, GCC 11+, Clang 14+ / Xcode 15+),
and internet access on the first configure (FetchContent downloads pinned, hash-checked archives
into `build*/_deps`). No system packages are needed.

**Linux / macOS**

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release   # or omit -G Ninja for Makefiles
cmake --build build-release
ctest --test-dir build-release --output-on-failure
./build-release/catalog_probe ../starcatalog/stars.db
```

**Windows (Visual Studio 2022, x64 Developer PowerShell)**

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\build\Release\catalog_probe.exe ..\starcatalog\stars.db
```
(Names contain UTF-8 such as `Omicron² Eridani`; run `chcp 65001` for a readable console.)
Ninja from a Developer prompt also works: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`.

**Options**

| option | default | meaning |
|---|---|---|
| `STARMAP_USE_SYSTEM_SQLITE` | OFF | use `find_package(SQLite3)` instead of downloading the amalgamation |
| `STARMAP_BUILD_TESTS` | ON | build `starcatalog_tests` + register with ctest |
| `STARMAP_BUILD_PROBE` | ON | build `catalog_probe` |
| `STARMAP_WARNINGS_AS_ERRORS` | OFF | `-Werror` / `/WX` for project code |
| `STARMAP_TEST_DB` | `../starcatalog/stars.db` | database the tests load |

### Pointing it at stars.db

* **Tests:** `-DSTARMAP_TEST_DB=/path/to/stars.db` at configure time, or the environment variable
  `STARMAP_DB=/path/to/stars.db` at run time (wins). If the file is missing, the catalog tests print
  a message and pass (skip); the sqlite/NameIndex unit tests always run.
* **Probe:** first argument. `catalog_probe path/to/stars.db [--max-ly N] [--min-snr N]
  [--max-ruwe N] [--name "Epsilon Eridani"]... [--lean]` (`--lean` skips planets + provenance).
  With no `--name`, it looks up Sol, Epsilon Eridani, Omicron^2 Eridani, Gamma Pavonis, Proxima Centauri.
* **Your app:** `LoadOptions::db_path`. The db is opened `SQLITE_OPEN_READONLY`, so it can ship
  read-only next to the executable.

## API overview

```cpp
#include "catalog/CatalogLoader.h"
#include "catalog/CatalogStats.h"
#include "catalog/Indexes.h"
#include "ecs/Components.h"
#include <entt/entity/registry.hpp>

using namespace starmap;

entt::registry registry;
catalog::LoadOptions opt;
opt.db_path = "stars.db";
opt.max_dist_ly = 100.0;          // default 200
opt.min_parallax_snr = 10.0;      // optional; only Gaia-parallax rows are filtered
// opt.max_ruwe = 1.4;            // optional; ditto. Careful: bright nearby stars often have
                                  // high RUWE (eps Eri 2.7, 40 Eri 1.9) and would be dropped.
opt.load_planets = true;          // default false
catalog::LoadResult r = catalog::load_catalog(registry, opt);   // throws catalog::CatalogError
std::printf("%zu stars, %zu names in %.0f ms\n", r.stars, r.names, r.elapsed_ms);

// Names -> entity (ASCII case-insensitive exact match)
const auto& names = registry.ctx().get<catalog::NameIndex>();
if (auto e = names.find("epsilon eridani")) {
    const auto& [info, pos, phys] = registry.get<ecs::StarInfo, ecs::Position, ecs::Physical>(*e);
    std::printf("%s: %.2f ly, %d planet(s)\n", info.display_name.c_str(), pos.dist_ly, phys.planet_count);
    for (const catalog::NameEntry& n : names.names_of(*e))   // primary first
        std::printf("%s [%s]%s\n", n.name.c_str(), n.catalog.c_str(), n.is_primary ? " *" : "");
}
// star_id -> entity
auto sol = registry.ctx().get<catalog::StarIndex>().find(1);

// Ranges for colormaps / legend
auto stats = catalog::CatalogStats::compute(registry);
if (const auto* t = stats.field("teff_k"))
    std::printf("teff: %zu stars (%.0f%%), p01..p99 = %.0f..%.0f K\n",
                t->count, 100 * t->coverage(stats.total_stars), t->p01, t->p99);
```

**Entities & components** (one entity per `stars` row):

| component | content |
|---|---|
| `StarId` | `stars.star_id` (Sol = 1) |
| `Position` | `galactic_pc` (x,y,z), `equatorial_pc` (x_eq,y_eq,z_eq), `dist_ly` — float |
| `StarInfo` | `display_name`, optional `bayer_name`, `spectral_type`; `spectral_class` char (`'?'` unknown); `is_sol` |
| `Photometry` | optional `app_mag`, `abs_mag`, `bp_rp`, `ci_bv` |
| `Physical` | optional float `teff_k`, `radius_sol`, `mass_sol`, `luminosity_sol`, `age_gyr`, `metallicity`; `planet_count` |
| `CatalogIds` | optional int64 `gaia_source_id`, `hip`, `hd`, `hyg_id` |
| `Quality` | optional `parallax_mas`, `parallax_error_mas`, `ruwe`; `parallax_snr()` |
| `DisplayColor` | `glm::vec4 rgba` — loader emplaces grey (0.5,0.5,0.5,1) |
| `Selected` | empty tag; never added by the loader |
| `Provenance` | all `*_source` strings, `dist_source`, `app_mag_band`, `sources` — only with `load_provenance` |
| `Planets` | `std::vector<Planet>` (name, disc_year, method, period, a, e, radius, mass, Teq) — only with `load_planets`, only on exoplanet hosts |

**Context (`registry.ctx()`)**: `StarIndex` (star_id → entity, always) and `NameIndex` (if `load_names`).
Loading again replaces both (`ctx().insert_or_assign`).

**Name lookup notes**

* Exact match after ASCII lower-casing: `"Epsilon Eridani"`, `"epsilon eridani"`, `"Ran"`, `"HIP 16537"`,
  `"hd 22049"`, `"Omicron^2 Eridani"`, `"Omicron² Eridani"`, `"40 Eridani"`, `"Gl 144"`,
  `"Gaia DR3 5164707970261890560"`, `"Sol"`/`"Sun"` all work (formats as stored by the Python builder).
* **Limitation:** folding is ASCII-only. Non-ASCII bytes are compared verbatim, so `"ε Eri"` matches but
  `"Ε ERI"` (Greek capital) does not; no accent/whitespace normalisation. Full Unicode folding would need ICU.
* If several stars share a name (binary components' shared designations), `find()` prefers the star for
  which it is the primary name; `find_all()` returns all of them.

**Filtering** happens in SQL. `min_parallax_snr` / `max_ruwe` apply only to rows with
`dist_source = 'gaia_parallax'`; HYG-only stars, exoplanet-archive distances and Sol are always kept.

## Plugging into the frame loop

```
startup:   load_catalog(registry, opt)                       // entities + ctx indexes
           stats = CatalogStats::compute(registry)           // colormap ranges, legend coverage
on change of colour mode (spectral / teff / radius / age / metallicity / planets):
           color system:  view<const Physical, const StarInfo, DisplayColor>  -> writes DisplayColor
                          (stars lacking the field -> neutral "no data" colour, e.g. grey + low alpha)
every frame:
           render system: view<const Position, const DisplayColor>  -> fill a bgfx instance buffer
           picking/UI:    (implemented in M3) PickingSystem on click, systems::select keeps the
                          single Selected tag + SelectionState in sync, NameSearch over the
                          NameIndex, load_star_details() for the info panel
```

EnTT views iterate only the packed arrays of the listed components, so the render system never touches
names or provenance strings. Filtering stars at runtime (e.g. hide RUWE > 1.4) can be done with a
`Hidden` tag and `view<const Position, const DisplayColor>(entt::exclude<Hidden>)`.

## Performance (this machine: 8-core Xeon VM, GCC 14, Release)

Full catalog (91,998 stars, 116,916 names): ~230–245 ms in a fresh `catalog_probe` process (~200 ms for repeat loads) with default options, ~305–320 ms with planets +
provenance; peak RSS of `catalog_probe` ≈ 83 MB (lean) / 115 MB (planets + provenance). Debug ≈ 1.2 s.

App (milestone 3, Release): pick 0.8–1.1 ms, search keystroke 2–5 ms, search index 16–18 ms at
start-up, details 0.2–0.4 ms per selection (see "Selection, search, fly-to and details").

## Known issues / not yet covered (milestone 3)

* Picking has no occlusion: a star is picked by screen distance even if a
  nearer star's glow covers it. That is intentional for point sprites, but it
  can surprise when two stars overlap exactly (the one nearer the camera wins
  ties).
* In the zoomed-out overview (all 92k stars in a small disc), almost every
  click hits some visible star, so "click empty space to deselect" works only
  where there really is space. Use Esc or the Deselect button.
* Search is ASCII-case-insensitive only: "Ε ERI" (Greek capital) does not
  match "ε Eri"; typing "eps eri" does. Ranking prefers primary names, so
  "eps" lists stars *named* Epsilon ... before Ran (Epsilon Eridani, whose
  display name is Ran).
* The hover tooltip is re-picked only when the mouse moves, so during a
  flight it can briefly name the star that *was* under the cursor.
* Wayland, Vulkan, Windows and macOS were not re-tested for milestone 3
  (only X11 + OpenGL on this VM). The fallback font path is Linux-specific
  (optional; without it the ☉ / ⊕ glyphs fall back to "Rsun" / "Mearth").
