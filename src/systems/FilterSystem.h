#pragma once
// =============================================================================
// systems/FilterSystem.h — FilterSettings -> FilteredOut tags
// =============================================================================
//
// ROLE IN THE ARCHITECTURE (frame loop, after the UI and before rendering)
//     FilterPanel / CLI --edit--> FilterSettings (ctx, dirty = true)
//     update_filters()  --runs star_visible() on every star, only when dirty
//                          or when the active scheme changed--> FilteredOut tag
//                          (scheme filters, the distance range, and the
//                          tagged-only view, all AND)
//     StarRenderSystem  --view<Position, DisplayColor, DisplaySize>(exclude<FilteredOut>)
//
// HOW TO REPRESENT "HIDDEN" IN AN ECS — three options were considered:
//
//  1. A tag component (chosen). Hidden stars carry an empty FilteredOut
//     component; the render view excludes it. Changing the filter is a
//     STRUCTURAL change (add/remove components), which in EnTT is an O(1)
//     swap-and-pop in the tag's sparse set, and only for stars whose
//     visibility actually changed (we check contains() first). The per-frame
//     render loop pays nothing for filters it isn't using.
//     Cost: "tag churn" while the user drags a slider (thousands of
//     adds/removes per frame) — measured at a few ms for 92k stars, fine.
//
//  2. A per-frame branch: store `bool visible` in a component (or recompute
//     the predicate) and `if (!visible) continue;` in the render loop. No
//     structural changes ever, but every frame iterates and tests all 92k
//     stars, even when nothing changed — the dirty-flag approach does the
//     work only when inputs change.
//
//  3. An EnTT GROUP: an owning group over <Position, DisplayColor, DisplaySize>
//     with exclude<FilteredOut> keeps the visible stars packed contiguously
//     at the front of those arrays, so rendering iterates only visible stars
//     with perfect cache locality. But every tag change then SWAPS component
//     data around inside the owned pools, groups restrict which other groups
//     may own the same components, and the gain (~0.5 ms per frame here) is
//     not worth the complexity at 92k stars. Worth revisiting for millions.
//
// Also: the total count of visible stars falls out of this pass for free
// (total - hidden), which is what the panel's "Showing N of M" displays.
// =============================================================================

#include <entt/entity/fwd.hpp>

namespace starmap::systems {

// Recomputes FilteredOut if FilterSettings::dirty, the active colour scheme
// changed, or the selection changed (the selected star is exempt) since the
// last pass. Updates FilterSettings::visible and
// last_pass_ms. Returns true when it did work.
bool update_filters(entt::registry& registry);

}  // namespace starmap::systems
