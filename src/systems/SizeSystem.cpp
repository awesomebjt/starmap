// =============================================================================
// systems/SizeSystem.cpp — how big each star is drawn.
//
// THE FORMULA AND WHY
//   Real stellar radii (0.1 .. 100 Rsun = 2e-9 .. 2e-6 pc) are hopelessly
//   sub-pixel at any useful zoom, so the billboard size is a *visual* size.
//   We base it on luminosity, which drives how prominent a star looks:
//       L / Lsun = 10^(-0.4 * (M_abs - 4.83))        (4.83 = Sun's absolute V mag)
//       radius_pc = 0.12 pc * clamp(L^0.25, 0.3, 4)
//   The 4th root compresses the enormous range of L (1e-6 .. 1e3) into a
//   factor of ~13 in size. 0.12 pc makes the Sun ~6 px across at 10 pc with the
//   default 50 deg FOV at 720 px height. The vertex shader then clamps to a
//   pixel range and dims stars it had to enlarge (see vs_star.sc), so distant
//   and faint stars are drawn as dim points rather than vanishing.
//   Stars without an absolute magnitude (0.1 % of the catalogue) fall back to
//   the radius-luminosity relation L ~ R^2 T^4 when possible, else a default.
// =============================================================================
#include "systems/SizeSystem.h"

#include "ecs/Components.h"
#include "scene/RenderComponents.h"

#include <entt/entity/registry.hpp>

#include <algorithm>
#include <cmath>

namespace starmap::systems {

namespace {
constexpr float kBaseRadiusPc = 0.12f;
constexpr float kSunAbsMag = 4.83f;
}  // namespace

float display_radius_pc(const ecs::Photometry& ph, const ecs::Physical& phys) {
    float lum = 0.3f;  // default: a typical faint catalogue star
    if (ph.abs_mag) {
        lum = std::pow(10.0f, -0.4f * (*ph.abs_mag - kSunAbsMag));
    } else if (phys.luminosity_sol) {
        lum = *phys.luminosity_sol;
    } else if (phys.radius_sol && phys.teff_k) {
        const float t = *phys.teff_k / 5772.0f;
        lum = *phys.radius_sol * *phys.radius_sol * t * t * t * t;  // Stefan-Boltzmann, solar units
    }
    const float scale = std::clamp(std::pow(std::max(lum, 1e-12f), 0.25f), 0.3f, 4.0f);
    return kBaseRadiusPc * scale;
}

void assign_display_sizes(entt::registry& registry) {
    auto view = registry.view<const ecs::Photometry, const ecs::Physical>();
    // EnTT idiom: reserve the storage once (we know the count) so emplace
    // never reallocates. view.size_hint() is an upper bound of the entity count.
    registry.storage<scene::DisplaySize>().reserve(view.size_hint());
    for (auto [entity, ph, phys] : view.each()) {
        // Adding a component of a type the view does NOT iterate is safe while
        // iterating; adding/removing Photometry or Physical here would not be.
        // emplace_or_replace: safe to call twice (e.g. after reloading the catalog).
        registry.emplace_or_replace<scene::DisplaySize>(entity, display_radius_pc(ph, phys));
    }
}

}  // namespace starmap::systems
