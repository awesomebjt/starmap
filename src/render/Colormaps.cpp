// =============================================================================
// render/Colormaps.cpp — implementation of the colour ramps (see Colormaps.h).
// =============================================================================
#include "render/Colormaps.h"

#include <glm/common.hpp>  // glm::clamp, glm::mix

#include <algorithm>
#include <array>
#include <cmath>

namespace starmap::render {

glm::vec3 rgb_from_hex(std::uint32_t rrggbb) {
    return glm::vec3(static_cast<float>((rrggbb >> 16) & 0xffu),
                     static_cast<float>((rrggbb >> 8) & 0xffu),
                     static_cast<float>(rrggbb & 0xffu)) / 255.0f;
}

glm::vec3 blackbody_rgb(float kelvin) {
    // Helland's fit works in units of 100 K. Each channel is a piecewise
    // function: below ~6600 K red is saturated and green/blue rise; above it
    // blue is saturated and red/green fall off as a power law.
    const double t = std::clamp(static_cast<double>(kelvin), 1000.0, 40000.0) / 100.0;
    double r, g, b;
    if (t <= 66.0) {
        r = 255.0;
        g = 99.4708025861 * std::log(t) - 161.1195681661;
    } else {
        r = 329.698727446 * std::pow(t - 60.0, -0.1332047592);
        g = 288.1221695283 * std::pow(t - 60.0, -0.0755148492);
    }
    if (t >= 66.0) {
        b = 255.0;
    } else if (t <= 19.0) {
        b = 0.0;
    } else {
        b = 138.5177312231 * std::log(t - 10.0) - 305.0447927307;
    }
    auto channel = [](double v) { return static_cast<float>(std::clamp(v, 0.0, 255.0) / 255.0); };
    return {channel(r), channel(g), channel(b)};
}

glm::vec3 adjust_saturation(const glm::vec3& rgb, float s) {
    const float luma = 0.2126f * rgb.r + 0.7152f * rgb.g + 0.0722f * rgb.b;
    return glm::clamp(glm::mix(glm::vec3(luma), rgb, s), 0.0f, 1.0f);
}

namespace {
// The 9 canonical viridis stops (matplotlib), evenly spaced in t.
constexpr std::array<std::uint32_t, 9> kViridis = {
    0x440154, 0x472d7b, 0x3b528b, 0x2c728e, 0x21918c, 0x28ae80, 0x5ec962, 0xaddc30, 0xfde725};
}  // namespace

glm::vec3 viridis(float t) {
    t = glm::clamp(t, 0.0f, 1.0f);
    const float x = t * static_cast<float>(kViridis.size() - 1);  // position between stops
    const auto i = static_cast<std::size_t>(std::floor(x));
    if (i >= kViridis.size() - 1) return rgb_from_hex(kViridis.back());
    const float f = x - static_cast<float>(i);                     // fraction between stop i and i+1
    return glm::mix(rgb_from_hex(kViridis[i]), rgb_from_hex(kViridis[i + 1]), f);
}

glm::vec3 viridis_on_black(float t) {
    return viridis(0.15f + 0.85f * glm::clamp(t, 0.0f, 1.0f));
}

glm::vec3 diverging_blue_orange(float t) {
    t = glm::clamp(t, -1.0f, 1.0f);
    const glm::vec3 blue(0.25f, 0.55f, 1.0f), white(0.92f, 0.92f, 0.92f), orange(1.0f, 0.45f, 0.15f);
    return t < 0.0f ? glm::mix(white, blue, -t) : glm::mix(white, orange, t);
}

glm::vec3 spectral_class_rgb(char c) {
    switch (c) {
        case 'O': return rgb_from_hex(0x9b7bff);  // violet-blue: hottest, rarest
        case 'B': return rgb_from_hex(0x6f9dff);  // blue
        case 'A': return rgb_from_hex(0xb9dcff);  // pale blue (Sirius, Vega)
        case 'F': return rgb_from_hex(0xf4f6ff);  // white
        case 'G': return rgb_from_hex(0xffe066);  // yellow (the Sun)
        case 'K': return rgb_from_hex(0xff9933);  // orange
        case 'M': return rgb_from_hex(0xff4a33);  // red: ~3/4 of all nearby stars
        case 'L': return rgb_from_hex(0xc23a6a);  // brown dwarfs: crimson ...
        case 'T': return rgb_from_hex(0xa64dbf);  // ... magenta ...
        case 'Y': return rgb_from_hex(0x7a4dbf);  // ... purple (coldest)
        case 'D': return rgb_from_hex(0x4dfff0);  // white dwarfs: cyan so they stand out
        case 'C': return rgb_from_hex(0xd9261c);  // carbon stars
        case 'S': return rgb_from_hex(0xff8fa8);
        case 'W': return rgb_from_hex(0x99a3ff);  // Wolf-Rayet
        default:  return rgb_from_hex(0x6b6b6b);  // unknown
    }
}

}  // namespace starmap::render
