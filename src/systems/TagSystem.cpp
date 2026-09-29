// =============================================================================
// systems/TagSystem.cpp — see TagSystem.h.
// =============================================================================
#include "systems/TagSystem.h"

#include "scene/ColorScheme.h"
#include "scene/Filters.h"
#include "scene/Tags.h"

#include <entt/entity/registry.hpp>

namespace starmap::systems {

void set_star_tag(entt::registry& registry, entt::entity e, int color) {
    if (!registry.valid(e)) return;
    if (color < 0 || color >= scene::kTagColorCount) color = 0;
    const auto index = static_cast<std::uint8_t>(color);
    // Same colour again (clicking the swatch that is already selected) must
    // not schedule a 92k recolour.
    if (const auto* existing = registry.try_get<scene::StarTag>(e); existing && existing->color == index) return;
    const bool was_tagged = registry.all_of<scene::StarTag>(e);
    registry.emplace_or_replace<scene::StarTag>(e, scene::StarTag{index});

    auto* filters = registry.ctx().find<scene::FilterSettings>();
    if (!filters || !filters->only_tagged) return;
    // The view is showing tag colours, so this star's pixel has to change.
    if (auto* colors = registry.ctx().find<scene::ColorSettings>()) colors->dirty = true;
    // A star that was only on screen because it is Sol or the selection
    // becomes "really" visible once it has a tag; the filter pass recomputes
    // selection_filtered. Recolouring an existing tag does not move anyone
    // in or out of the view.
    if (!was_tagged) filters->dirty = true;
}

void clear_star_tag(entt::registry& registry, entt::entity e) {
    if (!registry.valid(e) || !registry.all_of<scene::StarTag>(e)) return;
    registry.remove<scene::StarTag>(e);
    auto* filters = registry.ctx().find<scene::FilterSettings>();
    if (!filters || !filters->only_tagged) return;
    // It may now be hidden. If it stays (Sol, or still selected), it must
    // return to the scheme colour, so both passes run.
    filters->dirty = true;
    if (auto* colors = registry.ctx().find<scene::ColorSettings>()) colors->dirty = true;
}

}  // namespace starmap::systems
