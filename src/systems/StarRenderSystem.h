#pragma once
// =============================================================================
// systems/StarRenderSystem.h — ECS -> GPU bridge for stars.
//
// Every frame: iterate registry.view<const Position, const DisplayColor,
// const DisplaySize>(), pack one render::StarInstance per star into a CPU
// vector, and hand it to render::StarRenderer, which copies it into a bgfx
// transient instance buffer and issues the (single) instanced draw call.
//
// WHY REBUILD EVERY FRAME? Colours change when the scheme changes, and a
// future milestone will filter/hide stars interactively. Packing 92k x 48 B
// takes well under a millisecond, and transient buffers are exactly bgfx's tool
// for "data regenerated each frame". (Alternative: a bgfx::DynamicVertexBuffer
// used as instance buffer, updated only when colours change — worth it for
// millions of stars, not needed here.)
// =============================================================================

#include "render/StarRenderer.h"

#include <entt/entity/fwd.hpp>

#include <vector>

namespace starmap::systems {

class StarRenderSystem {
public:
    // Returns the number of stars submitted.
    uint32_t update(entt::registry& registry, render::StarRenderer& renderer, bgfx::ViewId view,
                    const render::StarRenderParams& params);

private:
    std::vector<render::StarInstance> instances_;  // reused every frame: no per-frame allocation
};

}  // namespace starmap::systems
