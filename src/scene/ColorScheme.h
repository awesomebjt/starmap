#pragma once
// =============================================================================
// scene/ColorScheme.h — which physical quantity the star colours represent.
//
// ROLE IN THE ARCHITECTURE
//   `ColorSettings` is a registry context variable (one per registry, like the
//   camera). InputHandler changes `scheme` (keys 1-6) and sets `dirty`;
//   ColorSystem::update() sees `dirty`, recolours every star ONCE, and clears
//   the flag. Recolouring 92k stars takes ~2 ms, so doing it every frame would
//   be wasteful but not fatal — the dirty flag is the standard "only recompute
//   derived data when its inputs changed" pattern you will use everywhere in a
//   game (e.g. rebuilding a navmesh, re-sorting a UI list).
//
// ADDING A NEW SCHEME = 3 small edits:
//   1. add an enumerator before `Count` below,
//   2. add a row to the table in ColorScheme.cpp (name, CLI key, legend),
//   3. add a `case` in ColorSystem.cpp's color_for().
//   The number key is its position in the enum + 1.
// =============================================================================

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace starmap::catalog { struct CatalogStats; }

namespace starmap::scene {

enum class ColorScheme : std::uint8_t {
    Temperature,    // blackbody colour of teff_k            (key 1)
    SpectralClass,  // categorical O B A F G K M L T Y D     (key 2)
    Diameter,       // log10(radius_sol) through viridis     (key 3)
    Age,            // age_gyr through viridis               (key 4)
    Metallicity,    // [Fe/H] through a diverging map        (key 5)
    Planets,        // number of known exoplanets            (key 6)
    Count           // number of schemes (not a scheme)
};

struct ColorSchemeInfo {
    const char* key;    // command-line spelling: "temperature", "spectral", ...
    const char* title;  // shown in the HUD
};

[[nodiscard]] const ColorSchemeInfo& scheme_info(ColorScheme s);
[[nodiscard]] std::optional<ColorScheme> scheme_from_string(std::string_view key);

// Value ranges that map onto the ends of each colour ramp. Filled once from
// CatalogStats (the 1st/99th percentiles, so a handful of outliers don't
// squash everyone else into one colour).
struct ColorRanges {
    float log_radius_lo = -1.0f, log_radius_hi = 0.5f;  // log10(R / Rsun)
    float age_lo = 0.5f, age_hi = 12.0f;                // Gyr
    float feh_lo = -1.0f, feh_hi = 0.4f;                // dex; blue end / orange end
    int planets_max = 8;
};
[[nodiscard]] ColorRanges make_color_ranges(const catalog::CatalogStats& stats);

// One-line legend text for the HUD, e.g. "blackbody 2,400 K (red) .. 12,000 K (blue)".
[[nodiscard]] std::string scheme_legend(ColorScheme s, const ColorRanges& r);

struct ColorSettings {
    ColorScheme scheme = ColorScheme::Temperature;
    bool dirty = true;  // true => ColorSystem must rewrite every DisplayColor this frame
    ColorRanges ranges;

    void set(ColorScheme s) {
        if (s != scheme) {
            scheme = s;
            dirty = true;
        }
    }
};

}  // namespace starmap::scene
