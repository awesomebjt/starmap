// =============================================================================
// systems/SelectionSystem.cpp — see SelectionSystem.h
// =============================================================================
#include "systems/SelectionSystem.h"

#include "catalog/Sqlite.h"
#include "catalog/StarDetails.h"
#include "ecs/Components.h"
#include "scene/Selection.h"

#include <entt/entity/registry.hpp>

#include <chrono>

namespace starmap::systems {

namespace {
scene::SelectionState& state(entt::registry& registry) {
    // ctx().emplace() on an existing type just returns it, so this is "get or
    // create": the functions work even in a unit test that never set up ctx.
    return registry.ctx().contains<scene::SelectionState>() ? registry.ctx().get<scene::SelectionState>()
                                                            : registry.ctx().emplace<scene::SelectionState>();
}
}  // namespace

void select(entt::registry& registry, entt::entity e) {
    if (!registry.valid(e)) return clear_selection(registry);
    auto& s = state(registry);
    if (s.entity == e && registry.all_of<ecs::Selected>(e)) return;  // no change, keep the version
    // storage<Selected>().clear() removes the tag from EVERY entity in one go
    // (it is just a sparse set; clearing is O(size), here 0 or 1 elements).
    // This enforces the "exactly one" invariant even if something went wrong.
    registry.storage<ecs::Selected>().clear();
    registry.emplace<ecs::Selected>(e);
    s.entity = e;
    ++s.version;
}

void clear_selection(entt::registry& registry) {
    auto& s = state(registry);
    if (s.entity == entt::null && registry.storage<ecs::Selected>().empty()) return;
    registry.storage<ecs::Selected>().clear();
    s.entity = entt::null;
    ++s.version;
}

entt::entity selected_entity(const entt::registry& registry) {
    // ctx().find<T>() returns a pointer (nullptr if absent) — the non-throwing
    // lookup, handy for optional singletons.
    // valid(): the selected entity may have been destroyed since (a reload,
    // a test); a stale handle must read as "nothing selected".
    const auto* s = registry.ctx().find<scene::SelectionState>();
    return s && registry.valid(s->entity) ? s->entity : entt::null;
}

bool update_details(entt::registry& registry, catalog::Database* db) {
    const auto& s = state(registry);
    auto& d = registry.ctx().contains<scene::SelectionDetails>() ? registry.ctx().get<scene::SelectionDetails>()
                                                                 : registry.ctx().emplace<scene::SelectionDetails>();
    if (d.version == s.version) return false;  // nothing changed since the last load
    d.version = s.version;
    d.entity = s.entity;
    d.details.reset();
    d.error.clear();
    d.load_ms = 0.0;
    if (s.entity == entt::null) return true;
    const auto* id = registry.try_get<ecs::StarId>(s.entity);
    if (!id) {
        d.error = "entity has no StarId";
        return true;
    }
    if (!db) {
        d.error = "catalog database not available";
        return true;
    }
    const auto t0 = std::chrono::steady_clock::now();
    try {
        d.details = catalog::load_star_details(*db, id->value);
    } catch (const std::exception& ex) {
        // A details query failing must not take the app down: show why instead.
        d.error = ex.what();
    }
    d.load_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

}  // namespace starmap::systems
