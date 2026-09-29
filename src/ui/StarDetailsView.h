#pragma once
// =============================================================================
// ui/StarDetailsView.h — the "Star" tab of the side panel: everything we know
// about the selected star
// =============================================================================
//
// DATA SOURCES (and why there are several)
//   * scene::SelectionDetails (ctx)  - the full stars row + planets, loaded
//     LAZILY from SQLite by SelectionSystem::update_details when the
//     selection changes (see catalog/StarDetails.h for why lazy wins).
//   * catalog::NameIndex (ctx)       - all names of the star, already in RAM
//     for search, so they are not queried again.
//   * ecs::Position (component)      - galactic x/y/z for the galactic l/b.
//   * scene::FilterSettings (ctx)    - "is the selection hidden by filters?"
//
// WHY A TAB AND NOT A SEPARATE WINDOW
//   A floating ImGui window would cover part of the 3D view (exactly where
//   the user just clicked, quite often) and would need its own placement
//   logic. The side panel already reserves screen space and shrinks the 3D
//   viewport accordingly, so the details live there, in a second tab next to
//   "Filters". Selecting a star switches to this tab automatically; the
//   sections inside are CollapsingHeaders so long lists (100+ names for a
//   bright star, 6 planets) can be folded away.
//
// Missing values are shown as a dim "n/a" everywhere (ui::text_na).
// =============================================================================

#include <entt/entity/fwd.hpp>

namespace starmap::ui {

class StarDetailsView {
public:
    // Draws into the current window/tab. Buttons in it act directly on the
    // registry through the same system functions the keyboard uses
    // (focus_selection, clear_selection, filter reset).
    void draw(entt::registry& registry);
};

}  // namespace starmap::ui
