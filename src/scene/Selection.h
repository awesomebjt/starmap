#pragma once
// =============================================================================
// scene/Selection.h — "which star is selected / hovered / was clicked"
// =============================================================================
//
// TAG vs CONTEXT VARIABLE — we use BOTH, on purpose:
//   * ecs::Selected (an empty tag component, Components.h) sits on exactly one
//     entity. Tags are the ECS way to let SYSTEMS ask "is this entity
//     selected?" while iterating, or to iterate "all selected entities" with a
//     view — e.g. registry.view<Position>(entt::exclude<Selected>) — and they
//     scale naturally to multi-selection later.
//   * SelectionState (a ctx variable, below) stores the same entity so that
//     code which just wants "THE selected star" gets it in O(1) without
//     scanning a view, and carries a `version` counter that consumers compare
//     against to notice a change (the details loader, the filter pass).
//   The price of redundancy is keeping the two in sync. That is why ONLY the
//   functions in systems/SelectionSystem.h may change the selection: they
//   update the tag and the ctx together, bump the version, and nobody else
//   ever emplaces/erases Selected.
// =============================================================================

#include "catalog/StarDetails.h"

#include <entt/entity/entity.hpp>
#include <glm/vec2.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace starmap::scene {

struct SelectionState {
    entt::entity entity = entt::null;  // mirror of the single entity tagged ecs::Selected
    std::uint32_t version = 0;         // incremented on every select/clear
};

// Lazily loaded data for the details panel (see catalog/StarDetails.h for why
// it is not stored in components). Refreshed by SelectionSystem when
// SelectionState::version moves on.
struct SelectionDetails {
    std::uint32_t version = ~0u;               // SelectionState::version this was loaded for
    entt::entity entity = entt::null;
    std::optional<catalog::StarDetails> details;
    std::string error;                         // set if the query failed (e.g. db moved away)
    double load_ms = 0.0;
};

// Raw pointer input for the 3D view, produced by InputHandler (which knows
// about SDL events) and consumed by InteractionSystem (which knows about
// stars and cameras). All positions in BACK-BUFFER PIXELS relative to the
// window's top-left corner — the same space as OrbitCamera's viewport.
struct PointerInput {
    std::optional<glm::vec2> click_px;        // left click (press+release without dragging) this frame
    bool double_click = false;                // ... and it was the second click of a double-click
    std::optional<glm::vec2> right_click_px;  // right click, same gesture; opens the tag menu, does not select
    std::optional<glm::vec2> hover_px;        // pointer over the 3D view with no button held; nullopt otherwise
    bool hover_moved = false;                 // hover_px changed since the last frame (re-pick needed)
};

// Result of the hover pick (for the tooltip) and pick timings (for the
// README / panel footer: "how long does a brute-force pick take?").
struct HoverState {
    entt::entity entity = entt::null;
    double last_click_pick_ms = 0.0;
    double last_hover_pick_ms = 0.0;
};

}  // namespace starmap::scene
