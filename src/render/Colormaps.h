#pragma once
// =============================================================================
// render/Colormaps.h — pure functions that turn numbers into colours.
//
// ROLE IN THE ARCHITECTURE
//   ColorSystem (systems/ColorSystem.cpp) decides WHICH quantity to show
//   (temperature, age, ...) and normalises it; the functions here decide what
//   colour a value becomes. They have no dependency on bgfx, SDL or EnTT, so
//   they are unit-tested directly (tests/test_app_core.cpp) and could be reused
//   by the future Dear ImGui legend to draw colour bars.
//
// COLOUR SPACE NOTE
//   All outputs are *sRGB-encoded* values in [0,1] — the same numbers you would
//   type into a colour picker. Our back buffer is a plain (non-sRGB) RGBA8
//   surface, so these values go to the screen unchanged. (Strictly speaking,
//   additive blending should happen in *linear* light; doing it on sRGB values
//   makes overlapping stars saturate a little faster. For a star map that is
//   invisible in practice, and it keeps the pipeline simple. A later milestone
//   with HDR/bloom would switch to linear colour + an sRGB back buffer.)
// =============================================================================

#include <glm/vec3.hpp>

#include <cstdint>

namespace starmap::render {

// Colour of a black body at `kelvin`, as perceived with a daylight (D65-like)
// white point: ~6600 K is white, cooler is orange/red, hotter is blue-white.
// Uses Tanner Helland's curve fit (2012) to Mitchell Charity's blackbody
// table; valid 1000 K .. 40000 K (inputs are clamped to that range).
//   * T. Helland, "How to Convert Temperature (K) to RGB: Algorithm and Sample Code",
//     https://tannerhelland.com/2012/09/18/convert-temperature-rgb-algorithm-code.html
//   * M. Charity, "What color is a blackbody? - some pixel rgb values",
//     http://www.vendian.org/mncharity/dir3/blackbody/
[[nodiscard]] glm::vec3 blackbody_rgb(float kelvin);

// Matplotlib's "viridis" colormap (perceptually uniform, colour-blind friendly),
// t clamped to [0,1]. Implemented as linear interpolation between the 9 standard
// stops #440154 ... #fde725 (t = 0, 0.125, ..., 1).
[[nodiscard]] glm::vec3 viridis(float t);

// Viridis without its darkest ~15 %: the stock dark-purple end is nearly
// invisible as a small point on a black background. Use this for star colours.
[[nodiscard]] glm::vec3 viridis_on_black(float t);

// Diverging map for signed quantities centred on a meaningful zero
// (metallicity: 0 = solar). t in [-1,1]: -1 blue, 0 near-white, +1 orange-red.
[[nodiscard]] glm::vec3 diverging_blue_orange(float t);

// Categorical palette for the first letter of a spectral type
// (O B A F G K M, L T Y brown dwarfs, D white dwarfs, C S W rarer classes,
// '?' unknown -> grey). Chosen to be DISTINCT rather than physically accurate:
// the Temperature scheme already shows the physical colours.
[[nodiscard]] glm::vec3 spectral_class_rgb(char spectral_class);

// Push a colour away from (s > 1) or towards (s < 1) its grey of equal luma
// (Rec. 709 weights), clamped to [0,1]. Real blackbody colours are very pale
// (a 4000 K star is only faintly orange); on 2-pixel points those differences
// vanish, so the Temperature scheme exaggerates saturation. The HUE ordering,
// which is what carries the information, is unchanged.
[[nodiscard]] glm::vec3 adjust_saturation(const glm::vec3& rgb, float s);

// 0xRRGGBB -> sRGB floats in [0,1].
[[nodiscard]] glm::vec3 rgb_from_hex(std::uint32_t rrggbb);

}  // namespace starmap::render
