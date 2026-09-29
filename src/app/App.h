#pragma once
// =============================================================================
// app/App.h — owns everything and runs the frame loop.
//
// ARCHITECTURE AT A GLANCE
//
//   startup:  CommandLine -> load_catalog() -> registry (entities + ctx)
//             Window (SDL3)  ->  Graphics (bgfx::init)  ->  renderers (GPU resources)
//
//   each frame (App::run):
//     1. SDL_PollEvent  -> ImGuiLayer (always first), then InputHandler unless
//                          ImGui wants the mouse/keyboard (camera, scheme, toggles)
//     2. ImGui frame    -> side panel edits ColorSettings / FilterSettings,
//                          the tag menu may set a StarTag, and the panel
//                          reports its width, so the 3D viewport = window - panel
//     3. CameraSystem   -> view/proj matrices in ctx OrbitCamera (scene viewport)
//     4. ColorSystem    -> DisplayColor      (only if ColorSettings::dirty)
//        FilterSystem   -> FilteredOut tags  (only if FilterSettings::dirty)
//     5. view 0 clear   (whole window)
//        GuideRenderer  -> view 1 (rings)
//        StarRenderSystem -> StarRenderer -> view 2 (instanced stars, additive,
//                          skipping FilteredOut entities)
//        HudSystem + panel -> ImGuiRenderer -> view 3 (UI, drawn last = on top)
//     6. bgfx::frame()  -> everything recorded above is sorted by view and
//                          executed on the GPU, then presented.
//
// MEMBER ORDER IS LOAD-BEARING: C++ constructs members top to bottom and
// destroys them bottom to top. bgfx resources (renderers) are declared AFTER
// Graphics so they are destroyed BEFORE bgfx::shutdown(); Graphics after
// Window so bgfx shuts down before the window it renders into disappears; the
// screenshot callback before Graphics because bgfx holds a pointer to it until
// shutdown. ImGuiLayer owns bgfx textures/buffers too, so it also comes after
// Graphics (and is destroyed before bgfx::shutdown()). The catalog is loaded before the window opens, so a missing
// stars.db produces an error message instead of a flashing window.
// =============================================================================

#include "app/CommandLine.h"
#include "catalog/CatalogLoader.h"
#include "catalog/Sqlite.h"
#include "input/InputHandler.h"
#include "platform/Window.h"
#include "render/Graphics.h"
#include "render/GuideRenderer.h"
#include "render/ScreenshotCallback.h"
#include "render/ShaderLoader.h"
#include "render/StarRenderer.h"
#include "systems/StarRenderSystem.h"
#include "systems/PickingSystem.h"
#include "ui/ImGuiLayer.h"
#include "ui/SidePanel.h"

#include <entt/entity/registry.hpp>

#include <memory>

namespace starmap::app {

class App {
public:
    explicit App(AppOptions options);  // throws std::runtime_error / catalog::CatalogError
    int run();                         // returns the process exit code

private:
    AppOptions options_;
    entt::registry registry_;             // the ECS world: one entity per star + ctx singletons
    catalog::LoadResult load_result_;     // filled by loading the catalog (initialised before the window)
    render::ScreenshotCallback callback_;
    platform::Window window_;
    render::Graphics graphics_;
    render::ShaderLoader shaders_;
    render::StarRenderer star_renderer_;
    render::GuideRenderer guide_renderer_;
    ui::ImGuiLayer imgui_;                // Dear ImGui context + SDL3 backend + our bgfx backend
    ui::SidePanel panel_;                 // the right-hand panel: search + Filters / Star tabs
    systems::StarRenderSystem star_render_system_;
    input::InputHandler input_;
    render::StarRenderParams star_params_;
    // Milestone 3: a second, read-only SQLite connection kept open for the
    // lazy per-selection detail queries (catalog/StarDetails.h). The loader's
    // connection is closed after loading; reopening per click would cost
    // ~1 ms of file open + schema parse each time, keeping one costs nothing.
    // unique_ptr because Database has no "empty" state and may fail to open
    // (the app still works without details).
    std::unique_ptr<catalog::Database> details_db_;

    [[nodiscard]] systems::PickParams pick_params() const;
    void apply_startup_selection();  // --select / --search
};

}  // namespace starmap::app
