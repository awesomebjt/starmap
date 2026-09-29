// =============================================================================
// systems/ColorSystem.cpp — scheme -> DisplayColor.
// =============================================================================
#include "systems/ColorSystem.h"

#include "ecs/Components.h"
#include "render/Colormaps.h"
#include "scene/Filters.h"
#include "scene/Tags.h"

#include <entt/entity/registry.hpp>
#include <glm/common.hpp>

#include <algorithm>
#include <cmath>

namespace starmap::systems {

using scene::ColorScheme;

namespace {
// Normalise v from [lo, hi] to [0, 1] (clamped).
float unit(float v, float lo, float hi) {
    return hi > lo ? glm::clamp((v - lo) / (hi - lo), 0.0f, 1.0f) : 0.5f;
}
glm::vec4 opaque(const glm::vec3& c) { return {c, 1.0f}; }
constexpr float kTemperatureSaturation = 2.0f;
}  // namespace

glm::vec3 color_for_value(ColorScheme scheme, float value, const scene::ColorRanges& r) {
    // The value -> RGB half of the mapping, shared by color_for() (one star)
    // and the UI legend / slider track (ui/FilterPanel.cpp), so the legend can
    // never disagree with what the stars show.
    switch (scheme) {
        case ColorScheme::Temperature:
            // x2 saturation: see render::adjust_saturation for why.
            return render::adjust_saturation(render::blackbody_rgb(value), kTemperatureSaturation);

        case ColorScheme::Diameter:
            // Radii span 0.01 (white dwarfs) .. 90 Rsun (giants): a LOG scale is
            // the only way to see structure at both ends.
            return render::viridis_on_black(unit(std::log10(std::max(value, 1e-6f)), r.log_radius_lo, r.log_radius_hi));

        case ColorScheme::Age:
            return render::viridis_on_black(unit(value, r.age_lo, r.age_hi));

        case ColorScheme::Metallicity: {
            // Diverging map centred on the Sun ([Fe/H] = 0); each side is scaled
            // separately because the data are asymmetric (-1.9 .. +0.4).
            const float feh = value;
            const float t = feh < 0.0f ? -glm::clamp(feh / (r.feh_lo < 0.0f ? r.feh_lo : -1.0f), 0.0f, 1.0f)
                                       : glm::clamp(feh / (r.feh_hi > 0.0f ? r.feh_hi : 0.5f), 0.0f, 1.0f);
            return render::diverging_blue_orange(t);
        }

        case ColorScheme::Planets: {
            // Log scale (most hosts have ONE planet, a few have 5-8, so a
            // linear scale would paint nearly all hosts the same colour), mapped
            // onto the upper half of viridis (teal -> yellow). The dark purple
            // low end of viridis is invisible against a black sky, which is the
            // wrong property for "look here, there are planets".
            const float n = std::max(value, 1.0f);
            const float nmax = static_cast<float>(std::max(r.planets_max, 2));
            const float t = std::log(n) / std::log(nmax);  // 1 planet -> 0, max -> 1
            return render::viridis(0.45f + 0.55f * glm::clamp(t, 0.0f, 1.0f));
        }

        case ColorScheme::SpectralClass:  // categorical: see render::spectral_class_rgb
        case ColorScheme::Count:
            break;
    }
    return glm::vec3(kNoDataColor);
}

glm::vec4 color_for(ColorScheme scheme, const ecs::StarInfo& info, const ecs::Physical& phys,
                    const scene::ColorRanges& r) {
    switch (scheme) {
        case ColorScheme::Temperature:
            if (!phys.teff_k) return kNoDataColor;
            return opaque(color_for_value(scheme, *phys.teff_k, r));

        case ColorScheme::SpectralClass:
            if (info.spectral_class == '?') return kNoDataColor;
            return opaque(render::spectral_class_rgb(info.spectral_class));

        case ColorScheme::Diameter:
            if (!phys.radius_sol || *phys.radius_sol <= 0.0f) return kNoDataColor;
            return opaque(color_for_value(scheme, *phys.radius_sol, r));

        case ColorScheme::Age:
            if (!phys.age_gyr) return kNoDataColor;
            // Only ~9 % of stars have an age: emphasise them (intensity 1.6).
            return {color_for_value(scheme, *phys.age_gyr, r), 1.6f};

        case ColorScheme::Metallicity:
            if (!phys.metallicity) return kNoDataColor;
            return opaque(color_for_value(scheme, *phys.metallicity, r));

        case ColorScheme::Planets:
            // Every star has a planet_count (0 if none known), so "no data" here
            // means "no known planets": keep them faint so hosts pop out.
            if (phys.planet_count <= 0) return {0.5f, 0.5f, 0.5f, 0.08f};
            // ~740 hosts among 92k stars: intensity 4 => 2x brighter (capped) and
            // a 2x larger minimum size, so every host is findable in the overview.
            return {color_for_value(scheme, static_cast<float>(phys.planet_count), r), 4.0f};

        case ColorScheme::Count:
            break;
    }
    return kNoDataColor;
}

bool update_colors(entt::registry& registry) {
    auto& settings = registry.ctx().get<scene::ColorSettings>();
    if (!settings.dirty) return false;

    // EnTT idiom: a VIEW over several component types visits exactly the
    // entities that have all of them. `const` in the template arguments marks
    // read-only access (EnTT enforces it at compile time and it documents
    // intent); DisplayColor is written, so it is non-const.
    // each() with a lambda (or the range-for over .each() used here) hands out
    // references straight into the packed component arrays — no lookups.
    auto view = registry.view<const ecs::StarInfo, const ecs::Physical, ecs::DisplayColor>();
    for (auto [entity, info, phys, color] : view.each()) {
        color.rgba = color_for(settings.scheme, info, phys, settings.ranges);
    }
    // Second pass, only while the tagged view is on. Scheme colours are
    // painted first so a star with no tag (Sol, and a selected star the
    // filter exempts) keeps the scheme. Walking the tag storage instead of
    // try_get on all 92k stars leaves the common path — the view off — as a
    // single loop; the tagged set is then a second, small one.
    if (const auto* filters = registry.ctx().find<scene::FilterSettings>(); filters && filters->only_tagged) {
        auto tagged = registry.view<const scene::StarTag, ecs::DisplayColor>();
        for (auto [entity, tag, color] : tagged.each()) {
            (void)entity;
            color.rgba = scene::tag_rgba(tag.color);
        }
    }
    settings.dirty = false;
    return true;
}

}  // namespace starmap::systems
