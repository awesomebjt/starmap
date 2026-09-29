#pragma once
// =============================================================================
// render/StarRenderer.h — GPU resources and the draw call(s) for stars.
//
// ROLE IN THE ARCHITECTURE
//   StarRenderSystem (ECS side) gathers one StarInstance per star from the
//   registry; StarRenderer (GPU side) owns the shader program, the shared unit
//   quad and the uniforms, and submits the instances. Splitting "what to draw"
//   (ECS) from "how to draw" (bgfx) keeps bgfx calls out of the systems and
//   lets the renderer be reused for, say, a minimap view.
//
// INSTANCED RENDERING IN ONE PARAGRAPH
//   Drawing 92k stars as 92k separate draw calls would drown the CPU/driver in
//   per-call overhead. Instead we submit ONE quad (4 vertices, 6 indices) and
//   tell the GPU "draw it N times"; per-instance data (position, size, colour)
//   comes from a second buffer that advances once per INSTANCE instead of once
//   per vertex. In bgfx that buffer is an InstanceDataBuffer and the shader sees
//   its fields as i_data0, i_data1, ... (see shaders/varying.def.sc).
// =============================================================================

#include <bgfx/bgfx.h>

#include <cstdint>
#include <span>

namespace starmap::scene { struct OrbitCamera; }

namespace starmap::render {

class ShaderLoader;

// Exactly the per-instance layout the vertex shader expects: three vec4s.
// The instance stride must be a multiple of 16 bytes (one vec4 per i_dataN);
// static_asserts below make a layout mistake a compile error, not black screen.
struct StarInstance {
    float x, y, z;       // i_data0.xyz  galactic position (pc)
    float radius_pc;     // i_data0.w    display radius (pc)
    float r, g, b;       // i_data1.rgb  colour (sRGB 0..1)
    float intensity;     // i_data1.w    brightness multiplier (DisplayColor.a); > 1 also enlarges
    float ring;          // i_data2.x    1 = highlight ring (Sol / Marked tag)
    float reserved[3];   // i_data2.yzw  padding to a whole vec4 (free for future per-star data)
};
static_assert(sizeof(StarInstance) == 48, "StarInstance must be 3 x vec4 (48 bytes)");
static_assert(sizeof(StarInstance) % 16 == 0, "bgfx instance stride must be a multiple of 16");

// Artistic knobs (uniforms). Defaults tuned by looking at screenshots.
struct StarRenderParams {
    float min_px = 1.6f;          // stars never smaller than this radius (pixels): avoids sub-pixel flicker
    float max_px = 40.0f;         // cap for very close stars
    float exposure = 1.0f;        // global brightness
    float dim_exponent = 0.8f;    // dimming of enlarged stars: (true/drawn)^k
    float min_intensity = 0.10f;  // floor so the faintest stars remain visible
    float marker_min_px = 9.0f;   // marked stars (Sol) are at least this big, so the ring is visible
    float max_intensity = 2.0f;   // cap on emphasis brightness (avoid pure-white blobs)
};

class StarRenderer {
public:
    explicit StarRenderer(const ShaderLoader& shaders);
    ~StarRenderer();
    StarRenderer(const StarRenderer&) = delete;
    StarRenderer& operator=(const StarRenderer&) = delete;

    // Submit all instances to `view`. The view's transform must already be set
    // (bgfx::setViewTransform) — the shader reads u_view/u_proj from it.
    // Returns the number of instances actually submitted (can be lower if the
    // transient buffer is exhausted).
    uint32_t submit(bgfx::ViewId view, std::span<const StarInstance> instances,
                    const scene::OrbitCamera& camera, const StarRenderParams& params);

private:
    bgfx::VertexBufferHandle quad_vb_ = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle quad_ib_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle program_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_star_params_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_star_params2_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_viewport_px_ = BGFX_INVALID_HANDLE;
};

}  // namespace starmap::render
