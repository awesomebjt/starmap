// =============================================================================
// render/GuideRenderer.cpp — distance rings, longitude spokes, NGP tick, labels
// =============================================================================
//
// A second, much simpler bgfx renderer than StarRenderer, and a good first
// file to read to see the basic bgfx resource lifecycle:
//   1. build vertices on the CPU (once, in the constructor)
//   2. describe their memory layout (bgfx::VertexLayout)
//   3. upload into a STATIC vertex buffer (createVertexBuffer + bgfx::copy)
//   4. every frame: setVertexBuffer + setState + submit
//   5. destroy the handles in the destructor, BEFORE bgfx::shutdown
//      (App's member order ensures renderers die before Graphics).
// Static versus transient buffers: the guide geometry never changes, so it
// lives in GPU memory permanently. The star instance data is rebuilt every
// frame, so StarRenderer uses transient buffers instead.
//
// Geometry is a "line list" (BGFX_STATE_PT_LINES): no index buffer and no
// thick lines. Line width is fixed at 1 px on most modern APIs; thick lines
// would need screen-space quads, which isn't worth it for faint guides.
// All lines are in parsecs in the galactic frame, so they share the stars'
// camera (view 0 has the same view/proj matrices as view 1).
// =============================================================================
#include "render/GuideRenderer.h"

#include "render/ShaderLoader.h"

#include <glm/gtc/constants.hpp>
#include <glm/trigonometric.hpp>

#include <cmath>
#include <cstdint>

namespace starmap::render {

namespace {
constexpr float kLyPerPc = 3.261563777f;

struct LineVertex {
    float x, y, z;
    uint32_t abgr;  // colour packed as bytes R,G,B,A in memory (little-endian 0xAABBGGRR)
};

uint32_t abgr(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (uint32_t{a} << 24) | (uint32_t{b} << 16) | (uint32_t{g} << 8) | uint32_t{r};
}
}  // namespace

GuideRenderer::GuideRenderer(const ShaderLoader& shaders) {
    std::vector<LineVertex> v;
    // PT_LINES = "line list": every PAIR of vertices is one independent segment.
    auto segment = [&v](glm::vec3 a, glm::vec3 b, uint32_t c) {
        v.push_back({a.x, a.y, a.z, c});
        v.push_back({b.x, b.y, b.z, c});
    };

    // Rings in the galactic plane. A circle of radius r: (r cos t, r sin t, 0).
    const float ring_ly[] = {10.f, 50.f, 100.f, 200.f};
    constexpr int kSegments = 128;
    for (float ly : ring_ly) {
        const float r = ly / kLyPerPc;
        const uint32_t c = abgr(90, 120, 170, ly == 200.f ? 110 : 80);
        for (int i = 0; i < kSegments; ++i) {
            const float t0 = glm::two_pi<float>() * static_cast<float>(i) / kSegments;
            const float t1 = glm::two_pi<float>() * static_cast<float>(i + 1) / kSegments;
            segment({r * std::cos(t0), r * std::sin(t0), 0.f}, {r * std::cos(t1), r * std::sin(t1), 0.f}, c);
        }
        // Label on the ring at galactic longitude l = -45 deg.
        const float l = glm::radians(-45.0f);
        labels_.push_back({{r * std::cos(l), r * std::sin(l), 0.f}, std::to_string(static_cast<int>(ly)) + " ly"});
    }

    // Longitude spokes every 30 deg from the 10 ly ring to the 200 ly ring.
    const float r0 = 10.f / kLyPerPc, r1 = 200.f / kLyPerPc;
    for (int deg = 0; deg < 360; deg += 30) {
        const float l = glm::radians(static_cast<float>(deg));
        const glm::vec3 dir(std::cos(l), std::sin(l), 0.f);
        const uint32_t c = deg == 0 ? abgr(255, 190, 90, 150)   // towards the Galactic centre: warm
                                    : abgr(90, 120, 170, 40);
        segment(dir * r0, dir * r1, c);
    }
    labels_.push_back({{r1 * 1.04f, 0.f, 0.f}, "-> Galactic centre (l=0)"});
    labels_.push_back({{0.f, r1 * 1.04f, 0.f}, "l=90 (direction of orbit)"});
    // North Galactic Pole tick.
    segment({0.f, 0.f, 0.f}, {0.f, 0.f, 20.f / kLyPerPc}, abgr(150, 220, 150, 120));
    labels_.push_back({{0.f, 0.f, 21.f / kLyPerPc}, "NGP"});

    // Vertex layout: position (3 floats) + colour (4 bytes, normalized to 0..1
    // in the shader because of the `true` "normalized" flag).
    bgfx::VertexLayout& layout = layout_;  // kept: submit_line() builds transient buffers with it
    layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .end();
    vertex_count_ = static_cast<uint32_t>(v.size());
    vb_ = bgfx::createVertexBuffer(bgfx::copy(v.data(), static_cast<uint32_t>(v.size() * sizeof(LineVertex))), layout);
    program_ = shaders.load_program("vs_line", "fs_line");
}

GuideRenderer::~GuideRenderer() {
    bgfx::destroy(program_);
    bgfx::destroy(vb_);
}

void GuideRenderer::submit(bgfx::ViewId view) const {
    bgfx::setVertexBuffer(0, vb_, 0, vertex_count_);
    // Lines use normal alpha blending (src*a + dst*(1-a)) so they can be faint.
    // They are drawn in an EARLIER view than the stars, so stars add on top.
    // BGFX_STATE_LINEAA would request anti-aliased lines where supported.
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_PT_LINES | BGFX_STATE_BLEND_ALPHA | BGFX_STATE_LINEAA);
    bgfx::submit(view, program_);
}

void GuideRenderer::submit_line(bgfx::ViewId view, const glm::vec3& a, const glm::vec3& b, uint32_t color) const {
    // A per-frame line (the Sol -> selection line changes whenever the
    // selection does), so a STATIC buffer like the rings' would be the wrong
    // tool: it would have to be destroyed and re-created on every change.
    // bgfx's TRANSIENT vertex buffers are made for this: memory from a ring
    // buffer that bgfx recycles after the frame, no create/destroy, no
    // handle to manage. The usual ritual:
    //   1. ask how many vertices fit this frame (the transient pool is finite,
    //      default 6 MB shared by everyone, including our ImGui backend);
    //   2. allocTransientVertexBuffer; 3. write into tvb.data; 4. set + submit.
    // The line is DASHED (kDashes on/off pairs) so it never gets mistaken
    // for one of the solid guide spokes.
    constexpr uint32_t kDashes = 40;
    constexpr uint32_t kVerts = kDashes * 2;  // one segment (2 vertices) per dash
    if (bgfx::getAvailTransientVertexBuffer(kVerts, layout_) < kVerts) return;  // pool exhausted: skip, don't crash
    bgfx::TransientVertexBuffer tvb;
    bgfx::allocTransientVertexBuffer(&tvb, kVerts, layout_);
    auto* out = reinterpret_cast<LineVertex*>(tvb.data);  // tvb.data is raw bytes laid out per layout_
    for (uint32_t i = 0; i < kDashes; ++i) {
        // Each dash covers the first 60 % of its 1/kDashes slice of a -> b.
        const float t0 = static_cast<float>(i) / static_cast<float>(kDashes);
        const float t1 = t0 + 0.6f / static_cast<float>(kDashes);
        const glm::vec3 p0 = a + (b - a) * t0, p1 = a + (b - a) * t1;
        out[2 * i] = LineVertex{p0.x, p0.y, p0.z, color};
        out[2 * i + 1] = LineVertex{p1.x, p1.y, p1.z, color};
    }
    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_PT_LINES | BGFX_STATE_BLEND_ALPHA | BGFX_STATE_LINEAA);
    bgfx::submit(view, program_);
}

uint32_t GuideRenderer::pack_abgr(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { return abgr(r, g, b, a); }

}  // namespace starmap::render
