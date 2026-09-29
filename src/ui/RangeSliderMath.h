#pragma once
// =============================================================================
// ui/RangeSliderMath.h — the maths behind the two-handle range slider
// =============================================================================
//
// Kept separate from the ImGui widget (ui/RangeSlider.cpp) so it can be
// unit-tested without a GUI: value <-> position mapping (linear or log),
// which handle a click grabs, and moving a handle without letting the two
// cross. The widget is then only "read the mouse, call these, draw".
//
// POSITIONS: t in [0, 1] along the track (0 = left end = domain min).
//   linear:  t = (v - min) / (max - min)
//   log:     t = (log10 v - log10 min) / (log10 max - log10 min)
// A log mapping gives each DECADE (0.01..0.1, 0.1..1, 1..10 R_sun) the same
// slider length, matching how radii are distributed and how the Diameter
// colormap is built. It requires min > 0.
// =============================================================================

namespace starmap::ui {

struct SliderScale {
    float min = 0.0f;
    float max = 1.0f;
    bool log = false;      // logarithmic mapping (min must be > 0)
    bool integer = false;  // snap values to whole numbers (e.g. planet counts)

    [[nodiscard]] float to_t(float value) const;    // value -> 0..1 (clamped)
    [[nodiscard]] float from_t(float t) const;      // 0..1 -> value (snapped if integer)
};

enum class Handle { None, Low, High };

// Which handle a click at position t_mouse should grab.
//   * the nearer handle wins;
//   * if both handles sit on the same spot (lo == hi), the side of the click
//     decides (left of it -> Low, right -> High), otherwise the user could
//     never separate them again;
//   * a click exactly ON the merged pair picks High, except at the top end of
//     the track (t = 1) where High could not move at all, so Low is picked.
//     The widget additionally re-picks by drag direction while the handles
//     coincide (see RangeSlider.cpp).
[[nodiscard]] Handle pick_handle(float t_mouse, float t_lo, float t_hi);

// Moves `which` handle to t_mouse (converted to a value on `scale`), clamped
// so that lo <= hi always holds ("handles can't cross": the dragged handle
// stops at the other one). Returns true if lo or hi changed.
bool drag_handle(Handle which, float t_mouse, const SliderScale& scale, float& lo, float& hi);

// After typing numbers into the min/max fields: clamp both into the domain
// and restore lo <= hi by moving the value that was NOT just edited
// (edited_low says which one the user typed into).
void sanitize_range(const SliderScale& scale, bool edited_low, float& lo, float& hi);

}  // namespace starmap::ui
