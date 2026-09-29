// =============================================================================
// scene/ColorScheme.cpp — names, ranges and legend text for the colour schemes.
// =============================================================================
#include "scene/ColorScheme.h"

#include "catalog/CatalogStats.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace starmap::scene {
namespace {
// Order MUST match the enum. A static_assert below catches a forgotten row.
constexpr std::array<ColorSchemeInfo, static_cast<std::size_t>(ColorScheme::Count)> kInfo = {{
    {"temperature", "Temperature"},
    {"spectral", "Spectral class"},
    {"diameter", "Diameter"},
    {"age", "Age"},
    {"metallicity", "Metallicity"},
    {"planets", "Exoplanets"},
}};
static_assert(kInfo.size() == static_cast<std::size_t>(ColorScheme::Count));
}  // namespace

const ColorSchemeInfo& scheme_info(ColorScheme s) { return kInfo[static_cast<std::size_t>(s)]; }

std::optional<ColorScheme> scheme_from_string(std::string_view key) {
    for (std::size_t i = 0; i < kInfo.size(); ++i) {
        if (key == kInfo[i].key) return static_cast<ColorScheme>(i);
    }
    if (key == "temp" || key == "teff") return ColorScheme::Temperature;  // friendly aliases
    if (key == "radius") return ColorScheme::Diameter;
    if (key == "feh") return ColorScheme::Metallicity;
    return std::nullopt;
}

ColorRanges make_color_ranges(const catalog::CatalogStats& stats) {
    ColorRanges r;
    if (const auto* f = stats.field("radius_sol"); f && f->count > 0 && f->p01 > 0.0) {
        // log10 is monotonic, so log10(percentile) == percentile of log10.
        r.log_radius_lo = static_cast<float>(std::log10(f->p01));
        r.log_radius_hi = static_cast<float>(std::log10(f->p99));
    }
    if (const auto* f = stats.field("age_gyr"); f && f->count > 0) {
        r.age_lo = static_cast<float>(f->p01);
        r.age_hi = static_cast<float>(f->p99);
    }
    if (const auto* f = stats.field("metallicity"); f && f->count > 0) {
        // p01/p99 are -1.9 / +0.4, but almost all nearby (thin-disk) stars lie
        // within -0.7 .. +0.4; using the full tail would paint nearly every star
        // the same near-white. Clamp the metal-poor end so the disk shows contrast.
        r.feh_lo = std::max(static_cast<float>(f->p01), -0.7f);  // negative: metal-poor end
        r.feh_hi = std::min(static_cast<float>(f->p99), 0.4f);   // positive: metal-rich end
    }
    if (const auto* f = stats.field("planet_count"); f && f->count > 0) {
        r.planets_max = static_cast<int>(f->max);
    }
    return r;
}

std::string scheme_legend(ColorScheme s, const ColorRanges& r) {
    char buf[160];
    switch (s) {
        case ColorScheme::Temperature:
            std::snprintf(buf, sizeof buf, "blackbody colour of Teff: 2500 K red .. 5800 K white-yellow .. 12000 K blue; grey = unknown");
            break;
        case ColorScheme::SpectralClass:
            std::snprintf(buf, sizeof buf, "O violet B blue A pale-blue F white G yellow K orange M red | L T Y brown dwarfs | D white dwarf cyan | ? grey");
            break;
        case ColorScheme::Diameter:
            std::snprintf(buf, sizeof buf, "radius (log): %.2f Rsun dark .. %.1f Rsun yellow (viridis); grey = unknown",
                          std::pow(10.0, static_cast<double>(r.log_radius_lo)), std::pow(10.0, static_cast<double>(r.log_radius_hi)));
            break;
        case ColorScheme::Age:
            std::snprintf(buf, sizeof buf, "age: %.1f Gyr dark .. %.1f Gyr yellow (viridis); grey = unknown",
                          static_cast<double>(r.age_lo), static_cast<double>(r.age_hi));
            break;
        case ColorScheme::Metallicity:
            std::snprintf(buf, sizeof buf, "[Fe/H]: %.2f blue (metal-poor) .. 0 white (solar) .. +%.2f orange (metal-rich); grey = unknown",
                          static_cast<double>(r.feh_lo), static_cast<double>(r.feh_hi));
            break;
        case ColorScheme::Planets:
            std::snprintf(buf, sizeof buf, "known exoplanets (log): none = faint grey, 1 teal .. %d yellow; hosts drawn larger", r.planets_max);
            break;
        case ColorScheme::Count:
            buf[0] = '\0';
            break;
    }
    return buf;
}

}  // namespace starmap::scene
