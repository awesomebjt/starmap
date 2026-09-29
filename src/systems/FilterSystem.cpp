// =============================================================================
// systems/FilterSystem.cpp — see FilterSystem.h for the design discussion.
// =============================================================================
#include "systems/FilterSystem.h"

#include "ecs/Components.h"
#include "scene/ColorScheme.h"
#include "scene/Filters.h"
#include "scene/Tags.h"
#include "systems/SelectionSystem.h"

#include <entt/entity/registry.hpp>

#include <chrono>

namespace starmap::systems {

bool update_filters(entt::registry& registry) {
    auto& settings = registry.ctx().get<scene::FilterSettings>();
    const scene::ColorScheme active = registry.ctx().get<scene::ColorSettings>().scheme;
    // Changing the active scheme changes WHICH filter applies (unless in
    // combine mode, but re-running is cheap and keeps the rule simple).
    const entt::entity selection = selected_entity(registry);
    if (!settings.dirty && settings.applied_scheme == active && settings.applied_selection == selection) return false;

    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<scene::ColorScheme> applied = scene::applied_filters(settings, active);
    // Distance and the tagged-only view are not scheme filters, so the
    // "nothing applied" fast path has to know about them too. Otherwise
    // turning on "show only tagged" with no scheme filter would clear every
    // FilteredOut tag and show the whole catalog.
    const bool distance_on = scene::distance_is_trimmed(settings);
    const bool tags_on = settings.only_tagged;

    // storage<T>() is the component pool for T. For an empty (tag) type it is
    // just a sparse set of entities: contains/emplace/erase are O(1).
    // Working on the storage directly instead of registry.emplace/remove
    // skips a validity check per call; both would be correct here.
    auto& hidden = registry.storage<scene::FilteredOut>();

    if (applied.empty() && !distance_on && !tags_on) {
        // Fast path: no filter applies -> nobody is hidden.
        hidden.clear();
    } else {
        // Iterate the two components the scheme predicate reads. `const` =
        // read-only. Modifying the FilteredOut pool during this loop is safe
        // because the view does not iterate that pool (adding to the pool we
        // iterate would be the classic "modify while iterating" bug).
        // Position and StarTag are looked up only when that rule is active:
        // a temperature drag should not also touch those storages. A star
        // with no Position (the hand-made filter tests) counts as 0 ly, the
        // origin, which the full-range distance filter never rejects.
        const auto stars = registry.view<const ecs::StarInfo, const ecs::Physical>();
        for (auto [entity, info, phys] : stars.each()) {
            float dist_ly = 0.0f;
            if (distance_on) {
                if (const auto* pos = registry.try_get<ecs::Position>(entity)) dist_ly = pos->dist_ly;
            }
            const bool tagged = tags_on && registry.all_of<scene::StarTag>(entity);
            const bool visible = scene::star_visible(settings, applied, info, phys, dist_ly, tagged);
            // Only touch the pool when the state CHANGES: dragging a handle by a
            // few pixels flips a few hundred stars, not all 92k.
            const bool is_hidden = hidden.contains(entity);
            if (!visible && !is_hidden) hidden.emplace(entity);
            else if (visible && is_hidden) hidden.erase(entity);
        }
    }

    // The selected star stays visible whatever the filters say (see
    // FilterSettings::applied_selection). Remember whether it WOULD have
    // been hidden so the details view can say so.
    settings.selection_filtered = false;
    if (selection != entt::null && hidden.contains(selection)) {
        hidden.erase(selection);
        settings.selection_filtered = true;
    }
    settings.applied_selection = selection;

    settings.visible = settings.total_stars - hidden.size();
    settings.applied_scheme = active;
    settings.dirty = false;
    settings.last_pass_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

}  // namespace starmap::systems
