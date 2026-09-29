#pragma once
// =============================================================================
// ui/SearchBox.h — the star-name search field at the top of the side panel
// =============================================================================
//
// WHAT IT DOES
//   A text field with live suggestions: every keystroke re-runs
//   catalog::NameSearch (a linear scan over ~117k normalised names, ~5-15 ms)
//   and lists up to 20 hits ranked exact > prefix > word-prefix > substring,
//   then primary name, brightness and distance. Up/Down move the highlight,
//   Enter (or a click on a row) commits, Esc clears the text.
//
// WHY THE SUGGESTIONS ARE AN INLINE LIST AND NOT A POPUP
//   The obvious ImGui tool for a dropdown is BeginPopup/BeginCombo. But an
//   ImGui popup takes keyboard focus when it opens and closes when the user
//   clicks elsewhere, so typing into the text field while the popup is open
//   fights the popup for focus (the classic "autocomplete in ImGui" problem;
//   see ocornut/imgui issue #718). An ordinary child window drawn right below
//   the field needs no focus at all: the text field stays active while you
//   type, and the list is just more widgets in the panel. The cost is that it
//   pushes the rest of the panel down instead of overlapping it, which in a
//   side panel is fine (arguably better: nothing is hidden).
//
// ROLE IN THE ARCHITECTURE
//   The box does NOT select anything itself. draw() returns the entity the
//   user committed to, and SidePanel hands it to InteractionSystem's
//   select_and_fly(), the same function a mouse pick uses. One code path for
//   "a star was chosen" keeps click and search behaviour identical.
// =============================================================================

#include "catalog/NameSearch.h"

#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>

#include <array>
#include <string>
#include <string_view>
#include <vector>

struct ImGuiInputTextCallbackData;  // ImGui's callback payload (forward-declared: keeps imgui.h out)

namespace starmap::ui {

class SearchBox {
public:
    // Draws the field (+ suggestions) into the current window. `focus` asks
    // for keyboard focus this frame (Ctrl+F / '/'). Returns the committed
    // entity, or entt::null if nothing was committed this frame.
    entt::entity draw(entt::registry& registry, bool focus);

    // Pre-fills the query (CLI --search) and shows its suggestions.
    void set_query(std::string_view text);

    [[nodiscard]] double last_search_ms() const noexcept { return last_ms_; }
    [[nodiscard]] double max_search_ms() const noexcept { return max_ms_; }
    [[nodiscard]] int searches() const noexcept { return searches_; }

private:
    // ImGui's InputText writes into a caller-owned char buffer (it is a C API
    // at heart). 128 bytes is plenty for a star name or a 19-digit Gaia id.
    std::array<char, 128> buf_{};
    std::string last_query_;               // query the current hits_ belong to
    std::vector<catalog::SearchHit> hits_;
    int highlight_ = 0;                    // row Up/Down/Enter act on
    bool scroll_to_highlight_ = false;
    bool committed_ = false;               // hide the list after a commit until the text changes
    double last_ms_ = 0.0, max_ms_ = 0.0;
    int searches_ = 0;

    void run_search(const entt::registry& registry);
    // Called by ImGui from inside InputText for Up/Down (CallbackHistory).
    static int input_callback(ImGuiInputTextCallbackData* data);
};

}  // namespace starmap::ui
