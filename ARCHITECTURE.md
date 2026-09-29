# starmap architecture reference

Companion to `AGENTS.md` (the short rules file coding agents load) and `README.md` (the user
guide, with the full frame flow under "How a frame works"). The source comments are the primary
documentation. This file is a map to them.

## Targets (CMakeLists.txt)

| target | contents | links |
|---|---|---|
| `starcatalog` | `src/catalog/*`: SQLite wrapper, loader, indexes, search, details | EnTT, glm, SQLite |
| `starmap_core` | CLI parsing, colour schemes/maps, camera, filters, selection, picking, flights, range-slider maths. No window, GPU or ImGui | starcatalog, bx |
| `starmap_render` | window, bgfx renderers, HUD, input, ImGui layer and panels, `App` | starmap_core, bgfx, bimg, SDL3, starmap_imgui |
| `starmap` | `apps/starmap/main.cpp` | starmap_render |
| `catalog_probe` | `apps/catalog_probe.cpp` console tool | starcatalog |
| `starcatalog_tests`, `starmap_app_tests` | doctest executables (`tests/`) | starcatalog / starmap_core |
| `starmap_shaders` | shaderc outputs in `<build>/shaders/{glsl,spirv}` (Linux) | built before `starmap` |

The split exists so everything in `starmap_core` can be unit-tested on a headless machine.

## Data flow

1. **Startup** (`App` constructor, member order is load-bearing: see `App.h`):
   `load_catalog()` creates one entity per star with its components and puts `StarIndex` and
   `NameIndex` in `registry.ctx()`. `CatalogStats` provides the colour and filter domains.
   `assign_display_sizes()` runs once. Sol gets `Marked`. Then come the SDL window, bgfx init,
   renderers, ImGui, `NameSearch`, the selection and flight ctx state, and a second read-only
   SQLite connection for details. `--select`/`--search` are applied last.
2. **Each frame** (`App::run`):
   - SDL events go to ImGui first, then to `InputHandler` unless ImGui wants them.
   - `process_pointer` handles clicks (`pick_star` using the previous frame's matrices, then
     `select_and_fly` or `clear_selection`) and hover.
   - `update_details` runs, then ImGui `NewFrame` and `SidePanel::draw`. The search box can
     select, so `update_details` runs again.
   - Then `update_flight(dt)`, `CameraSystem` update, `update_colors` and `update_filters`.
   - Views are recorded: clear, then guides and the Sol line, then `StarRenderSystem`.
   - Finally `draw_hud`, ImGui render (view 3) and `bgfx::frame()`.
3. **Screenshot mode** (`--screenshot FILE --frames N`) skips input and uses a fixed flight
   dt of 1/60 s. A bgfx callback writes the PNG, then the app exits.

## bgfx views (App.cpp)

| id | name | rect | content |
|---|---|---|---|
| 0 | `kViewClear` | whole window | clear only (keeps the strip under the panel clean) |
| 1 | `kViewGuides` | window minus panel | rings, spokes, galactic-centre line, dashed Sol->selection line |
| 2 | `kViewStars` | window minus panel | instanced star billboards, additive blend |
| 3 | `kViewImGui` | whole window | panel + HUD; sequential mode, orthographic |

Views execute in id order, which is how layering is guaranteed.

## registry.ctx() singletons

| type | header | written by | read by |
|---|---|---|---|
| `StarIndex`, `NameIndex` | catalog/Indexes.h | loader | search, `--focus`, Sol lookups |
| `NameSearch` | catalog/NameSearch.h | App (built once) | SearchBox, `--select` |
| `CatalogStats` | catalog/CatalogStats.h | App | legend, filter domains |
| `ColorSettings` | scene/ColorScheme.h | input keys 1-6, panel | ColorSystem (`dirty`) |
| `FilterSettings` | scene/Filters.h | panel, CLI filters (scheme ranges, distance, tagged-only) | FilterSystem (`dirty`); ColorSystem reads `only_tagged` |
| `ViewSettings` | scene/ViewSettings.h | input keys, panel | renderers, HUD, panel |
| `OrbitCamera` | scene/OrbitCamera.h | input, FlightSystem | CameraSystem, picking |
| `SelectionState` | scene/Selection.h | SelectionSystem only | everything selection-aware |
| `SelectionDetails` | scene/Selection.h | `update_details` | StarDetailsView |
| `PointerInput`, `HoverState` | scene/Selection.h | InputHandler / InteractionSystem | InteractionSystem / HUD |
| `TagMenuRequest` | scene/Tags.h | InteractionSystem (right-click pick) | ui/TagMenu (ImGui popup) |
| `CameraFlight` | scene/CameraFlight.h | FlightSystem, InteractionSystem | FlightSystem |

Rule of thumb from the code: app-wide state that isn't per star goes in ctx, never in globals.

## Components and tags

- Per star (loader): `StarId`, `Position` (galactic pc), `StarInfo`, `Photometry`, `Physical`,
  `CatalogIds`, `Quality`, `DisplayColor`. Optional `Provenance` and `Planets` exist only with
  `LoadOptions::load_provenance/load_planets`, which the app leaves off. The details panel
  queries these lazily instead.
- Render: `scene::DisplaySize` (SizeSystem, once).
- Tags (empty structs, membership only): `scene::FilteredOut` (FilterSystem), `ecs::Selected`
  (SelectionSystem, at most one entity), `scene::Marked` (Sol: always drawn, ringed, pickable).
- `scene::StarTag` (scene/Tags.h): present only on a star the user tagged from the
  right-click menu. The payload is a palette index, 0..15. FilterSystem's tagged-only
  view hides stars that lack it; ColorSystem copies the palette into `DisplayColor`
  while that view is on.

## How-tos

### Add a shader
1. Write `shaders/vs_x.sc` / `fs_x.sc` in bgfx's dialect (`$input`/`$output`, `#include <bgfx_shader.sh>`).
2. Declare attributes/varyings in `shaders/varying.def.sc`. A different input signature
   gets its own def file, like `varying_imgui.def.sc`.
3. Add them to `cmake/Shaders.cmake`: a `bgfx_compile_shaders(TYPE VERTEX|FRAGMENT ...)` call
   (profiles `430 spirv` on Linux) and the `starmap_shaders` target's DEPENDS/SOURCES.
4. Load with `ShaderLoader::load_program("vs_x", "fs_x")`. It picks `shaders/<backend>/`.
5. Rebuild and check the result with a screenshot.

### Add a colour scheme
- `scene/ColorScheme.h`: enum value before `Count`. `ColorScheme.cpp`: key/title table, any
  aliases (e.g. "feh"), HUD legend text.
- `systems/ColorSystem.cpp`: the colour for a star (both switches).
- `scene/Filters.cpp`: domain from `CatalogStats` and the per-star filter value.
- `ui/FilterPanel.cpp`: legend ticks/format and slider config.
- `app/CommandLine.cpp` `usage_text()` (`--scheme` list), README, and tests in `tests/test_app_core.cpp` / `test_filters.cpp`.
- Keys 1-9 map to scheme indices generically (`InputHandler.cpp`).

### Add a filter
Scheme filters are per scheme (`FilterSettings` in `scene/Filters.h`). The predicate goes in
`Filters.cpp`, application in `systems/FilterSystem.cpp` (whoever edits `FilterSettings` sets `dirty = true`), UI in `ui/FilterPanel.cpp`,
CLI in `FilterOptions` + `apply_filter_options()` and `CommandLine.cpp`, tests in `tests/test_filters.cpp`.
Distance from Sol and the tagged-only view are not schemes: they live on `FilterSettings` directly
(`distance_lo`/`distance_hi`, `only_tagged`) and are ANDed with the scheme filters. `only_tagged`
also sets `ColorSettings::dirty`, because the palette overwrite happens in `ColorSystem`.
A right-click tag is `scene::StarTag` plus `systems::set_star_tag` / `ui::draw_tag_menu`.

### Add a CLI flag
`AppOptions` (`app/CommandLine.h`) -> parse in `parse_command_line()` and document in
`usage_text()` (`CommandLine.cpp`) -> apply in `App.cpp` -> test in `tests/test_app_core.cpp`
(parse success and error cases) -> README "Command line".

### Add a system
A free function over `entt::registry&` in `src/systems/`. Put it in `starmap_core` if it has
no SDL/bgfx/ImGui calls, so it can be tested. Call it at the right point in `App::run` and
update the frame-order comments in `App.h` and README.

## Testing

- `tests/test_main.cpp` holds doctest's `main`. Other files only include `<doctest/doctest.h>`.
- Catalog tests use `STARMAP_DB` or the baked-in `../starcatalog/stars.db`, and print a skip
  message when it's missing.
- Headless tests build synthetic registries (see `tests/test_selection.cpp` for picking and
  flights on hand-made stars).
- Filter a run: `./build-debug/starmap_app_tests -tc="pick_star*"`. List cases: `-ltc`.

## Docker

See the README section "Building with Docker", `Dockerfile` and `docker/entrypoint.sh`.
Dependency sources are prefetched into the image from `cmake/DependencyPins.cmake` via
`cmake/Prefetch.cmake`, so container builds work offline.
