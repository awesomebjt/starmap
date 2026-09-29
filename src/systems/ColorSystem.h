#pragma once
// =============================================================================
// systems/ColorSystem.h — writes ecs::DisplayColor for every star according to
// the active colour scheme (scene::ColorSettings in registry.ctx()).
//
// ROLE IN THE ARCHITECTURE (frame loop)
//     InputHandler --(key 1-6: ColorSettings::set(), dirty = true)--> ColorSystem
//     ColorSystem  --(reads StarInfo + Physical, writes DisplayColor)--> StarRenderSystem
//   The render system never knows which scheme is active: it just draws
//   DisplayColor. Adding a scheme never touches rendering code.
// =============================================================================

#include "scene/ColorScheme.h"

#include <entt/entity/fwd.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace starmap::ecs { struct StarInfo; struct Physical; }

namespace starmap::systems {

// DisplayColor.rgba.a is an INTENSITY multiplier, not blending alpha (stars are
// blended additively): < 1 recedes, 1 normal, > 1 emphasised — brighter AND a
// larger minimum size in the vertex shader. Schemes where only a small subset
// has data (Age 9 %, Exoplanets 1 %) emphasise the stars that do.
// Colour for unknown values: dim grey, visible for context but receding.
inline constexpr glm::vec4 kNoDataColor{0.55f, 0.55f, 0.55f, 0.25f};

// Value -> RGB for the range schemes (Temperature, Diameter, Age, Metallicity,
// Planets). Shared by color_for() and the UI legend so both always agree.
// SpectralClass is categorical: use render::spectral_class_rgb instead.
[[nodiscard]] glm::vec3 color_for_value(scene::ColorScheme scheme, float value, const scene::ColorRanges& ranges);

// The pure mapping (unit-tested): one star's data -> colour under `scheme`.
[[nodiscard]] glm::vec4 color_for(scene::ColorScheme scheme, const ecs::StarInfo& info,
                                  const ecs::Physical& phys, const scene::ColorRanges& ranges);

// If ColorSettings::dirty, recolour all stars and clear the flag.
// When FilterSettings::only_tagged is set, stars that carry a StarTag are
// then overwritten with the tag palette (scene/Tags.h); everyone else keeps
// the scheme colour. The widget that toggles only_tagged sets `dirty`,
// because this function does not watch the filter flag on its own.
// Returns true when it did work (useful for logging/timing).
bool update_colors(entt::registry& registry);

}  // namespace starmap::systems
