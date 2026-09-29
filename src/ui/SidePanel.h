#pragma once
// =============================================================================
// ui/SidePanel.h — the right-hand panel: search box + "Filters" / "Star" tabs
// =============================================================================
//
// LAYOUT
//   One fixed, full-height ImGui window anchored to the right edge (as the
//   M2 filter panel was). Its left edge can be dragged to resize it; draw()
//   returns the width in POINTS so App can shrink the 3D viewport and nothing
//   is hidden behind the panel.
//       +-----------------------------+
//       | [ Search stars (Ctrl+F) ]   |  <- always visible (SearchBox)
//       |   suggestions (while typing)|
//       | [Filters] [Star]            |  <- ImGui tab bar
//       |   ... tab contents ...      |
//       +-----------------------------+
//   The search box sits ABOVE the tabs so it is reachable from either tab;
//   committing a search (or clicking a star in the view) switches to the
//   "Star" tab once, via ImGuiTabItemFlags_SetSelected. After that the user
//   is free to switch back to "Filters"; we only force the tab when the
//   selection CHANGES (tracked with SelectionState::version).
//
// ROLE IN THE ARCHITECTURE
//   Owns the three UI pieces (SearchBox, FilterPanel, StarDetailsView) and
//   the panel width. Reads ViewSettings (show_panel, focus_search) and, on a
//   search commit, calls systems::select_and_fly — the same function a mouse
//   pick uses.
// =============================================================================

#include "ui/FilterPanel.h"
#include "ui/SearchBox.h"
#include "ui/StarDetailsView.h"

#include <cstdint>
#include <entt/entity/fwd.hpp>

namespace starmap::ui {

class SidePanel {
public:
    // Returns the panel width in points (0 when hidden).
    float draw(entt::registry& registry);

    SearchBox& search() noexcept { return search_; }

private:
    float width_ = 430.0f;  // points; remembered across frames when the user resizes (M2: 380, M3 needs room for tables)
    SearchBox search_;
    FilterPanel filters_;
    StarDetailsView details_;
    std::uint32_t seen_selection_version_ = 0;  // to switch tabs once per new selection
};

}  // namespace starmap::ui
