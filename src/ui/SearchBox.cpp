// =============================================================================
// ui/SearchBox.cpp — see SearchBox.h for the design.
// =============================================================================
#include "ui/SearchBox.h"

#include "catalog/Indexes.h"
#include "ecs/Components.h"
#include "scene/Filters.h"
#include "ui/UiUtil.h"

#include <entt/entity/registry.hpp>
#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>

namespace starmap::ui {

namespace {
const char* tier_label(catalog::MatchTier t) {
    switch (t) {
        case catalog::MatchTier::Exact: return "exact";
        case catalog::MatchTier::Prefix: return "prefix";
        case catalog::MatchTier::WordPrefix: return "word";
        case catalog::MatchTier::Substring: return "contains";
    }
    return "";
}
}  // namespace

void SearchBox::set_query(std::string_view text) {
    const std::size_t n = std::min(text.size(), buf_.size() - 1);
    std::memcpy(buf_.data(), text.data(), n);
    buf_[n] = '\0';
    committed_ = false;  // draw() notices buf_ != last_query_ and searches
}

void SearchBox::run_search(const entt::registry& registry) {
    last_query_ = buf_.data();
    highlight_ = 0;
    scroll_to_highlight_ = true;
    hits_.clear();
    const auto* search = registry.ctx().find<catalog::NameSearch>();
    if (!search || last_query_.empty()) return;
    hits_ = search->search(last_query_, registry, 20, &last_ms_);
    max_ms_ = std::max(max_ms_, last_ms_);
    ++searches_;
}

// ImGui calls this from inside InputText when a flag-selected event happens.
// With ImGuiInputTextFlags_CallbackHistory that is Up/Down while the field is
// active (a single-line field otherwise ignores those keys). This is the same
// hook the ImGui demo's console uses for command history. UserData carries
// `this`, since the callback is a plain function pointer (C API).
int SearchBox::input_callback(ImGuiInputTextCallbackData* data) {
    auto* self = static_cast<SearchBox*>(data->UserData);
    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory && !self->hits_.empty()) {
        const int n = static_cast<int>(self->hits_.size());
        if (data->EventKey == ImGuiKey_UpArrow) self->highlight_ = (self->highlight_ + n - 1) % n;
        if (data->EventKey == ImGuiKey_DownArrow) self->highlight_ = (self->highlight_ + 1) % n;
        self->scroll_to_highlight_ = true;
        self->committed_ = false;  // arrow keys re-open a hidden list
    }
    return 0;  // return value is ignored for history events
}

entt::entity SearchBox::draw(entt::registry& registry, bool focus) {
    entt::entity chosen = entt::null;

    // SetKeyboardFocusHere() = "give keyboard focus to the NEXT widget". It
    // must be called right before the InputText, in the frame focus is wanted.
    if (focus) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-FLT_MIN);  // full panel width
    // Flags:
    //   EnterReturnsTrue - the call returns true when Enter is pressed (not on
    //                      every edit); edits are detected by comparing text.
    //   EscapeClearsAll  - Esc empties the field (and a second Esc leaves it).
    //   CallbackHistory  - route Up/Down to input_callback (highlight moves).
    //   AutoSelectAll    - focusing the field selects its text, so Ctrl+F then
    //                      typing replaces the previous query.
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_EscapeClearsAll |
                                      ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_AutoSelectAll;
    const bool enter = ImGui::InputTextWithHint("##search", "Search stars (Ctrl+F or /)", buf_.data(), buf_.size(),
                                                flags, &SearchBox::input_callback, this);

    // Live search: re-run whenever the text differs from what hits_ belong to.
    if (last_query_ != buf_.data()) {
        run_search(registry);
        committed_ = false;
    }

    auto commit = [&](std::size_t i) {
        chosen = hits_[i].entity;
        // Show what was chosen in the field (the display name), so the field
        // doubles as a "current selection" label. last_query_ follows, so
        // this does not trigger a new search next frame. committed_ must be
        // set AFTER set_query(), which clears it (a caught bug: the list
        // stayed open after Enter).
        if (const auto* info = registry.try_get<ecs::StarInfo>(chosen)) set_query(info->display_name);
        last_query_ = buf_.data();
        committed_ = true;
    };
    if (enter && !hits_.empty()) commit(static_cast<std::size_t>(std::clamp(highlight_, 0, static_cast<int>(hits_.size()) - 1)));

    if (committed_ || last_query_.empty()) return chosen;

    if (hits_.empty()) {
        ImGui::TextDisabled("No star matches \"%s\"", last_query_.c_str());
        return chosen;
    }

    // ---- suggestion list (inline child window, see header) -----------------------------------
    const auto* names = registry.ctx().find<catalog::NameIndex>();
    const float row_h = ImGui::GetTextLineHeightWithSpacing();
    const int rows = std::min(static_cast<int>(hits_.size()), 10);
    // BeginChild with a fixed height + border = a scrollable box. The ID
    // ("##suggest") keeps its scroll position between frames.
    if (ImGui::BeginChild("##suggest", ImVec2(0.0f, row_h * static_cast<float>(rows) + ImGui::GetStyle().WindowPadding.y * 2.0f),
                          ImGuiChildFlags_Borders)) {
        for (std::size_t i = 0; i < hits_.size(); ++i) {
            const catalog::SearchHit& h = hits_[i];
            const auto* info = registry.try_get<ecs::StarInfo>(h.entity);
            const char* display = info ? info->display_name.c_str() : "?";
            const catalog::NameEntry* matched =
                names && h.entry < names->entries().size() ? &names->entries()[h.entry] : nullptr;
            ImGui::PushID(static_cast<int>(i));
            const bool is_hl = static_cast<int>(i) == highlight_;
            // Right edge of the row, measured before anything is drawn on it.
            const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
            // Selectable spanning the row; the text is drawn on top of it
            // with SameLine so the row can mix bright and dim parts.
            // AllowOverlap lets later widgets on the same line coexist.
            if (ImGui::Selectable("##row", is_hl, ImGuiSelectableFlags_AllowOverlap)) commit(i);
            if (matched) ImGui::SetItemTooltip("%s match on the %s name", tier_label(h.tier), matched->catalog.c_str());
            if (is_hl && scroll_to_highlight_) ImGui::SetScrollHereY();
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextUnformatted(display);
            if (matched && matched->name != display) {
                ImGui::SameLine();
                ImGui::TextDisabled("= %s", matched->name.c_str());
            }
            if (registry.all_of<scene::FilteredOut>(h.entity)) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.3f, 1.0f), "(filtered)");
            }
            // Right-aligned distance.
            char dist[32];
            std::snprintf(dist, sizeof dist, "%.1f ly", static_cast<double>(h.dist_ly));
            const float w = ImGui::CalcTextSize(dist).x;
            ImGui::SameLine();
            if (ImGui::GetCursorPosX() < right - w) ImGui::SetCursorPosX(right - w);
            ImGui::TextDisabled("%s", dist);
            ImGui::PopID();
        }
        scroll_to_highlight_ = false;
    }
    ImGui::EndChild();  // EndChild is ALWAYS called, like End() (BeginChild's quirk)
    ImGui::TextDisabled("%zu match%s in %.2f ms  |  Up/Down, Enter, Esc", hits_.size(), hits_.size() == 1 ? "" : "es",
                        last_ms_);
    return chosen;
}

}  // namespace starmap::ui
