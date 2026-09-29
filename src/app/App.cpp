// =============================================================================
// app/App.cpp — startup (scene setup) and the frame loop.
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   App is the one object that owns everything: the ECS registry, the SDL
//   window, the bgfx context and the GPU resources. It wires the systems
//   together in a fixed order every frame. main.cpp only parses the command
//   line and calls App(options).run().
//
// THE FRAME LOOP (App::run), in the order it runs and why:
//   1. EVENTS   SDL_PollEvent until the queue is empty. Each event goes to
//               Dear ImGui first; our InputHandler then sees it only if ImGui
//               does not want that device (see input/InputHandler.cpp). Input
//               mutates only ctx state (camera angles, scheme, dirty flags,
//               PointerInput "a click happened at (x, y)").
//   2. POINTER  InteractionSystem::process_pointer() turns a pending click
//               into a pick (screen-space nearest star), a selection and a
//               fly-to, and refreshes the hover tooltip target. A right click
//               picks the same way and leaves a TagMenuRequest instead of
//               selecting. It runs AFTER all events and BEFORE the camera
//               update on purpose: the pick must use last frame's matrices,
//               i.e. exactly the picture the user was looking at when they clicked.
//   3. DETAILS  SelectionSystem::update_details() loads the selected star's
//               full row + planets from SQLite if the selection changed
//               (lazy: one indexed query, ~0.1-1 ms, only on change).
//   4. UI       ImGuiLayer::begin_frame(), then SidePanel::draw() and the tag
//               menu. ImGui is an IMMEDIATE-MODE GUI: there is no widget tree
//               to build once; every frame we call ImGui::Checkbox(...) etc.
//               and the call both draws the widget and returns whether the
//               user changed it. The panel writes straight into
//               ColorSettings/FilterSettings, a committed search calls
//               select_and_fly(), and the tag menu calls set_star_tag().
//               Running it BEFORE the systems means an edit made this frame
//               is visible this frame. The panel returns its width, from
//               which we derive the 3D viewport (window minus panel). The
//               tag menu is drawn even when the panel is hidden.
//   5. FLIGHT   FlightSystem::update_flight(dt) advances an active camera
//               flight (eased, frame-rate independent: progress is elapsed
//               SECONDS / duration, not a per-frame step).
//   6. CAMERA   systems::update() turns the orbit parameters into view and
//               projection matrices for the scene viewport.
//   7. DERIVED  update_colors() recolours all stars (and, while the tagged
//               view is on, overwrites tagged stars with their tag colour)
//               and update_filters() re-tags FilteredOut stars, each only if
//               its dirty flag (or the scheme, or the selection) changed.
//               Most frames both return immediately.
//   8. RECORD   bgfx calls (setViewTransform, setInstanceDataBuffer, submit)
//               do NOT draw yet. They append commands to bgfx's frame buffer.
//               bgfx sorts them by view and state. Guides + the Sol-to-
//               selection line go to view 1, stars to view 2. The HUD
//               (status, help, ring labels, selection reticle, hover tooltip)
//               is also built with ImGui here, and ImGuiLayer::end_frame()
//               converts all UI into bgfx draw calls on the last view.
//   9. FRAME    bgfx::frame() hands the recorded frame to the renderer. In our
//               single-threaded setup (see render/Graphics.cpp) the GPU work
//               is issued and the image presented inside this call, which
//               also blocks for vsync unless --no-vsync.
//   Then timing, the title bar, and (in --screenshot mode) the exit condition.
//
// This is a "variable timestep, render every frame" loop. The only thing that
// animates is the camera flight, which scales by the measured frame time. In
// --screenshot mode a FIXED dt of 1/60 s is used instead, so a given
// --frames N always produces the same picture however fast the frames render.
// =============================================================================
#include "app/App.h"

#include "catalog/CatalogStats.h"
#include "scene/Tags.h"
#include "ui/TagMenu.h"
#include "catalog/Indexes.h"
#include "ecs/Components.h"
#include "scene/ColorScheme.h"
#include "scene/OrbitCamera.h"
#include "scene/RenderComponents.h"
#include "scene/ViewSettings.h"
#include "systems/CameraSystem.h"
#include "scene/Filters.h"
#include "systems/ColorSystem.h"
#include "systems/FilterSystem.h"
#include "systems/HudSystem.h"
#include "systems/SizeSystem.h"
#include "catalog/NameSearch.h"
#include "scene/CameraFlight.h"
#include "scene/Selection.h"
#include "systems/FlightSystem.h"
#include "systems/InteractionSystem.h"
#include "systems/SelectionSystem.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace starmap::app {

namespace {

// bgfx "views" are numbered render passes, executed in increasing ViewId order
// at bgfx::frame(). Each has its own viewport, clear settings and camera
// transform. Within a view bgfx may re-order draw calls (to minimise state
// changes); ACROSS views the order is guaranteed. We use that to draw the
// guide lines strictly before (= underneath) the stars, and the UI after
// (= on top of) everything.
//   0  clear   covers the WHOLE window and only clears it. The 3D views are
//              narrowed to the area left of the panel; a clear only affects
//              its own view's rectangle, so without this view the strip
//              under the (slightly translucent) panel would show stale
//              pixels from earlier frames.
//   1  guides  rings/arrow (scene viewport)
//   2  stars   instanced billboards (scene viewport)
//   3  imgui   the panel + HUD, full window, orthographic, drawn last
constexpr bgfx::ViewId kViewClear = 0;
constexpr bgfx::ViewId kViewGuides = 1;
constexpr bgfx::ViewId kViewStars = 2;
constexpr bgfx::ViewId kViewImGui = 3;

std::filesystem::path resolve_db(const AppOptions& o) {
    if (o.db_path) return *o.db_path;
    std::vector<std::filesystem::path> tried;
    if (auto p = find_default_db(platform::executable_dir(), std::filesystem::current_path(), &tried)) return *p;
    std::string msg = "stars.db not found. Pass its path as the first argument, e.g.\n"
                      "  starmap ../starcatalog/stars.db\n"
                      "(build it with starcatalog/build_star_catalog.py). Looked in:";
    for (const auto& t : tried) msg += "\n  " + t.string();
    throw std::runtime_error(msg);
}

// Load the catalog and set up every ctx variable the systems expect.
catalog::LoadResult load_scene(entt::registry& registry, const AppOptions& o) {
    catalog::LoadOptions lo;
    lo.db_path = resolve_db(o);
    lo.max_dist_ly = o.max_ly;
    lo.min_parallax_snr = o.min_snr;
    lo.max_ruwe = o.max_ruwe;
    const catalog::LoadResult r = catalog::load_catalog(registry, lo);
    std::printf("loaded %zu stars, %zu names from %s in %.0f ms\n", r.stars, r.names, lo.db_path.string().c_str(),
                r.elapsed_ms);

    // Context variables ("singletons" of this registry). emplace<T>(args...)
    // constructs T in place and returns a reference to it.
    const auto stats = catalog::CatalogStats::compute(registry);
    auto& colors = registry.ctx().emplace<scene::ColorSettings>();
    colors.scheme = o.scheme;
    colors.ranges = scene::make_color_ranges(stats);
    colors.dirty = true;                                    // first frame colours everything
    registry.ctx().emplace<catalog::CatalogStats>(stats);   // domains for legend + filter sliders

    // Filter state: one SchemeFilter per colour scheme (all "show everything"
    // initially), the distance range, and the tagged-only switch, plus the
    // slider domains derived from the catalog statistics. Command-line
    // filters (--filter, including distance=LO:HI, --hide-missing, --classes,
    // --combine) are applied on top; a malformed one aborts start-up.
    auto& filters = registry.ctx().emplace<scene::FilterSettings>(scene::make_filter_settings(stats));
    if (const std::string err = scene::apply_filter_options(filters, o.filters, o.scheme); !err.empty()) {
        throw std::runtime_error(err);
    }

    auto& view = registry.ctx().emplace<scene::ViewSettings>();
    view.show_panel = o.panel;
    view.show_hud = o.hud;
    view.show_guides = o.guides;
    view.exposure = o.exposure;

    auto& cam = registry.ctx().emplace<scene::OrbitCamera>();
    if (o.camera) {
        cam.distance = std::clamp(o.camera->distance_pc, scene::kMinDistancePc, scene::kMaxDistancePc);
        cam.yaw_deg = o.camera->yaw_deg;
        cam.pitch_deg = std::clamp(o.camera->pitch_deg, scene::kMinPitchDeg, scene::kMaxPitchDeg);
    }
    if (o.focus) {
        if (auto p = systems::star_position(registry, *o.focus)) cam.target = *p;
        else std::fprintf(stderr, "warning: --focus '%s' not found, orbiting Sol\n", o.focus->c_str());
    }

    // Milestone 3 ctx state. NameSearch pre-normalises every name once (so a
    // keystroke only scans, never re-normalises); the others are small
    // plain structs described in scene/Selection.h and scene/CameraFlight.h.
    {
        const auto t0 = std::chrono::steady_clock::now();
        const auto& search = registry.ctx().emplace<catalog::NameSearch>(registry.ctx().get<catalog::NameIndex>());
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("search index: %zu names normalised in %.1f ms\n", search.size(), ms);
    }
    registry.ctx().emplace<scene::SelectionState>();
    registry.ctx().emplace<scene::SelectionDetails>();
    registry.ctx().emplace<scene::PointerInput>();
    registry.ctx().emplace<scene::HoverState>();
    registry.ctx().emplace<scene::CameraFlight>();
    registry.ctx().emplace<scene::TagMenuRequest>();

    systems::assign_display_sizes(registry);
    // Tag Sol so it is drawn with a ring. StarIndex maps star_id -> entity; Sol is star_id 1.
    if (auto sol = registry.ctx().get<catalog::StarIndex>().find(1)) registry.emplace<scene::Marked>(*sol);
    return r;
}

render::GraphicsConfig make_graphics_config(const AppOptions& o, bgfx::CallbackI* cb) {
    render::GraphicsConfig cfg;
    if (!render::renderer_from_string(o.renderer, cfg.renderer)) {
        throw std::runtime_error("unknown --renderer '" + o.renderer + "' (auto, opengl, vulkan, d3d11, d3d12, metal)");
    }
    cfg.vsync = o.vsync && !o.screenshot;  // screenshots/benchmarks should not wait for the display
    cfg.callback = cb;
    return cfg;
}

}  // namespace

// The member-initialiser list runs in DECLARATION order (see App.h), which is
// what makes "load catalog, then open window, then init bgfx, then create GPU
// resources" happen in that order.
App::App(AppOptions options)
    : options_(std::move(options)),
      registry_(),
      load_result_(load_scene(registry_, options_)),
      callback_(false),
      window_("starmap", options_.width, options_.height),
      graphics_(window_, make_graphics_config(options_, &callback_)),
      shaders_(render::ShaderLoader::default_roots(platform::executable_dir())),
      star_renderer_(shaders_),
      guide_renderer_(shaders_),
      imgui_(window_.sdl(), shaders_, kViewImGui) {
    std::printf("renderer: %s, back buffer %ux%u px\n", graphics_.renderer_name(), graphics_.width(),
                graphics_.height());
    try {
        details_db_ = std::make_unique<catalog::Database>(resolve_db(options_).string(), catalog::OpenMode::ReadOnly);
    } catch (const std::exception& ex) {
        // Not fatal: picking, search and fly-to still work from the ECS; the
        // details tab shows the error instead of the full record.
        std::fprintf(stderr, "warning: star details unavailable: %s\n", ex.what());
    }
    apply_startup_selection();
}

systems::PickParams App::pick_params() const {
    // The picker mirrors the star shader's size rules, so it takes the same
    // numbers the renderer uses (the rest of PickParams are picking policy).
    systems::PickParams p;
    p.min_px = star_params_.min_px;
    p.max_px = star_params_.max_px;
    p.marker_min_px = star_params_.marker_min_px;
    p.dim_exponent = star_params_.dim_exponent;
    p.min_intensity = star_params_.min_intensity;
    p.max_intensity = star_params_.max_intensity;
    p.exposure = registry_.ctx().get<scene::ViewSettings>().exposure;  // +/- keys change what is visible
    return p;
}

void App::apply_startup_selection() {
    if (options_.search) panel_.search().set_query(*options_.search);
    if (!options_.select) return;
    const auto& search = registry_.ctx().get<catalog::NameSearch>();
    double ms = 0.0;
    const auto hits = search.search(*options_.select, registry_, 1, &ms);
    if (hits.empty()) {
        std::fprintf(stderr, "warning: --select '%s' matches no star\n", options_.select->c_str());
        return;
    }
    const entt::entity e = hits.front().entity;
    const auto& info = registry_.get<ecs::StarInfo>(e);
    std::printf("--select '%s' -> %s (%.2f ly), search %.2f ms\n", options_.select->c_str(), info.display_name.c_str(),
                static_cast<double>(registry_.get<ecs::Position>(e).dist_ly), ms);
    if (options_.camera) {
        // --camera given too: honour its distance and angles, only re-centre.
        systems::select(registry_, e);
        systems::fly_to(registry_, registry_.get<ecs::Position>(e).galactic_pc,
                        registry_.ctx().get<scene::OrbitCamera>().distance, 0.0f);
    } else {
        systems::select_and_fly(registry_, e, 0.0f);  // duration 0 = jump, no animation
    }
}

int App::run() {
    const bool screenshot_mode = options_.screenshot.has_value();
    const double ticks_per_ms = static_cast<double>(SDL_GetPerformanceFrequency()) / 1000.0;
    uint64_t last = SDL_GetPerformanceCounter();
    double smoothed_ms = 0.0, title_timer_ms = 0.0, sum_ms = 0.0, max_ms = 0.0;
    int timed_frames = 0;
    bool screenshot_requested = false;
    int frames_since_request = 0;
    const std::string shot_path = screenshot_mode ? options_.screenshot->string() : std::string();

    auto& view_settings = registry_.ctx().get<scene::ViewSettings>();
    double last_frame_ms = 16.7;       // wall-clock duration of the previous frame (drives the flight)
    double max_click_pick_ms = 0.0;    // statistics printed at exit
    int click_picks = 0;

    for (int frame = 0;; ++frame) {
        // ---- 1. events --------------------------------------------------------
        // Drain ALL pending events every frame. SDL_PollEvent never blocks
        // (SDL_WaitEvent would, which suits editors but not animated scenes).
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) {
                view_settings.quit_requested = true;  // window close button, Cmd+Q, SIGINT
            } else if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                // PIXEL_SIZE_CHANGED (not RESIZED, which is in points) is the
                // event that matters for the back buffer: it also fires when
                // the window moves to a monitor with a different scale factor
                // without changing its size in points.
                graphics_.resize(static_cast<uint32_t>(e.window.data1), static_cast<uint32_t>(e.window.data2));
            } else if (!screenshot_mode) {
                // ImGui first: it must see every event (including ones we
                // act on) to track mouse position, button and key state.
                imgui_.process_event(e);
                input_.handle(e, registry_, window_.pixel_density(),
                              input::UiCapture{imgui_.wants_mouse(), imgui_.wants_keyboard()});
            }
            // In --screenshot mode no events reach ImGui or the camera, so a
            // stray pointer on the virtual display cannot change the picture.
        }
        if (view_settings.quit_requested) break;

        const auto w = static_cast<uint16_t>(graphics_.width());
        const auto h = static_cast<uint16_t>(graphics_.height());

        // --pick X,Y: inject a click as if the user had clicked there. Frame 2,
        // because the pick needs camera matrices from a previous frame (the
        // viewport excludes the panel, whose width is known after frame 0).
        if (options_.pick && frame == 2) {
            auto& pointer = registry_.ctx().get<scene::PointerInput>();
            pointer.click_px = glm::vec2((*options_.pick)[0], (*options_.pick)[1]) * window_.pixel_density();
        }

        // ---- 2. pointer: click -> pick -> select -> fly; hover -> tooltip target ----------------
        {
            auto& pointer = registry_.ctx().get<scene::PointerInput>();
            const bool had_click = pointer.click_px.has_value();
            // A click at an x beyond the 3D viewport lands on the panel, and
            // ImGui would have claimed it (UiCapture) before it got here; the
            // picker additionally ignores stars projected outside the viewport.
            systems::process_pointer(registry_, pick_params());
            if (had_click) {
                const auto& hover = registry_.ctx().get<scene::HoverState>();
                max_click_pick_ms = std::max(max_click_pick_ms, hover.last_click_pick_ms);
                ++click_picks;
                const entt::entity sel = systems::selected_entity(registry_);
                std::printf("pick: %s in %.3f ms\n",
                            sel != entt::null ? registry_.get<ecs::StarInfo>(sel).display_name.c_str() : "(nothing)",
                            hover.last_click_pick_ms);
                std::fflush(stdout);  // stdout to a file/pipe is block-buffered; flush so scripts see it live
            }
        }

        // ---- 3. lazy details for a new selection -----------------------------------------------
        systems::update_details(registry_, details_db_.get());

        // ---- 4. UI (immediate mode) ----------------------------------------------
        imgui_.begin_frame();
        // The panel reports its width in POINTS (ImGui's unit). bgfx views and
        // the camera work in PIXELS; DisplayFramebufferScale is the factor
        // (2 on a Retina/HiDPI display, 1 here).
        const float px_per_pt = ImGui::GetIO().DisplayFramebufferScale.x;
        const float panel_pt = panel_.draw(registry_);
        // After the panel, still inside the ImGui frame, so a tag chosen this
        // frame sets the dirty flags before update_colors / update_filters.
        // Not inside SidePanel::draw: that returns early when the panel is hidden.
        ui::draw_tag_menu(registry_);
        const int panel_px = static_cast<int>(panel_pt * px_per_pt + 0.5f);
        // Keep at least a sliver of 3D view even if the window is tiny.
        const auto scene_w = static_cast<uint16_t>(std::max(64, static_cast<int>(w) - panel_px));
        // A search commit inside the panel may have selected a star: load its
        // details now rather than one frame later (no "loading..." flash).
        systems::update_details(registry_, details_db_.get());

        // ---- 5. camera flight ---------------------------------------------------
        // Seconds, clamped: after a long stall (window drag, breakpoint) one
        // huge step would just end the flight, which is fine, but clamping to
        // 0.1 s keeps even that transition visible.
        const double flight_dt_ms = screenshot_mode ? 1000.0 / 60.0 : std::min(last_frame_ms, 100.0);
        systems::update_flight(registry_, static_cast<float>(flight_dt_ms / 1000.0));

        // ---- 6-7. systems -------------------------------------------------------
        systems::update(registry_, scene_w, h, graphics_.homogeneous_depth());
        systems::update_colors(registry_);
        systems::update_filters(registry_);  // after colours: both react to a scheme change this frame
        const auto& cam = registry_.ctx().get<scene::OrbitCamera>();

        // ---- 8. record draw calls -------------------------------------------------
        // View setup is cheap and done every frame so a resize (or a panel
        // resize) is picked up automatically. Only view 0 clears; everything
        // else draws on top of it.
        bgfx::setViewClear(kViewClear, BGFX_CLEAR_COLOR, 0x02040aff /* RGBA: near-black, faint blue */);
        bgfx::setViewRect(kViewClear, 0, 0, w, h);
        bgfx::touch(kViewClear);
        // The 3D views cover only the area left of the panel. The projection
        // (built from the same scene_w in CameraSystem) has the matching
        // aspect ratio, so Sol stays centred in what you can actually see.
        bgfx::setViewRect(kViewGuides, 0, 0, scene_w, h);
        bgfx::setViewRect(kViewStars, 0, 0, scene_w, h);
        // setViewTransform uploads the camera for a view; the shaders then see
        // it as u_view / u_proj / u_viewProj.
        bgfx::setViewTransform(kViewGuides, &cam.view[0][0], &cam.proj[0][0]);
        bgfx::setViewTransform(kViewStars, &cam.view[0][0], &cam.proj[0][0]);
        // touch() = "submit an empty draw call": guarantees a view (and its
        // clear, for view 0) is processed even if nothing else is submitted
        // to it this frame.
        bgfx::touch(kViewGuides);
        if (view_settings.show_guides) {
            guide_renderer_.submit(kViewGuides);
            // Sol -> selection: a dashed line in the same view as the rings
            // (under the stars). Its distance label is drawn by the HUD.
            const entt::entity sel = systems::selected_entity(registry_);
            if (sel != entt::null && !registry_.get<ecs::StarInfo>(sel).is_sol) {
                const glm::vec3 sol = systems::star_position(registry_, "Sol").value_or(glm::vec3(0.0f));
                guide_renderer_.submit_line(kViewGuides, sol, registry_.get<ecs::Position>(sel).galactic_pc,
                                            render::GuideRenderer::pack_abgr(255, 210, 90, 150));
            }
        }

        star_params_.exposure = view_settings.exposure;
        const uint32_t drawn = star_render_system_.update(registry_, star_renderer_, kViewStars, star_params_);

        systems::HudInfo hud;
        hud.renderer = graphics_.renderer_name();
        hud.visible_stars = drawn;
        hud.total_stars = registry_.ctx().get<scene::FilterSettings>().total_stars;
        hud.frame_ms = smoothed_ms;
        hud.pixels_per_point = px_per_pt;
        hud.pick = pick_params();
        systems::draw_hud(registry_, hud, &guide_renderer_.labels());
        if (view_settings.show_imgui_demo) ImGui::ShowDemoWindow(&view_settings.show_imgui_demo);
        // Ends the ImGui frame (ImGui::Render) and records the UI draw calls
        // into kViewImGui. Must come after every ImGui:: call of the frame.
        imgui_.end_frame();

        if (screenshot_mode && !screenshot_requested && frame >= options_.frames - 1) {
            // The capture happens while bgfx renders this frame; the callback
            // (ScreenshotCallback::screenShot) fires inside a later frame() call.
            bgfx::requestScreenShot(BGFX_INVALID_HANDLE, shot_path.c_str());
            screenshot_requested = true;
        }

        // ---- 9. submit the frame -------------------------------------------------
        // In single-threaded mode this executes all views on the GPU and presents.
        bgfx::frame();

        // ---- timing ------------------------------------------------------------
        const uint64_t now = SDL_GetPerformanceCounter();
        const double ms = static_cast<double>(now - last) / ticks_per_ms;
        last = now;
        last_frame_ms = ms;
        smoothed_ms = smoothed_ms == 0.0 ? ms : smoothed_ms * 0.9 + ms * 0.1;  // exponential moving average
        if (frame >= 3) {  // skip warm-up frames (shader compilation, first uploads)
            sum_ms += ms;
            max_ms = std::max(max_ms, ms);
            ++timed_frames;
        }
        title_timer_ms += ms;
        if (title_timer_ms > 500.0) {
            title_timer_ms = 0.0;
            const auto& colors = registry_.ctx().get<scene::ColorSettings>();
            char title[160];
            std::snprintf(title, sizeof title, "starmap - %s - %u stars - %.0f fps",
                          scene::scheme_info(colors.scheme).title, drawn, 1000.0 / smoothed_ms);
            window_.set_title(title);
        }

        if (screenshot_mode) {
            if (screenshot_requested) ++frames_since_request;
            if (callback_.saved_count() > 0) break;
            if (frames_since_request > 10) {
                std::fprintf(stderr, "screenshot was not delivered by the renderer\n");
                return 1;
            }
        }
    }
    // Filter-pass timing (see systems/FilterSystem.cpp): how long the last
    // re-tagging of all stars took. Printed so it can be measured from scripts.
    const auto& fs = registry_.ctx().get<scene::FilterSettings>();
    std::printf("filter: %zu of %zu stars visible, last filter pass %.3f ms\n", fs.visible, fs.total_stars,
                fs.last_pass_ms);
    // Milestone 3 timings (README "Performance"): picks, searches, detail loads.
    if (click_picks > 0) std::printf("picking: %d click pick(s), worst %.3f ms\n", click_picks, max_click_pick_ms);
    if (panel_.search().searches() > 0)
        std::printf("search: %d searches, last %.2f ms, worst %.2f ms\n", panel_.search().searches(),
                    panel_.search().last_search_ms(), panel_.search().max_search_ms());
    if (const auto& sd = registry_.ctx().get<scene::SelectionDetails>(); sd.details)
        std::printf("details: %s loaded in %.3f ms (%zu planets)\n", sd.details->display_name.c_str(), sd.load_ms,
                    sd.details->planets.size());
    if (timed_frames > 0) {
        std::printf("frames timed: %d, average %.2f ms (%.0f fps), worst %.2f ms\n", timed_frames,
                    sum_ms / timed_frames, 1000.0 * timed_frames / sum_ms, max_ms);
    }
    return 0;
}

}  // namespace starmap::app
