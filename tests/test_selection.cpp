// =============================================================================
// tests/test_selection.cpp — milestone 3: picking, selection, camera flights.
//
// Everything here runs on small hand-made registries with no window and no
// GPU: the systems under test are pure ECS + maths by design (see the
// starmap_core comment in CMakeLists.txt). Positions are chosen so that the
// expected answer can be reasoned out, and where a screen position is needed
// it is computed with CameraSystem::world_to_screen — the same projection
// the HUD uses — so the tests do not hard-code pixel coordinates.
// =============================================================================
#include "app/CommandLine.h"
#include "ecs/Components.h"
#include "scene/CameraFlight.h"
#include "scene/ColorScheme.h"
#include "scene/Filters.h"
#include "scene/OrbitCamera.h"
#include "scene/RenderComponents.h"
#include "scene/Selection.h"
#include "scene/Tags.h"
#include "catalog/CatalogStats.h"
#include "systems/CameraSystem.h"
#include "systems/FilterSystem.h"
#include "systems/FlightSystem.h"
#include "systems/InteractionSystem.h"
#include "systems/PickingSystem.h"
#include "systems/SelectionSystem.h"

#include <doctest/doctest.h>
#include <entt/entity/registry.hpp>
#include <glm/geometric.hpp>

#include <array>
#include <cmath>
#include <vector>

using namespace starmap;

namespace {

constexpr int kW = 800, kH = 600;

// doctest's CHECK(a == b) decomposes the expression, and entt::null_t's
// templated operator== makes that ambiguous; a plain helper sidesteps it.
bool is_null(entt::entity e) { return e == entt::null; }

// A registry with a camera looking at the origin from 10 pc and whatever
// stars the test adds. homogeneous_depth = true (OpenGL convention); picking
// does not depend on the depth range, only on x, y and w.
struct Scene {
    entt::registry reg;
    Scene() {
        auto& cam = reg.ctx().emplace<scene::OrbitCamera>();
        cam.distance = 10.0f;
        cam.yaw_deg = 0.0f;
        cam.pitch_deg = 0.0f;
        systems::update(reg, kW, kH, true);
    }
    entt::entity star(glm::vec3 p, float radius_pc = 0.01f, float intensity = 1.0f) {
        const auto e = reg.create();
        reg.emplace<ecs::Position>(e, ecs::Position{p, {}, glm::length(p) * 3.26f});
        reg.emplace<scene::DisplaySize>(e, scene::DisplaySize{radius_pc});
        reg.emplace<ecs::DisplayColor>(e, ecs::DisplayColor{{1.0f, 1.0f, 1.0f, intensity}});
        return e;
    }
    [[nodiscard]] const scene::OrbitCamera& cam() const { return reg.ctx().get<scene::OrbitCamera>(); }
    [[nodiscard]] glm::vec2 screen(entt::entity e) const {
        return *systems::world_to_screen(cam(), reg.get<ecs::Position>(e).galactic_pc);
    }
    [[nodiscard]] systems::PickResult pick(glm::vec2 px) const { return systems::pick_star(reg, cam(), px, {}); }
};

}  // namespace

// ---- billboard size / hit radius ------------------------------------------------------------
TEST_CASE("billboard_radius_px mirrors the shader's clamp rules") {
    const systems::PickParams p;  // min 1.6, max 40, marker 9
    const float f = 500.0f;
    // True size in range: r * f / depth.
    CHECK(systems::billboard_radius_px(0.1f, 5.0f, f, 1.0f, false, p) == doctest::Approx(10.0f));
    // Far away: clamped up to min_px.
    CHECK(systems::billboard_radius_px(0.001f, 100.0f, f, 1.0f, false, p) == doctest::Approx(1.6f));
    // Bright stars get a larger minimum: min * sqrt(intensity).
    CHECK(systems::billboard_radius_px(0.001f, 100.0f, f, 4.0f, false, p) == doctest::Approx(3.2f));
    // Marked (Sol) stars have their own, larger minimum.
    CHECK(systems::billboard_radius_px(0.001f, 100.0f, f, 1.0f, true, p) == doctest::Approx(9.0f));
    // Very close: clamped down to max_px.
    CHECK(systems::billboard_radius_px(10.0f, 0.1f, f, 1.0f, false, p) == doctest::Approx(40.0f));
    // Hit radius: 60 % of the drawn radius (the visible glow), at least 4 px, plus 2 px slack.
    CHECK(systems::hit_radius_px(1.6f, p) == doctest::Approx(6.0f));
    CHECK(systems::hit_radius_px(40.0f, p) == doctest::Approx(26.0f));
}

TEST_CASE("visibility mirrors the shader's brightness; faint stars need a direct hit") {
    const systems::PickParams p;
    CHECK(systems::visibility(10.0f, 10.0f, 1.0f, p) == doctest::Approx(1.0f));   // drawn at true size
    CHECK(systems::visibility(0.01f, 1.6f, 1.0f, p) == doctest::Approx(0.1f));    // tiny: floored at min_intensity
    CHECK(systems::visibility(10.0f, 10.0f, 4.0f, p) == doctest::Approx(2.0f));   // intensity capped at max_intensity
    CHECK(systems::visibility(10.0f, 10.0f, 0.08f, p) == doctest::Approx(0.08f)); // "no planets" grey in scheme 6
    CHECK(systems::hit_radius_px(1.6f, 0.1f, p) == doctest::Approx(p.faint_hit_px));
    CHECK(systems::hit_radius_px(1.6f, 1.0f, p) == doctest::Approx(6.0f));
}

TEST_CASE("pick_star: a faint star only counts on a direct hit, and a direct hit beats a brushed glow") {
    Scene s;
    const auto& v = s.cam().view;
    const glm::vec3 right(v[0][0], v[1][0], v[2][0]);
    const auto faint = s.star({0.0f, 0.0f, 0.0f}, 0.0005f);           // ~0.03 px true size: 10 % brightness
    const auto bright = s.star(right * 0.15f, 0.2f);                  // ~10 px away, drawn ~13 px
    const glm::vec2 pf = s.screen(faint);
    CHECK(s.pick(pf + glm::vec2(0.0f, -4.0f)).entity != faint);       // 4 px off a faint dot: not it
    CHECK(s.pick(pf).entity == faint);                                // dead centre: it
    CHECK(s.pick(s.screen(bright)).entity == bright);
    CHECK(s.pick(pf + glm::vec2(4.0f, -3.0f)).entity == bright);      // in the bright glow, 5 px off the dot
}

TEST_CASE("focal_length_px matches the projection") {
    Scene s;
    const float f = systems::focal_length_px(s.cam());
    // A point 1 pc to the side at depth d appears f/d pixels off centre.
    // With yaw = pitch = 0 the camera sits on one axis; find a sideways
    // direction from the view matrix (its first row = camera right vector).
    const auto& v = s.cam().view;
    const glm::vec3 right(v[0][0], v[1][0], v[2][0]);
    const auto e = s.star(right * 1.0f);
    const glm::vec2 c(kW * 0.5f, kH * 0.5f);
    CHECK(glm::length(s.screen(e) - c) == doctest::Approx(f / 10.0f).epsilon(0.01));
}

// ---- pick_star -----------------------------------------------------------------------------------
TEST_CASE("pick_star: nearest visible star within its hit radius") {
    Scene s;
    const auto a = s.star({0.0f, 0.0f, 0.0f});
    const auto& v = s.cam().view;
    const glm::vec3 right(v[0][0], v[1][0], v[2][0]);
    const auto b = s.star(right * 0.5f);  // ~32 px to the right of a
    const glm::vec2 pa = s.screen(a), pb = s.screen(b);
    REQUIRE(glm::length(pb - pa) > 20.0f);

    CHECK(s.pick(pa).entity == a);
    CHECK(s.pick(pa + glm::vec2(3.0f, 2.0f)).entity == a);   // within 6 px
    CHECK(s.pick(pb - glm::vec2(2.0f, 0.0f)).entity == b);
    CHECK(is_null(s.pick(pa + glm::vec2(0.0f, 30.0f)).entity));  // empty space

    const auto r = s.pick(pa + glm::vec2(4.0f, 0.0f));
    CHECK(r.screen_dist_px == doctest::Approx(4.0f).epsilon(0.05));
    CHECK(r.tested == 2);

    SUBCASE("FilteredOut stars are not pickable") {
        s.reg.emplace<scene::FilteredOut>(a);
        CHECK(is_null(s.pick(pa).entity));
        CHECK(s.pick(pb).entity == b);
    }
    SUBCASE("a click outside the viewport picks nothing") {
        CHECK(is_null(s.pick({-1.0f, pa.y}).entity));
        CHECK(is_null(s.pick({static_cast<float>(kW) + 5.0f, pa.y}).entity));
    }
}

TEST_CASE("pick_star: bigger billboards are easier to hit") {
    Scene s;
    const auto& v = s.cam().view;
    const glm::vec3 right(v[0][0], v[1][0], v[2][0]);
    const auto small = s.star({0.0f, 0.0f, 0.0f}, 0.001f);
    const auto big = s.star(right * 1.0f, 0.3f);  // drawn ~15 px radius at 10 pc
    const glm::vec2 off(0.0f, 12.0f);
    CHECK(is_null(s.pick(s.screen(small) + off).entity));  // 12 px from a 1.6 px dot: miss
    CHECK(s.pick(s.screen(big) + off).entity == big);           // 12 px from a 30 px disc: hit
}

TEST_CASE("pick_star: stars behind the camera are ignored") {
    Scene s;
    const auto& cam = s.cam();
    // Directly behind the eye on the view axis: a naive projection without
    // the w > 0 test would put it exactly at the screen centre.
    const glm::vec3 behind = cam.eye + (cam.eye - cam.target);
    s.star(behind, 5.0f);
    CHECK(is_null(s.pick({kW * 0.5f, kH * 0.5f}).entity));
}

// ---- selection model -------------------------------------------------------------------------------
TEST_CASE("select / clear keep exactly one Selected tag and bump the version") {
    Scene s;
    s.reg.ctx().emplace<scene::SelectionState>();
    const auto a = s.star({0, 0, 0}), b = s.star({1, 0, 0});
    CHECK(is_null(systems::selected_entity(s.reg)));
    systems::select(s.reg, a);
    CHECK(systems::selected_entity(s.reg) == a);
    CHECK(s.reg.all_of<ecs::Selected>(a));
    const auto v1 = s.reg.ctx().get<scene::SelectionState>().version;
    systems::select(s.reg, b);
    CHECK(s.reg.storage<ecs::Selected>().size() == 1);
    CHECK(s.reg.all_of<ecs::Selected>(b));
    CHECK_FALSE(s.reg.all_of<ecs::Selected>(a));
    CHECK(s.reg.ctx().get<scene::SelectionState>().version > v1);
    systems::clear_selection(s.reg);
    CHECK(s.reg.storage<ecs::Selected>().size() == 0);
    CHECK(is_null(systems::selected_entity(s.reg)));
    // Destroying the selected entity must not leave a dangling selection.
    systems::select(s.reg, a);
    s.reg.destroy(a);
    CHECK(is_null(systems::selected_entity(s.reg)));
}

TEST_CASE("process_pointer: click selects, empty click clears, drag state is not its business") {
    Scene s;
    const auto a = s.star({0, 0, 0});
    auto& in = s.reg.ctx().emplace<scene::PointerInput>();
    in.click_px = s.screen(a);
    systems::process_pointer(s.reg, {});
    CHECK(systems::selected_entity(s.reg) == a);
    CHECK_FALSE(in.click_px.has_value());  // consumed
    CHECK(systems::flight_active(s.reg));  // and a fly-to started
    in.click_px = s.screen(a) + glm::vec2(0.0f, 100.0f);
    systems::process_pointer(s.reg, {});
    CHECK(is_null(systems::selected_entity(s.reg)));
    // Hover: only re-picked when the pointer moved.
    in.hover_px = s.screen(a);
    in.hover_moved = true;
    systems::process_pointer(s.reg, {});
    CHECK(s.reg.ctx().get<scene::HoverState>().entity == a);
    in.hover_px.reset();
    systems::process_pointer(s.reg, {});
    CHECK(is_null(s.reg.ctx().get<scene::HoverState>().entity));
}

TEST_CASE("process_pointer: right-click asks for the tag menu and does not select") {
    Scene s;
    const auto a = s.star({0, 0, 0});
    auto& in = s.reg.ctx().emplace<scene::PointerInput>();
    const glm::vec2 at = s.screen(a);
    in.right_click_px = at;
    systems::process_pointer(s.reg, {});
    CHECK(is_null(systems::selected_entity(s.reg)));
    CHECK_FALSE(systems::flight_active(s.reg));
    CHECK_FALSE(in.right_click_px.has_value());  // consumed
    auto& menu = s.reg.ctx().get<scene::TagMenuRequest>();
    CHECK(menu.open);
    CHECK(menu.star == a);
    CHECK(menu.anchor_px.x == doctest::Approx(at.x));
    CHECK(menu.anchor_px.y == doctest::Approx(at.y));

    // A miss does not open a new request and does not clear one still pending.
    menu.open = false;
    in.right_click_px = at + glm::vec2(0.0f, 100.0f);
    systems::process_pointer(s.reg, {});
    CHECK_FALSE(menu.open);
    CHECK(menu.star == a);
    CHECK(is_null(systems::selected_entity(s.reg)));
}

// ---- easing / interpolation / flights -----------------------------------------------------------------
TEST_CASE("ease_in_out_cubic: endpoints, symmetry, monotonic, clamped") {
    using systems::ease_in_out_cubic;
    CHECK(ease_in_out_cubic(0.0f) == 0.0f);
    CHECK(ease_in_out_cubic(1.0f) == 1.0f);
    CHECK(ease_in_out_cubic(0.5f) == doctest::Approx(0.5f));
    CHECK(ease_in_out_cubic(-1.0f) == 0.0f);
    CHECK(ease_in_out_cubic(2.0f) == 1.0f);
    float prev = 0.0f;
    for (int i = 1; i <= 100; ++i) {
        const float t = static_cast<float>(i) / 100.0f;
        const float e = ease_in_out_cubic(t);
        CHECK(e >= prev);
        CHECK(e + ease_in_out_cubic(1.0f - t) == doctest::Approx(1.0f));  // point symmetry about (0.5, 0.5)
        prev = e;
    }
    // Slow start: the first 10 % of the time covers only 0.4 % of the way.
    CHECK(ease_in_out_cubic(0.1f) == doctest::Approx(0.004f));
}

TEST_CASE("interpolate: linear target, geometric distance, short-arc yaw") {
    const scene::CameraPose a{{0, 0, 0}, 1.0f, 350.0f, 10.0f};
    const scene::CameraPose b{{10, 0, 0}, 100.0f, 10.0f, 30.0f};
    const auto m = systems::interpolate(a, b, 0.5f, true);
    CHECK(m.target.x == doctest::Approx(5.0f));
    CHECK(m.distance == doctest::Approx(10.0f));  // sqrt(1 * 100): halfway in log space
    CHECK(std::remainder(m.yaw_deg, 360.0f) == doctest::Approx(0.0f).epsilon(1e-4));  // 350 -> 10 through 0, not 180
    CHECK(m.pitch_deg == doctest::Approx(20.0f));
    const auto e = systems::interpolate(a, b, 1.0f, true);
    CHECK(e.distance == doctest::Approx(100.0f));
    CHECK(std::remainder(e.yaw_deg - 10.0f, 360.0f) == doctest::Approx(0.0f).epsilon(1e-4));
    // Without angle animation the angles are left for the caller.
    CHECK(systems::interpolate(a, b, 0.5f, false).yaw_deg == 350.0f);
}

TEST_CASE("flights are frame-rate independent and land exactly") {
    auto run = [](float dt, float until_s) {
        Scene s;
        systems::fly_to(s.reg, {20.0f, 0.0f, 0.0f}, 5.0f, 0.8f);
        float t = 0.0f;
        while (t + dt <= until_s + 1e-6f) {
            systems::update_flight(s.reg, dt);
            t += dt;
        }
        return systems::pose_of(s.cam());
    };
    // Same elapsed time (0.4 s) reached in 4, 8 and 24 steps: same pose.
    const auto p30 = run(0.1f, 0.4f), p60 = run(0.05f, 0.4f), p144 = run(0.4f / 24.0f, 0.4f);
    CHECK(p30.target.x == doctest::Approx(p60.target.x).epsilon(1e-4));
    CHECK(p30.target.x == doctest::Approx(p144.target.x).epsilon(1e-3));
    CHECK(p30.distance == doctest::Approx(p144.distance).epsilon(1e-3));
    CHECK(p30.target.x == doctest::Approx(10.0f).epsilon(1e-3));  // t = 0.5 -> ease 0.5 -> halfway
    // After the duration the camera is exactly at the target and the flight is over.
    Scene s;
    systems::fly_to(s.reg, {20.0f, 0.0f, 0.0f}, 5.0f, 0.8f);
    for (int i = 0; i < 100; ++i) systems::update_flight(s.reg, 1.0f / 60.0f);
    CHECK_FALSE(systems::flight_active(s.reg));
    CHECK(s.cam().target.x == 20.0f);
    CHECK(s.cam().distance == 5.0f);
    // A negative dt (clock glitch) never runs a flight backwards.
    systems::fly_to(s.reg, {0.0f, 0.0f, 0.0f}, 5.0f, 0.8f);
    systems::update_flight(s.reg, -1.0f);
    CHECK(s.cam().target.x == doctest::Approx(20.0f));
}

TEST_CASE("retargeting mid-flight starts from the current pose (no jump); cancel stops in place") {
    Scene s;
    systems::fly_to(s.reg, {20.0f, 0.0f, 0.0f}, 5.0f, 0.8f);
    for (int i = 0; i < 20; ++i) systems::update_flight(s.reg, 0.02f);  // 0.4 s: halfway
    const auto mid = systems::pose_of(s.cam());
    systems::fly_to(s.reg, {0.0f, 20.0f, 0.0f}, 5.0f, 0.8f);
    systems::update_flight(s.reg, 0.0f);  // zero time later...
    const auto after = systems::pose_of(s.cam());
    CHECK(glm::length(after.target - mid.target) < 1e-4f);  // ...the camera has not moved
    CHECK(after.distance == doctest::Approx(mid.distance));

    systems::update_flight(s.reg, 0.1f);
    systems::cancel_flight(s.reg);
    const auto stopped = systems::pose_of(s.cam());
    CHECK_FALSE(systems::update_flight(s.reg, 0.1f));
    CHECK(glm::length(systems::pose_of(s.cam()).target - stopped.target) == 0.0f);
}

TEST_CASE("an orbit drag cancels an angle flight (R) but not a target-only flight (select)") {
    Scene s;
    systems::fly_to(s.reg, {5.0f, 0.0f, 0.0f});
    systems::cancel_angle_flight(s.reg);
    CHECK(systems::flight_active(s.reg));  // target-only: orbiting during it is fine
    systems::reset_view(s.reg);
    systems::cancel_angle_flight(s.reg);
    CHECK_FALSE(systems::flight_active(s.reg));
}

TEST_CASE("framing_distance zooms in to frame a star but never zooms out") {
    CHECK(systems::framing_distance(150.0f) == scene::kFocusDistancePc);
    CHECK(systems::framing_distance(2.0f) == 2.0f);
}

// ---- filters exempt the selection ------------------------------------------------------------------------
TEST_CASE("FilterSystem never hides the selected star, and reports that it would") {
    entt::registry reg;
    std::vector<entt::entity> stars;
    for (int i = 0; i < 3; ++i) {
        const auto e = reg.create();
        reg.emplace<ecs::StarInfo>(e, ecs::StarInfo{"s" + std::to_string(i), std::nullopt, std::nullopt, 'G', i == 0});
        ecs::Physical ph{};
        ph.teff_k = 3000.0f + 1000.0f * static_cast<float>(i);  // 3000, 4000, 5000 K
        reg.emplace<ecs::Physical>(e, ph);
        stars.push_back(e);
    }
    const auto stats = catalog::CatalogStats::compute(reg);
    auto& fs = reg.ctx().emplace<scene::FilterSettings>(scene::make_filter_settings(stats));
    reg.ctx().emplace<scene::ColorSettings>();  // temperature scheme
    reg.ctx().emplace<scene::SelectionState>();
    auto& f = fs.filter(scene::ColorScheme::Temperature);
    f.lo = 4500.0f;  // only the 5000 K star (and Sol = stars[0]) survive
    fs.dirty = true;
    systems::update_filters(reg);
    CHECK(reg.all_of<scene::FilteredOut>(stars[1]));

    systems::select(reg, stars[1]);
    CHECK(systems::update_filters(reg));  // the selection changed: re-run
    CHECK_FALSE(reg.all_of<scene::FilteredOut>(stars[1]));
    CHECK(fs.selection_filtered);
    systems::clear_selection(reg);
    systems::update_filters(reg);
    CHECK(reg.all_of<scene::FilteredOut>(stars[1]));
    CHECK_FALSE(fs.selection_filtered);
}

// ---- command line ----------------------------------------------------------------------------------------
TEST_CASE("CLI: --select, --pick, --search") {
    const char* argv[] = {"starmap", "--select", "eps Eri", "--pick", "412.5,300", "--search", "Ran"};
    const auto r = app::parse_command_line(7, argv);
    REQUIRE(r.error.empty());
    REQUIRE(r.options.select.has_value());
    CHECK(*r.options.select == "eps Eri");
    REQUIRE(r.options.pick.has_value());
    CHECK((*r.options.pick)[0] == 412.5f);
    CHECK((*r.options.pick)[1] == 300.0f);
    CHECK(*r.options.search == "Ran");
    const char* bad[] = {"starmap", "--pick", "12"};
    CHECK_FALSE(app::parse_command_line(3, bad).error.empty());
}
