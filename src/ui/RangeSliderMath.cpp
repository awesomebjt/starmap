// =============================================================================
// ui/RangeSliderMath.cpp — see RangeSliderMath.h
// =============================================================================
#include "ui/RangeSliderMath.h"

#include <algorithm>
#include <cmath>

namespace starmap::ui {

float SliderScale::to_t(float value) const {
    if (max <= min) return 0.0f;
    float t = 0.0f;
    if (log && min > 0.0f) {
        // Values <= 0 can't be placed on a log axis: pin them to the left end.
        const float v = std::max(value, min);
        t = (std::log10(v) - std::log10(min)) / (std::log10(max) - std::log10(min));
    } else {
        t = (value - min) / (max - min);
    }
    return std::clamp(t, 0.0f, 1.0f);
}

float SliderScale::from_t(float t) const {
    t = std::clamp(t, 0.0f, 1.0f);
    float v = 0.0f;
    if (log && min > 0.0f) {
        // Inverse of to_t: interpolate the exponent, then 10^x.
        v = std::pow(10.0f, std::log10(min) + t * (std::log10(max) - std::log10(min)));
    } else {
        v = min + t * (max - min);
    }
    if (integer) v = std::round(v);
    // Snap exactly onto the ends so "handle at the end" means "untrimmed"
    // (pow/log round-off would otherwise leave 7.9999 instead of 8).
    if (t <= 0.0f) v = min;
    if (t >= 1.0f) v = max;
    return std::clamp(v, min, max);
}

Handle pick_handle(float t_mouse, float t_lo, float t_hi) {
    const float d_lo = std::fabs(t_mouse - t_lo);
    const float d_hi = std::fabs(t_mouse - t_hi);
    if (d_lo < d_hi) return Handle::Low;
    if (d_hi < d_lo) return Handle::High;
    // Equal distance: either the handles overlap, or the click is exactly in
    // the middle. Use the side of the click relative to the low handle.
    if (t_mouse < t_lo) return Handle::Low;
    if (t_mouse > t_hi) return Handle::High;
    return t_hi >= 1.0f ? Handle::Low : Handle::High;  // exactly on a merged pair
}

bool drag_handle(Handle which, float t_mouse, const SliderScale& scale, float& lo, float& hi) {
    const float old_lo = lo, old_hi = hi;
    const float v = scale.from_t(t_mouse);
    if (which == Handle::Low) lo = std::min(v, hi);   // can't pass the high handle
    else if (which == Handle::High) hi = std::max(v, lo);  // can't pass the low handle
    return lo != old_lo || hi != old_hi;
}

void sanitize_range(const SliderScale& scale, bool edited_low, float& lo, float& hi) {
    lo = std::clamp(lo, scale.min, scale.max);
    hi = std::clamp(hi, scale.min, scale.max);
    if (scale.integer) {
        lo = std::round(lo);
        hi = std::round(hi);
    }
    if (lo > hi) {
        // The user typed a min above the max (or vice versa): follow them and
        // push the OTHER value along instead of rejecting the input.
        if (edited_low) hi = lo;
        else lo = hi;
    }
}

}  // namespace starmap::ui
