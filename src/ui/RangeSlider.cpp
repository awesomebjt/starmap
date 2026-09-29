// =============================================================================
// ui/RangeSlider.cpp — see RangeSlider.h for the building blocks.
// =============================================================================
#include "ui/RangeSlider.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace starmap::ui {
namespace {

std::string format_label(const RangeSliderConfig& cfg, float v) {
    char buf[48];
    std::snprintf(buf, sizeof buf, cfg.format, static_cast<double>(v));
    std::string s = buf;
    if (cfg.unit && *cfg.unit) {
        s += ' ';
        s += cfg.unit;
    }
    return s;
}

// Colour with a different alpha (ImU32 is 0xAABBGGRR).
ImU32 with_alpha(ImU32 c, int alpha) { return (c & 0x00FFFFFFu) | (static_cast<ImU32>(alpha) << 24); }

}  // namespace

bool RangeSlider(const char* id, float& lo, float& hi, const RangeSliderConfig& cfg) {
    // PushID scopes every ID created inside ("##range", "grabbed") under the
    // caller's id, so two RangeSliders in one window don't share state.
    ImGui::PushID(id);
    const ImGuiIO& io = ImGui::GetIO();

    // ---- layout ---------------------------------------------------------------------------
    const float width = std::max(ImGui::GetContentRegionAvail().x, 60.0f);
    const float track_h = ImGui::GetFrameHeight();                  // same height as other widgets
    const float label_h = cfg.tick_values.empty() ? 0.0f : ImGui::GetTextLineHeight() + 4.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();                  // top-left, in screen points

    // ---- behaviour: an invisible button covering the widget --------------------------------
    // It participates in layout (advances the cursor) and hit-testing like any
    // widget. IsItemActive() is true from mouse-down until mouse-up, even when
    // the mouse leaves the rectangle — exactly the capture a drag needs.
    ImGui::InvisibleButton("##range", ImVec2(width, track_h + label_h));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    // Grab radius and the usable track: inset by the radius so a handle at
    // either end is fully visible.
    const float grab_r = track_h * 0.36f;
    const float x0 = p0.x + grab_r + 1.0f;
    const float x1 = p0.x + width - grab_r - 1.0f;
    const float cy = p0.y + track_h * 0.5f;
    auto x_of = [&](float t) { return x0 + t * (x1 - x0); };
    const float mouse_t = std::clamp((io.MousePos.x - x0) / (x1 - x0), 0.0f, 1.0f);

    // Which handle is dragged is per-widget state that must survive between
    // frames: store it in ImGui's per-window key/value storage under an ID
    // derived from ours.
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID grabbed_key = ImGui::GetID("grabbed");
    float t_lo = cfg.scale.to_t(lo);
    float t_hi = cfg.scale.to_t(hi);
    if (ImGui::IsItemActivated()) {  // true only on the frame the mouse went down
        storage->SetInt(grabbed_key, static_cast<int>(pick_handle(mouse_t, t_lo, t_hi)));
    }
    if (active && t_lo == t_hi && mouse_t != t_lo) {
        // Both handles sit on the same spot: let the drag DIRECTION decide
        // which one moves (left -> Low, right -> High). Without this a merged
        // pair could only ever be pulled apart one way.
        storage->SetInt(grabbed_key, static_cast<int>(pick_handle(mouse_t, t_lo, t_hi)));
    }
    const Handle grabbed = active ? static_cast<Handle>(storage->GetInt(grabbed_key, 0)) : Handle::None;

    bool changed = false;
    if (grabbed != Handle::None) {
        changed = drag_handle(grabbed, mouse_t, cfg.scale, lo, hi);
        t_lo = cfg.scale.to_t(lo);
        t_hi = cfg.scale.to_t(hi);
    }
    // Hover feedback: highlight the handle a click would grab.
    const Handle hot = grabbed != Handle::None ? grabbed : (hovered ? pick_handle(mouse_t, t_lo, t_hi) : Handle::None);
    if (hovered || active) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    // ---- rendering ------------------------------------------------------------------------------
    // The window draw list collects this window's shapes; they are clipped to
    // the window and drawn in call order (later = on top).
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float ty0 = cy - track_h * 0.20f, ty1 = cy + track_h * 0.20f;  // the bar is thinner than the grabs
    const float xl = x_of(t_lo), xh = x_of(t_hi);

    if (cfg.track_color) {
        // Colormap track: many thin quads, each with a horizontal gradient
        // (AddRectFilledMultiColor interpolates the four corner colours).
        constexpr int kSegments = 64;
        for (int i = 0; i < kSegments; ++i) {
            const float ta = static_cast<float>(i) / kSegments, tb = static_cast<float>(i + 1) / kSegments;
            const ImU32 ca = cfg.track_color(ta), cb = cfg.track_color(tb);
            dl->AddRectFilledMultiColor(ImVec2(x_of(ta), ty0), ImVec2(x_of(tb), ty1), ca, cb, cb, ca);
        }
        // Dim the trimmed parts with translucent black: what stays bright is
        // what stays on screen.
        dl->AddRectFilled(ImVec2(x0, ty0), ImVec2(xl, ty1), IM_COL32(0, 0, 0, 185));
        dl->AddRectFilled(ImVec2(xh, ty0), ImVec2(x1, ty1), IM_COL32(0, 0, 0, 185));
    } else {
        dl->AddRectFilled(ImVec2(x0, ty0), ImVec2(x1, ty1), ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);
        dl->AddRectFilled(ImVec2(xl, ty0), ImVec2(xh, ty1), with_alpha(ImGui::GetColorU32(ImGuiCol_SliderGrab), 150), 3.0f);
    }
    dl->AddRect(ImVec2(x0, ty0), ImVec2(x1, ty1), ImGui::GetColorU32(ImGuiCol_Border), 3.0f);
    // A bright bracket above and below the selected span.
    const ImU32 span_col = ImGui::GetColorU32(ImGuiCol_Text);
    dl->AddLine(ImVec2(xl, ty0 - 2.0f), ImVec2(xh, ty0 - 2.0f), span_col, 1.5f);
    dl->AddLine(ImVec2(xl, ty1 + 2.0f), ImVec2(xh, ty1 + 2.0f), span_col, 1.5f);

    // Markers (e.g. 1st / 99th percentile): short ticks crossing the track.
    for (float m : cfg.markers) {
        const float x = x_of(cfg.scale.to_t(m));
        dl->AddLine(ImVec2(x, ty0 - 4.0f), ImVec2(x, ty1 + 4.0f), IM_COL32(255, 200, 90, 200), 1.5f);
    }

    // Labelled ticks under the track. Labels are clamped inside the widget so
    // the first/last one doesn't spill out of the panel.
    for (float v : cfg.tick_values) {
        const float x = x_of(cfg.scale.to_t(v));
        dl->AddLine(ImVec2(x, ty1 + 3.0f), ImVec2(x, ty1 + 7.0f), ImGui::GetColorU32(ImGuiCol_TextDisabled));
        char text_buf[32];  // tick labels without the unit: they must stay short to fit side by side
        std::snprintf(text_buf, sizeof text_buf, cfg.format, static_cast<double>(v));
        const std::string text = text_buf;
        const float tw = ImGui::CalcTextSize(text.c_str()).x;
        const float tx = std::clamp(x - tw * 0.5f, p0.x, p0.x + width - tw);
        dl->AddText(ImVec2(tx, p0.y + track_h + 2.0f), ImGui::GetColorU32(ImGuiCol_TextDisabled), text.c_str());
    }

    // The two grabs: filled circles with a dark outline so they read on any
    // track colour. Active = accent colour, hovered = slightly larger.
    auto draw_grab = [&](float x, Handle h) {
        const bool is_active = grabbed == h;
        const bool is_hot = hot == h;
        const ImU32 fill = ImGui::GetColorU32(is_active ? ImGuiCol_SliderGrabActive : ImGuiCol_SliderGrab);
        const float r = grab_r * (is_hot ? 1.12f : 1.0f);
        dl->AddCircleFilled(ImVec2(x, cy), r, fill);
        dl->AddCircle(ImVec2(x, cy), r, IM_COL32(20, 20, 30, 255), 0, 1.5f);
    };
    // Draw the grabbed (or hot) handle last so it is on top when both overlap.
    if (hot == Handle::Low) {
        draw_grab(xh, Handle::High);
        draw_grab(xl, Handle::Low);
    } else {
        draw_grab(xl, Handle::Low);
        draw_grab(xh, Handle::High);
    }

    // While dragging, show the exact value next to the cursor.
    if (grabbed != Handle::None) {
        const std::string text = format_label(cfg, grabbed == Handle::Low ? lo : hi);
        ImGui::SetTooltip("%s", text.c_str());
    }
    ImGui::PopID();
    return changed;
}

}  // namespace starmap::ui
