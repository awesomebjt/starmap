// =============================================================================
// systems/HudSystem.cpp — see HudSystem.h
// =============================================================================
#include "systems/HudSystem.h"

#include "render/GuideRenderer.h"
#include "ecs/Components.h"
#include "scene/ColorScheme.h"
#include "scene/Filters.h"
#include "scene/RenderComponents.h"
#include "scene/Tags.h"
#include "scene/Selection.h"
#include "scene/OrbitCamera.h"
#include "scene/ViewSettings.h"
#include "systems/CameraSystem.h"
#include "systems/SelectionSystem.h"

#include <entt/entity/registry.hpp>
#include <glm/geometric.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace starmap::systems {

namespace {
constexpr float kLyPerPc = 3.261563777f;
constexpr float kMargin = 10.0f;  // points from the viewport edges

std::string with_commas(std::size_t n) {
    std::string s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<std::size_t>(i), ",");
    return s;
}

// Flags for a passive, auto-sized text box: no title bar/resize/scrollbar
// (NoDecoration), sized to its content, not saved to ini, doesn't steal
// focus, and — most importantly — NoInputs: mouse events pass through to the
// 3D view.
// The selection reticle: a circle hugging the star's drawn glow plus four
// short ticks pointing at it, the classic "target lock" look. Drawn with
// ImGui's background draw list (behind all ImGui windows, on top of the 3D
// scene), in POINTS. Returns false if the star is behind the camera.
bool draw_reticle(ImDrawList* dl, const entt::registry& registry, const scene::OrbitCamera& cam, entt::entity e,
                  const PickParams& pick, float ppp) {
    const auto* pos = registry.try_get<ecs::Position>(e);
    if (!pos) return false;
    const glm::vec4 clip = cam.view_proj * glm::vec4(pos->galactic_pc, 1.0f);
    const auto s = world_to_screen(cam, pos->galactic_pc);
    if (!s || clip.w <= 0.0f) return false;
    // Same size rule as the shader and the picker, so the ring sits just
    // outside the visible glow whatever the zoom.
    const auto* size = registry.try_get<scene::DisplaySize>(e);
    const auto* color = registry.try_get<ecs::DisplayColor>(e);
    const float px = size && color ? billboard_radius_px(size->radius_pc, clip.w, focal_length_px(cam), color->rgba.a,
                                                         registry.all_of<scene::Marked>(e), pick)
                                   : pick.min_px;
    const float r = std::max(px * pick.glow_fraction + 6.0f, 11.0f) / ppp;
    const ImVec2 c(s->x / ppp, s->y / ppp);
    const ImU32 col = IM_COL32(255, 210, 90, 235);
    const ImU32 shadow = IM_COL32(0, 0, 0, 160);
    // A dark, wider stroke under the bright one keeps the ring readable over
    // bright stars and over the (blue) guide rings alike.
    dl->AddCircle(c, r, shadow, 0, 3.5f);
    dl->AddCircle(c, r, col, 0, 1.5f);
    const float t0 = r + 3.0f, t1 = r + 9.0f;
    constexpr ImVec2 kDirs[4] = {{1.0f, 0.0f}, {-1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, -1.0f}};
    for (const ImVec2& d : kDirs) {
        const float dx = d.x, dy = d.y;
        dl->AddLine(ImVec2(c.x + dx * t0, c.y + dy * t0), ImVec2(c.x + dx * t1, c.y + dy * t1), col, 2.0f);
    }
    if (const auto* info = registry.try_get<ecs::StarInfo>(e)) {
        const ImVec2 tp(c.x + t1 + 4.0f, c.y - t1 - ImGui::GetTextLineHeight() * 0.5f);
        dl->AddText(ImVec2(tp.x + 1.0f, tp.y + 1.0f), shadow, info->display_name.c_str());
        dl->AddText(tp, col, info->display_name.c_str());
    }
    return true;
}

constexpr ImGuiWindowFlags kOverlayFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                           ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
}  // namespace

void draw_hud(const entt::registry& registry, const HudInfo& info, const std::vector<render::GuideLabel>* labels) {
    // ctx() on a const registry gives const access; get<T>() asserts presence.
    const auto& view_settings = registry.ctx().get<scene::ViewSettings>();
    if (!view_settings.show_hud) return;
    const auto& colors = registry.ctx().get<scene::ColorSettings>();
    const auto& cam = registry.ctx().get<scene::OrbitCamera>();

    // The 3D viewport (left of the panel) in POINTS. The camera stores it in
    // pixels because the projection works in pixels.
    const float ppp = info.pixels_per_point > 0.0f ? info.pixels_per_point : 1.0f;
    const float scene_w = static_cast<float>(cam.viewport_w) / ppp;
    const float scene_h = static_cast<float>(cam.viewport_h) / ppp;
    const float max_text_w = std::max(scene_w - 2.0f * kMargin, 100.0f);

    // ---- top-left: status ---------------------------------------------------------------------
    ImGui::SetNextWindowPos(ImVec2(kMargin, kMargin), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(max_text_w, scene_h));
    ImGui::SetNextWindowBgAlpha(0.45f);  // translucent: the stars stay visible behind
    if (ImGui::Begin("##status", nullptr, kOverlayFlags)) {
        ImGui::Text("starmap  |  %s of %s stars  |  %.1f ms (%.0f fps)  |  %s", with_commas(info.visible_stars).c_str(),
                    with_commas(info.total_stars).c_str(), info.frame_ms, info.frame_ms > 0.0 ? 1000.0 / info.frame_ms : 0.0,
                    info.renderer.c_str());
        ImGui::TextDisabled("camera %.1f ly from (%.1f, %.1f, %.1f) pc, yaw %.0f, pitch %.0f",
                            static_cast<double>(cam.distance * kLyPerPc), static_cast<double>(cam.target.x),
                            static_cast<double>(cam.target.y), static_cast<double>(cam.target.z),
                            static_cast<double>(cam.yaw_deg), static_cast<double>(cam.pitch_deg));
        if (!view_settings.show_panel) {
            // With the panel hidden, keep the essential scheme info on screen.
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f), "[%d] %s", static_cast<int>(colors.scheme) + 1,
                               scene::scheme_info(colors.scheme).title);
            ImGui::PushTextWrapPos(max_text_w);
            ImGui::TextDisabled("%s  (P: filter panel)", scene::scheme_legend(colors.scheme, colors.ranges).c_str());
            ImGui::PopTextWrapPos();
        }
    }
    ImGui::End();  // always paired with Begin, even when Begin returned false

    // ---- bottom-left: controls ----------------------------------------------------------------------
    if (view_settings.show_help) {
        // Pivot (0, 1): the given position is the window's BOTTOM-left corner.
        ImGui::SetNextWindowPos(ImVec2(kMargin, scene_h - kMargin), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(max_text_w, scene_h));
        ImGui::SetNextWindowBgAlpha(0.35f);
        if (ImGui::Begin("##help", nullptr, kOverlayFlags)) {
            ImGui::PushTextWrapPos(max_text_w);
            ImGui::TextDisabled("click select | right-click tag | left-drag rotate | wheel zoom | right/middle-drag pan");
            ImGui::TextDisabled("Ctrl+F or / search | F fly to selection | H/Home Sol | R reset | Esc menu/deselect/quit");
            ImGui::TextDisabled("1-6 scheme | P/Tab panel | G guides | +/- brightness | F1 help");
            ImGui::PopTextWrapPos();
        }
        ImGui::End();
    }

    // ---- 3D labels ----------------------------------------------------------------------------------
    if (labels && view_settings.show_guides) {
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        dl->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(scene_w, scene_h));  // stay inside the 3D viewport
        std::vector<ImVec4> used;  // rectangles already taken (x0, y0, x1, y1)
        const ImU32 col = IM_COL32(140, 170, 230, 230);
        for (const auto& l : *labels) {
            // world_to_screen does proj * view * p, the perspective divide and
            // the viewport mapping (in pixels); anchors behind the camera give nullopt.
            const auto s = world_to_screen(cam, l.world);
            if (!s) continue;
            const ImVec2 p(s->x / ppp + 4.0f, s->y / ppp - ImGui::GetTextLineHeight() * 0.5f);
            const ImVec2 size = ImGui::CalcTextSize(l.text.c_str());
            const ImVec4 r(p.x - 2.0f, p.y - 1.0f, p.x + size.x + 2.0f, p.y + size.y + 1.0f);
            bool overlaps = false;  // skip labels that would collide with an earlier one
            for (const ImVec4& u : used) {
                if (r.x < u.z && r.z > u.x && r.y < u.w && r.w > u.y) overlaps = true;
            }
            if (overlaps) continue;
            used.push_back(r);
            dl->AddText(p, col, l.text.c_str());
        }
        dl->PopClipRect();
    }

    // ---- selection reticle + Sol-line distance label ------------------------------------------------
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(scene_w, scene_h));
    const entt::entity sel = selected_entity(registry);
    if (sel != entt::null && registry.valid(sel)) {
        draw_reticle(bg, registry, cam, sel, info.pick, ppp);
        // The Sol -> selection line itself is 3D geometry (GuideRenderer,
        // view 1); here we only label its midpoint with the distance.
        const auto* pos = registry.try_get<ecs::Position>(sel);
        const auto* star = registry.try_get<ecs::StarInfo>(sel);
        if (pos && star && !star->is_sol && view_settings.show_guides) {
            // Label only when the line is long enough on screen to carry it:
            // with both ends close together the text would sit on top of the
            // reticle's name label (or Sol's ring) and help nobody.
            const auto s0 = world_to_screen(cam, glm::vec3(0.0f));
            const auto s1 = world_to_screen(cam, pos->galactic_pc);
            const bool long_enough = !s0 || !s1 || glm::length(*s1 - *s0) / ppp > 160.0f;
            if (const auto m = world_to_screen(cam, pos->galactic_pc * 0.5f); m && long_enough) {
                char text[48];
                std::snprintf(text, sizeof text, "%.2f ly from Sol", static_cast<double>(pos->dist_ly));
                const ImVec2 p(m->x / ppp + 6.0f, m->y / ppp);
                bg->AddText(ImVec2(p.x + 1.0f, p.y + 1.0f), IM_COL32(0, 0, 0, 170), text);
                bg->AddText(p, IM_COL32(255, 210, 90, 200), text);
            }
        }
    }
    bg->PopClipRect();

    // ---- hover tooltip ------------------------------------------------------------------------------
    // HoverState::entity is refreshed by InteractionSystem (only when the
    // pointer moved) and cleared whenever ImGui owns the mouse.
    if (const auto* hover = registry.ctx().find<scene::HoverState>(); hover && registry.valid(hover->entity)) {
        const auto* star = registry.try_get<ecs::StarInfo>(hover->entity);
        const auto* pos = registry.try_get<ecs::Position>(hover->entity);
        if (star && pos) {
            // BeginTooltip() opens a small window that follows the mouse; like
            // any ImGui window it disappears the first frame we stop drawing it.
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(star->display_name.c_str());
            ImGui::TextDisabled("%.2f ly%s%s", static_cast<double>(pos->dist_ly), star->spectral_type ? "  |  " : "",
                                star->spectral_type ? star->spectral_type->c_str() : "");
            if (const auto* tag = registry.try_get<scene::StarTag>(hover->entity)) {
                const glm::vec3 c = scene::tag_rgb(tag->color);
                ImGui::TextColored(ImVec4(c.r, c.g, c.b, 1.0f), "Tagged %s", scene::tag_color_name(tag->color));
            }
            ImGui::TextDisabled("click to select | right-click to tag");
            ImGui::EndTooltip();
        }
    }
}

}  // namespace starmap::systems
