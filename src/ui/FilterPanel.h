#pragma once
// =============================================================================
// ui/FilterPanel.h — the "Filters" tab of the side panel: scheme, legend, filters
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   Called once per frame between ImGuiLayer::begin_frame() and end_frame().
//   It READS and WRITES plain data in registry.ctx():
//     ColorSettings   scheme selector (same effect as keys 1-6)
//     FilterSettings  hide-missing toggle, range, classes, combine; every edit
//                     sets FilterSettings::dirty so FilterSystem re-runs
//   and never touches entities or the GPU. That separation is the ECS way to
//   do UI: widgets edit state, systems derive data from state.
//
// LAYOUT
//   Milestone 2 made this its own right-anchored ImGui window. Milestone 3
//   moved the window into ui/SidePanel (which adds the search box and the
//   "Star" details tab) and this class now only draws the CONTENTS of the
//   "Filters" tab into whatever window/tab is current.
// =============================================================================

#include <entt/entity/fwd.hpp>

namespace starmap::ui {

class FilterPanel {
public:
    // Draws the widgets into the current ImGui window (no Begin/End here).
    void draw_contents(entt::registry& registry);
};

}  // namespace starmap::ui
