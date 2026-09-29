# starmap: rules for coding agents

A toy interactive 3D map of the ~92,000 stars within 200 light years of Sol, read from a SQLite
catalog. The real purpose is LEARNING: the user is a C++ programmer studying SDL3, bgfx, EnTT,
Dear ImGui and C++20 for a bigger game later. Longer reference: `docs/ARCHITECTURE.md` and `README.md`.

## The key rule: code is teaching material
- Comment every new or changed piece exhaustively, explaining WHY: why this SDL3/bgfx/EnTT/ImGui
  call, why this order, what breaks otherwise, which alternative was rejected. Not just what.
- Match the existing style: a `// ====` header block per file (role in the architecture, key
  design choices), then inline comments at each non-obvious call. Read neighbouring files first.
- Never strip or shorten existing explanatory comments when refactoring; move or update them.
  Fix comments that your change makes stale.
- Record user-visible changes in `README.md` (controls, CLI, frame flow, known issues).

## Stack (all pinned in `cmake/DependencyPins.cmake`)
C++20, CMake >= 3.24, Ninja. SDL3 3.4.16 (static), bgfx.cmake v1.161.9510-579 (bgfx, bx, bimg,
shaderc), Dear ImGui v1.92.9b, EnTT 3.16.0, glm 1.0.3, SQLite 3.53.4 amalgamation, doctest 2.5.3.
Bump versions ONLY in `DependencyPins.cmake` (URL + hash); the Docker prefetch reads the same file.
Never bump casually: ask the user first.

## Build and test (run from the project root)
```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release && ctest --test-dir build-release --output-on-failure
cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSTARMAP_WARNINGS_AS_ERRORS=ON
cmake --build build-debug && ctest --test-dir build-debug --output-on-failure
CC=clang-19 CXX=clang++-19 cmake -S . -B build-clang -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTARMAP_WARNINGS_AS_ERRORS=ON
cmake --build build-clang && ctest --test-dir build-clang --output-on-failure
```
Docker (image = build environment; `/build` must exist before the run; `:z`/`:Z` for SELinux):
```sh
docker build -t starmap-build .
mkdir -p ~/starmap-build
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/src:ro,z -v ~/starmap-build:/build:Z starmap-build
```
Options: `-e BUILD_TYPE=Debug -e WERROR=1`; add `-v /path/to/starcatalog:/starcatalog:ro,z` or
the stars.db tests skip. Never reuse a build dir between Docker and a host cmake.

## Definition of done
1. Zero warnings in project code with `STARMAP_WARNINGS_AS_ERRORS=ON` under GCC 14 AND clang 19
   (warnings inside `_deps` are third-party and don't count).
2. `ctest` green in all builds; new logic has doctest tests.
3. Visual check in screenshot mode, then look at the PNG:
   `./build-release/starmap ../starcatalog/stars.db --select "Epsilon Eridani" --scheme spectral --screenshot /tmp/shot.png --frames 10`
   (more flags: `--camera D,YAW,PITCH`, `--pick X,Y`, `--search TEXT`, `--filter S=LO:HI`, `--help`).

Runtime: the binary needs `shaders/` (compiled `glsl/`, `spirv/`) next to it. stars.db is the first
argument, or found next to the binary / in `../starcatalog/`. It comes from the separate Python
project `starcatalog` (`build_star_catalog.py`), not this repo; catalog data bugs are fixed there.

## Architecture map
- `src/catalog/` SQLite RAII wrapper, CatalogLoader (stars -> entities), StarIndex, NameIndex,
  NameSearch, CatalogStats, StarDetails. `src/ecs/Components.h` per-star components.
- `src/scene/` plain state: OrbitCamera, ColorScheme/ColorSettings, Filters, Selection,
  CameraFlight, ViewSettings, RenderComponents (DisplaySize, Marked).
- `src/systems/` logic over the registry; `src/render/` bgfx; `src/ui/` ImGui; `src/input/`
  SDL events; `src/platform/` SDL window; `src/app/` App (frame loop), CommandLine.
- `shaders/` bgfx `.sc`; `tests/`; `cmake/`; `docker/`; `apps/` (main.cpp, catalog_probe).
- Libraries: `starcatalog` (data, no graphics), `starmap_core` (no SDL/bgfx rendering/ImGui, unit
  tested), `starmap_render` (SDL3, bgfx, ImGui), `starmap` exe.

Frame order (`App::run`): SDL events (ImGui first, InputHandler only if ImGui doesn't want the
input) -> process_pointer (pick/select/fly) -> update_details -> ImGui NewFrame + SidePanel ->
update_details -> update_flight -> CameraSystem -> update_colors -> update_filters -> views ->
HUD -> ImGui render -> `bgfx::frame()`.
bgfx views: 0 clear (whole window), 1 guides + Sol line, 2 stars, 3 ImGui (drawn last).

Components: StarId, Position, StarInfo, Photometry, Physical, CatalogIds, Quality, DisplayColor,
DisplaySize; optional Provenance/Planets (off in the app). Tags: `FilteredOut`, `Selected`, `Marked` (Sol).
`registry.ctx()`: StarIndex, NameIndex, NameSearch, CatalogStats, ColorSettings, FilterSettings,
ViewSettings, OrbitCamera, SelectionState, SelectionDetails, PointerInput, HoverState, CameraFlight.

Invariants:
- At most one entity has `Selected`; change it only via `systems::select()`/`clear_selection()`
  (they keep ctx SelectionState in sync). The selection is exempt from filtering.
- ColorSystem/FilterSystem run only when their `dirty` flag (or scheme/selection) changed: set
  `dirty = true` after editing ColorSettings/FilterSettings.
- Star details, names and planets load lazily from SQLite on selection (StarDetails), not into
  components.
- Picking is screen-space and mirrors the star shader's size/visibility; change both together.

## How to extend (details in docs/ARCHITECTURE.md)
- Shader: add `shaders/vs_x.sc`/`fs_x.sc`, declare inputs/varyings in `varying.def.sc` (or a new
  def file), add them to `cmake/Shaders.cmake` (both `bgfx_compile_shaders` and the
  `starmap_shaders` SOURCES), load with `ShaderLoader::load_program("vs_x", "fs_x")`.
- Colour scheme: `scene/ColorScheme.h/.cpp` (enum before `Count`, key/title), `systems/ColorSystem.cpp`,
  `scene/Filters.cpp` (domain + value), `ui/FilterPanel.cpp` (legend/slider), README, tests.
- CLI flag: `AppOptions` in `app/CommandLine.h`, parse + `usage_text()` in `CommandLine.cpp`,
  apply in `App.cpp`, test in `tests/test_app_core.cpp`, README "Command line".

## Tests
doctest. `starcatalog_tests` (catalog, search; real stars.db via `STARMAP_DB` or
`../starcatalog/stars.db`, skipped if missing) and `starmap_app_tests` (starmap_core only, headless).
Split pure logic into GUI-free functions and test those (like `ui/RangeSliderMath`,
`PickingSystem`, `FlightSystem`). New test files must be added in `CMakeLists.txt`.
One test: `./build-debug/starmap_app_tests -tc="pick_star*"`.

## Avoid
- New runtime-linked libraries: the binary links only libc/libm/libstdc++/libgcc_s; SDL dlopen()s
  X11/Wayland/GL/audio. Keep deps static or dlopen-based.
- Writing into the source tree during a build (Docker mounts `/src` read-only).
- Allocating per frame in hot loops: reuse buffers (see `StarRenderSystem::instances_`), use
  bgfx transient buffers, and avoid per-star `std::string` work per frame.
- Direct SDL/bgfx/ImGui calls in `starmap_core` code (keeps it testable).
- Editing `probe_*.txt`, `screenshots/` or build dirs as if they were source.

## Known issues / next ideas
- HiDPI (>1 px per point) untested; Wayland, Vulkan, Windows, macOS never run.
- Panel width not persisted (`io.IniFilename = nullptr`); filter min/max fields show values
  rounded to the display format.
- Hover tooltip can be stale during a camera flight (re-picked only on mouse move).
- Upstream catalog errors (fix in the Python script, not here): Bubup/HD 38283 parsed as W
  (is F9.5 V), GJ 1054A as S (is M); HIP 80365, 10332, 40546 lie beyond 200 ly per Gaia and
  shouldn't be in the catalog; Sol has a duplicate 'Sol' name row.
