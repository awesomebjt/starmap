#pragma once
// =============================================================================
// ui/RangeSlider.h — a custom Dear ImGui widget: one track, two handles
// =============================================================================
//
// ImGui has SliderFloat2 (two separate sliders side by side) but no
// "min..max on one track" widget, so this is a CUSTOM WIDGET — a good
// example of how ImGui widgets are built from a few public building blocks:
//
//   1. InvisibleButton  reserves a rectangle in the layout and gives us
//                       hover/active/click state for it (the "behaviour").
//   2. Our maths        (RangeSliderMath) turns the mouse x into a value.
//   3. ImDrawList       draws whatever we like inside the rectangle (the
//                       "rendering"): track, selected span, markers, grabs.
//   4. GetStateStorage  remembers which handle is being dragged between
//                       frames, keyed by the widget's ID (immediate-mode UIs
//                       keep such per-widget state in a small ID->value map,
//                       because there is no widget object to store it in).
//
// Only the public ImGui API is used (no imgui_internal.h), so the widget
// survives ImGui upgrades.
// =============================================================================

#include "ui/RangeSliderMath.h"

#include <functional>
#include <vector>

namespace starmap::ui {

struct RangeSliderConfig {
    SliderScale scale;                       // domain, log/linear, integer snapping
    const char* format = "%.2f";             // printf format for tooltips / tick labels
    const char* unit = "";                   // appended to the drag tooltip (tick labels stay unit-less to fit)
    std::vector<float> markers;              // small ticks ON the track (e.g. p01 / p99)
    std::vector<float> tick_values;          // labelled ticks BELOW the track (value units)
    // Optional colour of the track at position t (0..1), e.g. the colormap.
    // The selected span is drawn in full colour, the trimmed parts dimmed —
    // so the slider also shows which colours will remain on screen.
    std::function<unsigned int(float t)> track_color;  // returns an ImU32 (packed RGBA)
};

// Draws the widget across the full available width. `id` must be unique
// within the current ImGui ID scope. Returns true while lo/hi changed this frame.
bool RangeSlider(const char* id, float& lo, float& hi, const RangeSliderConfig& config);

}  // namespace starmap::ui
