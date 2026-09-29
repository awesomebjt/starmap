#pragma once
// =============================================================================
// systems/TagSystem.h — write or clear a star's tag colour
// =============================================================================
//
// The tag menu (and the "Remove tag" button in the details tab) call these
// instead of emplacing StarTag themselves. One function keeps the component
// and the two dirty flags in agreement:
//   * FilterSettings::dirty when membership changes while the tagged-only
//     view is on (the star appears or disappears);
//   * ColorSettings::dirty when the colour on screen must change, which is
//     only while that view is on. With it off, tags are stored and the
//     scheme colours stay until the user turns the view on (that toggle
//     sets the colour dirty flag itself).
// A click is rare, so the flags mean "recolour / refilter once", never
// "every frame".
// =============================================================================

#include <entt/entity/fwd.hpp>

namespace starmap::systems {

// `color` is clamped into 0 .. kTagColorCount-1. No-op if the star already
// wears that colour, or if `e` is not a live entity. An int, not a uint8_t:
// the menu's loop index and a typed test both hand over a plain int, and a
// narrowing parameter would warn at every call.
void set_star_tag(entt::registry& registry, entt::entity e, int color);

// No-op if the star has no tag.
void clear_star_tag(entt::registry& registry, entt::entity e);

}  // namespace starmap::systems
