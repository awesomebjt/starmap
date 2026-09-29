#pragma once
// =============================================================================
// systems/SizeSystem.h — gives every star a scene::DisplaySize.
//
// Runs once after loading (sizes don't depend on the colour scheme). Separate
// from ColorSystem because "how big" and "what colour" change for different
// reasons — keeping systems single-purpose is what makes ECS code easy to
// extend (e.g. a later "size by planet count" mode touches only this file).
// =============================================================================

#include <entt/entity/fwd.hpp>

namespace starmap::ecs { struct Photometry; struct Physical; }

namespace starmap::systems {

// Display radius in parsecs for one star (pure function, unit-tested).
[[nodiscard]] float display_radius_pc(const ecs::Photometry& ph, const ecs::Physical& phys);

// Emplace-or-replace scene::DisplaySize on every entity that has Photometry + Physical.
void assign_display_sizes(entt::registry& registry);

}  // namespace starmap::systems
