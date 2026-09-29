#pragma once
// =============================================================================
// ui/TagMenu.h — the right-click "pick a colour" popup
// =============================================================================
//
// Drawn from App::run inside the ImGui frame, NOT from SidePanel. The panel
// returns immediately when it is hidden (P / --no-panel), and a tag still
// has to be settable then. BeginPopup opens its own window at the cursor,
// so it does not need to live inside the panel window.
//
// The core leaves a scene::TagMenuRequest; this function is the only place
// that calls OpenPopup / BeginPopup. Keeping ImGui out of InteractionSystem
// is what lets the right-click path be unit-tested with no window.
// =============================================================================

#include <entt/entity/fwd.hpp>

namespace starmap::ui {

void draw_tag_menu(entt::registry& registry);

}  // namespace starmap::ui
