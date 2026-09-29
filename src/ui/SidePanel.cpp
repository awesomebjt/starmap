// =============================================================================
// ui/SidePanel.cpp — see SidePanel.h for the layout.
// =============================================================================
#include "ui/SidePanel.h"

#include "scene/Selection.h"
#include "scene/ViewSettings.h"
#include "systems/InteractionSystem.h"
#include "systems/SelectionSystem.h"

#include <entt/entity/registry.hpp>
#include <imgui.h>

#include <algorithm>
#include <cfloat>

namespace starmap::ui {

namespace {
constexpr float kMinPanelWidth = 300.0f;  // points: below this the slider labels collide
}

float SidePanel::draw(entt::registry& registry) {
    auto& view = registry.ctx().get<scene::ViewSettings>();
    if (!view.show_panel) return 0.0f;

    // ---- window placement ---------------------------------------------------------------------
    // The main viewport's work area = the whole SDL window (in points).
    // Position and height are forced every frame (ImGuiCond_Always) to keep
    // the panel glued to the right edge, also after a window resize. The
    // width comes from width_, which we read back after Begin(): if the user
    // dragged the left border, ImGui changed the window's size during Begin()
    // and we adopt it for the next frame.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float max_w = std::max(kMinPanelWidth, vp->WorkSize.x * 0.6f);
    width_ = std::clamp(width_, kMinPanelWidth, max_w);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - width_, vp->WorkPos.y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width_, vp->WorkSize.y), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(kMinPanelWidth, 0.0f), ImVec2(max_w, FLT_MAX));
    // NoTitleBar: the tabs are the title. NoFocusOnAppearing: showing the
    // panel (P) must not steal keyboard focus from the 3D view.
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
    // Begin() returns false when the window is collapsed/clipped, but End()
    // must be called regardless — a classic ImGui pitfall (unlike BeginChild/
    // BeginCombo/BeginTabBar, whose End* is only called when Begin* returned true).
    if (ImGui::Begin("Side panel", nullptr, flags)) {
        width_ = ImGui::GetWindowWidth();

        // ---- search -------------------------------------------------------------------------------
        const bool focus = view.focus_search;
        view.focus_search = false;  // one-shot request, consumed here
        if (const entt::entity hit = search_.draw(registry, focus); hit != entt::null) systems::select_and_fly(registry, hit);
        ImGui::Spacing();

        // ---- tabs -----------------------------------------------------------------------------------
        const auto* sel = registry.ctx().find<scene::SelectionState>();
        const std::uint32_t version = sel ? sel->version : 0u;
        const bool has_selection = systems::selected_entity(registry) != entt::null;
        // Force the Star tab once when a NEW selection appears. SetSelected
        // is a per-frame flag: pass it for one frame and ImGui switches.
        const bool force_star = has_selection && version != seen_selection_version_;
        seen_selection_version_ = version;

        if (ImGui::BeginTabBar("##tabs")) {
            if (ImGui::BeginTabItem("Filters")) {
                // BeginChild gives each tab its own scroll region, so a long
                // details page does not scroll the search box out of sight.
                if (ImGui::BeginChild("##filters_scroll")) filters_.draw_contents(registry);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Star", nullptr, force_star ? ImGuiTabItemFlags_SetSelected : 0)) {
                if (ImGui::BeginChild("##star_scroll")) details_.draw(registry);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    return width_;
}

}  // namespace starmap::ui
