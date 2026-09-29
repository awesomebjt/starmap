// =============================================================================
// render/StarRenderer.cpp — unit quad, uniforms, instance batches, blend state
// (the design discussion, including the instance-data rules, is in StarRenderer.h;
// the per-vertex maths is in shaders/vs_star.sc).
// =============================================================================
#include "render/StarRenderer.h"

#include "render/ShaderLoader.h"
#include "scene/OrbitCamera.h"

#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace starmap::render {

namespace {
// One corner of the unit quad. z is always 0; kept as vec3 because the
// shared varying.def.sc declares a_position as vec3 (the line shader needs 3D).
struct QuadVertex {
    float x, y, z;
};
// Corners at +-1 so the fragment shader's uv (= corner) spans [-1,1] and the
// distance from the centre is simply length(uv).
constexpr QuadVertex kQuad[4] = {{-1.f, -1.f, 0.f}, {1.f, -1.f, 0.f}, {1.f, 1.f, 0.f}, {-1.f, 1.f, 0.f}};
// Two triangles. Winding doesn't matter: we disable back-face culling below
// (a billboard always faces the camera, and our right-handed projection flips
// the usual winding anyway — culling would just be one more thing to get wrong).
constexpr uint16_t kQuadIndices[6] = {0, 1, 2, 0, 2, 3};
}  // namespace

StarRenderer::StarRenderer(const ShaderLoader& shaders) {
    // A VertexLayout describes the bytes of ONE vertex so bgfx can bind them to
    // the shader's a_* inputs: here "3 floats = a_position". It must match both
    // the C++ struct and varying.def.sc.
    bgfx::VertexLayout layout;
    layout.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float).end();

    // Static buffers: uploaded once, live on the GPU. makeRef() would avoid a
    // copy, but only for data that outlives the upload — kQuad is constexpr
    // static storage, so makeRef is safe; copy() is the habit that never bites.
    quad_vb_ = bgfx::createVertexBuffer(bgfx::copy(kQuad, sizeof(kQuad)), layout);
    quad_ib_ = bgfx::createIndexBuffer(bgfx::copy(kQuadIndices, sizeof(kQuadIndices)));
    program_ = shaders.load_program("vs_star", "fs_star");

    // Uniforms are identified by NAME: the string must match the `uniform vec4
    // u_starParams;` declaration in the shader. Creating the same name twice
    // returns the same handle (reference counted).
    u_star_params_ = bgfx::createUniform("u_starParams", bgfx::UniformType::Vec4);
    u_star_params2_ = bgfx::createUniform("u_starParams2", bgfx::UniformType::Vec4);
    u_viewport_px_ = bgfx::createUniform("u_viewportPx", bgfx::UniformType::Vec4);
}

StarRenderer::~StarRenderer() {
    // bgfx handles are plain integers; nothing frees them automatically.
    // (A small RAII template per handle type is a nice exercise for later.)
    bgfx::destroy(u_viewport_px_);
    bgfx::destroy(u_star_params2_);
    bgfx::destroy(u_star_params_);
    bgfx::destroy(program_);
    bgfx::destroy(quad_ib_);
    bgfx::destroy(quad_vb_);
}

uint32_t StarRenderer::submit(bgfx::ViewId view, std::span<const StarInstance> instances,
                              const scene::OrbitCamera& camera, const StarRenderParams& p) {
    if (instances.empty()) return 0;

    // "Pixels per parsec at distance 1": a pinhole camera maps a size s at depth
    // d to s * f / d pixels, where the focal length in pixels is
    // f = (viewport_h / 2) / tan(fovY / 2). (= 0.5 * viewport_h * proj[1][1]).
    const float focal_px = 0.5f * static_cast<float>(camera.viewport_h) /
                           std::tan(glm::radians(camera.fov_y_deg) * 0.5f);
    const float params[4] = {focal_px, p.min_px, p.max_px, p.exposure};
    const float params2[4] = {p.dim_exponent, p.min_intensity, p.marker_min_px, p.max_intensity};
    const float viewport[4] = {static_cast<float>(camera.viewport_w), static_cast<float>(camera.viewport_h), 0.f, 0.f};

    // RENDER STATE for the star pass:
    //   WRITE_RGB       : write colour (no alpha channel needed, no depth write)
    //   BLEND_ADD       : dst = dst + src. Light adds up; order-independent, so
    //                     no depth sorting of 92k stars is needed.
    //   no DEPTH_TEST   : stars are glowing points, not solid objects. With a
    //                     depth test (and depth writes) the transparent corners of
    //                     a near quad would punch square holes into stars behind
    //                     it; with additive blending there is nothing to occlude.
    //   no CULL_*       : see kQuadIndices above.
    //   (MSAA not needed: the gaussian falloff is already smooth.)
    constexpr uint64_t kState = BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ADD;
    constexpr uint16_t kStride = sizeof(StarInstance);

    uint32_t submitted = 0;
    auto remaining = static_cast<uint32_t>(instances.size());
    while (remaining > 0) {
        // TRANSIENT instance buffer: memory that lives for exactly one frame.
        // Ask how many instances still fit this frame (all of them, normally —
        // see limits.maxTransientVbSize in Graphics.cpp), then allocate.
        const uint32_t n = bgfx::getAvailInstanceDataBuffer(remaining, kStride);
        if (n == 0) break;  // out of transient memory: draw what we have
        bgfx::InstanceDataBuffer idb;
        bgfx::allocInstanceDataBuffer(&idb, n, kStride);
        std::memcpy(idb.data, instances.data() + submitted, static_cast<std::size_t>(n) * kStride);

        // Per-draw state. bgfx is a *stateless-per-draw* API: everything set with
        // setVertexBuffer/setUniform/setState applies to the NEXT submit() only
        // and is reset afterwards, so it's re-set for each batch.
        bgfx::setVertexBuffer(0, quad_vb_);
        bgfx::setIndexBuffer(quad_ib_);
        bgfx::setInstanceDataBuffer(&idb);
        bgfx::setUniform(u_star_params_, params);
        bgfx::setUniform(u_star_params2_, params2);
        bgfx::setUniform(u_viewport_px_, viewport);
        bgfx::setState(kState);
        // submit() records the draw call into `view`; nothing is drawn yet.
        // The GPU work happens at bgfx::frame().
        bgfx::submit(view, program_);

        submitted += n;
        remaining -= n;
    }
    return submitted;
}

}  // namespace starmap::render
