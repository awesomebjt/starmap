// =============================================================================
// systems/StarRenderSystem.cpp — ECS view -> GPU instance data, every frame
// (see StarRenderSystem.h for the design; StarRenderer.cpp for the bgfx side).
// =============================================================================
#include "systems/StarRenderSystem.h"

#include "ecs/Components.h"
#include "scene/Filters.h"
#include "scene/OrbitCamera.h"
#include "scene/RenderComponents.h"

#include <entt/entity/registry.hpp>

namespace starmap::systems {

uint32_t StarRenderSystem::update(entt::registry& registry, render::StarRenderer& renderer, bgfx::ViewId view,
                                  const render::StarRenderParams& params) {
    // EnTT idiom: a multi-component view. It iterates the SMALLEST of the three
    // storages and checks membership in the others (O(1) sparse-set lookups),
    // handing out references into the packed arrays. All three are const: this
    // system only reads.
    // entt::exclude<FilteredOut>: skip entities that HAVE the tag. This one
    // argument is the whole integration of the filter feature into rendering
    // (FilterSystem decides who is tagged). Exclusion costs one extra O(1)
    // membership test per star. See FilterSystem.h for alternatives.
    auto stars = registry.view<const ecs::Position, const ecs::DisplayColor, const scene::DisplaySize>(
        entt::exclude<scene::FilteredOut>);

    // The tag storage for Marked: storage<T>() gives direct access to the
    // sparse set, and contains(e) is the cheapest membership test there is.
    const auto& marked = registry.storage<scene::Marked>();

    instances_.clear();                     // keeps capacity
    instances_.reserve(stars.size_hint());  // no-op after the first frame
    for (auto [entity, pos, color, size] : stars.each()) {
        const glm::vec3& p = pos.galactic_pc;
        // DisplayColor.a is an intensity multiplier (see ColorSystem.h), not
        // blending alpha; the vertex shader applies it.
        instances_.push_back({p.x, p.y, p.z, size.radius_pc,
                              color.rgba.r, color.rgba.g, color.rgba.b, color.rgba.a,
                              marked.contains(entity) ? 1.0f : 0.0f, {0.0f, 0.0f, 0.0f}});
    }
    return renderer.submit(view, instances_, registry.ctx().get<scene::OrbitCamera>(), params);
}

}  // namespace starmap::systems
