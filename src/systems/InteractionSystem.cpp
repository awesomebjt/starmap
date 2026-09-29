// =============================================================================
// systems/InteractionSystem.cpp — see InteractionSystem.h
// =============================================================================
#include "systems/InteractionSystem.h"

#include "ecs/Components.h"
#include "scene/Selection.h"
#include "scene/Tags.h"
#include "systems/CameraSystem.h"
#include "systems/FlightSystem.h"
#include "systems/SelectionSystem.h"

#include <entt/entity/registry.hpp>

namespace starmap::systems {

namespace {
glm::vec3 sol_position(const entt::registry& registry) {
    // Sol is the origin of the galactic frame by construction; the lookup
    // only matters if a future catalog re-centres the coordinates.
    return star_position(registry, "Sol").value_or(glm::vec3(0.0f));
}

template <typename T>
T& ctx_get_or_emplace(entt::registry& registry) {
    return registry.ctx().contains<T>() ? registry.ctx().get<T>() : registry.ctx().emplace<T>();
}
}  // namespace

void select_and_fly(entt::registry& registry, entt::entity e, float duration_s) {
    if (!registry.valid(e)) return;
    select(registry, e);
    if (const auto* pos = registry.try_get<ecs::Position>(e)) fly_to(registry, pos->galactic_pc, std::nullopt, duration_s);
}

void process_pointer(entt::registry& registry, const PickParams& params) {
    auto& in = ctx_get_or_emplace<scene::PointerInput>(registry);
    auto& hover = ctx_get_or_emplace<scene::HoverState>(registry);
    const auto& cam = registry.ctx().get<scene::OrbitCamera>();

    if (in.click_px) {
        const PickResult r = pick_star(registry, cam, *in.click_px, params);
        hover.last_click_pick_ms = r.elapsed_ms;
        if (r.entity != entt::null) {
            select_and_fly(registry, r.entity);
        } else if (in.double_click) {
            fly_home(registry);
        } else {
            clear_selection(registry);
        }
    }
    // Right click uses the same pick as the left button (last frame's
    // matrices, same hit radii) and then stops. Opening the ImGui menu is
    // the UI's job; we only leave the request. A miss does not clear a
    // request the UI has not consumed yet — the click outside the popup
    // closes it through ImGui, and a miss must not also wipe a request that
    // was set earlier this frame.
    if (in.right_click_px) {
        const PickResult r = pick_star(registry, cam, *in.right_click_px, params);
        if (r.entity != entt::null) {
            auto& menu = ctx_get_or_emplace<scene::TagMenuRequest>(registry);
            menu.open = true;
            menu.star = r.entity;
            menu.anchor_px = *in.right_click_px;
        }
    }
    if (!in.hover_px) {
        hover.entity = entt::null;
    } else if (in.hover_moved) {
        const PickResult r = pick_star(registry, cam, *in.hover_px, params);
        hover.entity = r.entity;
        hover.last_hover_pick_ms = r.elapsed_ms;
    }
    // Consume the one-shot events; hover_px persists (the pointer is still there).
    in.click_px.reset();
    in.double_click = false;
    in.right_click_px.reset();
    in.hover_moved = false;
}

void focus_selection(entt::registry& registry) {
    const entt::entity e = selected_entity(registry);
    if (e != entt::null && registry.valid(e)) {
        if (const auto* pos = registry.try_get<ecs::Position>(e)) return fly_to(registry, pos->galactic_pc);
    }
    fly_to(registry, sol_position(registry));
}

void fly_home(entt::registry& registry) {
    const float d = registry.ctx().get<scene::OrbitCamera>().distance;
    fly_to(registry, sol_position(registry), d);
}

void reset_view(entt::registry& registry) {
    const scene::OrbitCamera home = scene::OrbitCamera::home();
    fly_to_pose(registry, scene::CameraPose{home.target, home.distance, home.yaw_deg, home.pitch_deg});
}

}  // namespace starmap::systems
