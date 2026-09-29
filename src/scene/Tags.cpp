// =============================================================================
// scene/Tags.cpp — the 16 tag colours.
//
// The palette is data, not a colormap: each entry is a colour the user
// picked by hand, drawn exactly, with no scaling by temperature or magnitude.
// They are spaced around the hue wheel (plus white and copper) so two
// neighbours stay distinguishable on a black sky. Values stay at or below 1
// because the star shader blends additively; a channel above 1 clips to white.
// =============================================================================
#include "scene/Tags.h"

namespace starmap::scene {
namespace {

struct Entry {
    float r, g, b;
    const char* name;
};

// Order is the menu order, row-major, four per row: warm colours, then
// greens, then blues, then magentas, then the two neutrals.
constexpr Entry kPalette[kTagColorCount] = {
    {0.95f, 0.22f, 0.18f, "Red"},
    {1.00f, 0.50f, 0.12f, "Orange"},
    {1.00f, 0.76f, 0.18f, "Amber"},
    {0.98f, 0.94f, 0.28f, "Yellow"},
    {0.60f, 0.92f, 0.20f, "Lime"},
    {0.16f, 0.80f, 0.32f, "Green"},
    {0.12f, 0.80f, 0.62f, "Teal"},
    {0.22f, 0.86f, 0.98f, "Cyan"},
    {0.28f, 0.56f, 1.00f, "Azure"},
    {0.32f, 0.34f, 0.98f, "Blue"},
    {0.50f, 0.30f, 0.96f, "Indigo"},
    {0.72f, 0.32f, 0.96f, "Violet"},
    {0.96f, 0.24f, 0.72f, "Magenta"},
    {1.00f, 0.46f, 0.62f, "Rose"},
    {0.94f, 0.94f, 0.94f, "White"},
    {0.80f, 0.52f, 0.28f, "Copper"},
};

const Entry& entry(std::uint8_t color) {
    return kPalette[color < static_cast<std::uint8_t>(kTagColorCount) ? color : 0];
}

}  // namespace

glm::vec3 tag_rgb(std::uint8_t color) {
    const Entry& e = entry(color);
    return {e.r, e.g, e.b};
}

glm::vec4 tag_rgba(std::uint8_t color) { return {tag_rgb(color), 1.0f}; }

const char* tag_color_name(std::uint8_t color) {
    if (color >= static_cast<std::uint8_t>(kTagColorCount)) return "?";
    return kPalette[color].name;
}

}  // namespace starmap::scene
