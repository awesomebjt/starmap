// =============================================================================
// ui/TagMenu.cpp — see TagMenu.h.
// =============================================================================
#include "ui/TagMenu.h"

#include "ecs/Components.h"
#include "scene/Tags.h"
#include "systems/TagSystem.h"

#include <entt/entity/registry.hpp>
#include <imgui.h>

#include <cfloat>

namespace starmap::ui {

void draw_tag_menu(entt::registry& registry) {
    auto* req = registry.ctx().find<scene::TagMenuRequest>();
    if (!req) return;

    // OpenPopup must happen AFTER NewFrame (this function runs there) and
    // BEFORE BeginPopup. SetNextWindowPos on that same frame places the
    // menu on the click. The anchor is in back-buffer pixels; ImGui positions
    // are in points, and DisplayFramebufferScale is the pixels-per-point the
    // SDL backend published (1 on this machine, 2 on a Retina display).
    if (req->open && registry.valid(req->star)) {
        const ImVec2 fb = ImGui::GetIO().DisplayFramebufferScale;
        const float sx = fb.x > 0.0f ? fb.x : 1.0f;
        const float sy = fb.y > 0.0f ? fb.y : 1.0f;
        const ImVec2 pt(req->anchor_px.x / sx, req->anchor_px.y / sy);
        // Near the right or bottom edge, grow the window the other way so
        // the swatches are not clipped off the window. 0.75 leaves room for
        // a 4-column grid without measuring it first.
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const ImVec2 pivot(pt.x > display.x * 0.75f ? 1.0f : 0.0f, pt.y > display.y * 0.75f ? 1.0f : 0.0f);
        ImGui::SetNextWindowPos(pt, ImGuiCond_Always, pivot);
        ImGui::OpenPopup("##tag_star");
        req->open = false;
    } else if (req->open) {
        req->open = false;  // the star went away between the click and the draw
    }

    // BeginPopup returns false when the menu is closed, and EndPopup must
    // then be skipped (same rule as BeginChild / BeginTabBar).
    const bool open = ImGui::BeginPopup("##tag_star");
    req->visible = open;  // last-frame state for Esc in InputHandler
    if (!open) return;
    if (!registry.valid(req->star)) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    const entt::entity star = req->star;
    ImGui::TextUnformatted("Tag star");
    if (const auto* info = registry.try_get<ecs::StarInfo>(star)) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(info->display_name.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::Separator();

    const auto* current = registry.try_get<scene::StarTag>(star);
    // Square swatches a little larger than a text line, four per row: 16
    // colours without a scrolling list. PushID so every "##swatch" is unique.
    const float sz = ImGui::GetFrameHeight() * 1.35f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));
    for (int i = 0; i < scene::kTagColorCount; ++i) {
        if (i % 4 != 0) ImGui::SameLine();
        ImGui::PushID(i);
        const auto index = static_cast<std::uint8_t>(i);
        const glm::vec3 rgb = scene::tag_rgb(index);
        const bool selected = current && current->color == index;
        // The selected swatch gets a white frame. FrameBorderSize is 0 in
        // the default style, so the border colour would otherwise not draw.
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
        }
        if (ImGui::ColorButton("##swatch", ImVec4(rgb.r, rgb.g, rgb.b, 1.0f), ImGuiColorEditFlags_NoTooltip,
                               ImVec2(sz, sz))) {
            // Close the menu: the choice is made. The camera is deliberately
            // not flown and the selection is not changed — tagging should
            // leave the view where it is.
            systems::set_star_tag(registry, star, i);
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", scene::tag_color_name(index));
        if (selected) {
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }
    ImGui::PopStyleVar();

    ImGui::Spacing();
    if (!current) ImGui::BeginDisabled();
    if (ImGui::Button("Remove tag", ImVec2(-FLT_MIN, 0.0f))) {
        systems::clear_star_tag(registry, star);
        ImGui::CloseCurrentPopup();
    }
    if (!current) ImGui::EndDisabled();

    ImGui::EndPopup();
}

}  // namespace starmap::ui
