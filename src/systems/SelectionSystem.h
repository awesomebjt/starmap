#pragma once
// =============================================================================
// systems/SelectionSystem.h — the ONLY place that changes the selection
// =============================================================================
//
// See scene/Selection.h for why the selection is both a tag (ecs::Selected)
// and a ctx variable (SelectionState). These functions keep them in sync:
//   select()          : clear the tag everywhere, put it on one entity,
//                       update SelectionState and bump its version.
//   clear_selection() : remove the tag, reset SelectionState, bump version.
//   update_details()  : if the selection changed since the last call, fetch
//                       StarDetails for it from SQLite (lazily; see
//                       catalog/StarDetails.h) into ctx SelectionDetails.
// =============================================================================

#include <entt/entity/fwd.hpp>

namespace starmap::catalog { class Database; }

namespace starmap::systems {

void select(entt::registry& registry, entt::entity e);
void clear_selection(entt::registry& registry);
// entt::null when nothing is selected (or the ctx variable doesn't exist).
[[nodiscard]] entt::entity selected_entity(const entt::registry& registry);

// Returns true if it (re)loaded. `db` may be nullptr (tests, or the db could
// not be opened): the details then contain only an error message.
bool update_details(entt::registry& registry, catalog::Database* db);

}  // namespace starmap::systems
