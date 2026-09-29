#pragma once
// =============================================================================
// render/GuideRenderer.h — orientation aids drawn as lines:
//   * distance rings of 10, 50, 100 and 200 light years around Sol, lying in
//     the galactic plane (z = 0),
//   * faint "spokes" every 30 degrees of galactic longitude,
//   * a brighter spoke towards the Galactic centre (+x) and a short tick
//     towards the North Galactic Pole (+z).
// Their text labels ("50 ly", "Galactic centre") are drawn by HudSystem with
// Dear ImGui's background draw list at the projected positions returned by
// labels().
//
// Cheap: ~1,100 line vertices in a STATIC vertex buffer created once; a single
// draw call per frame.
// =============================================================================

#include <bgfx/bgfx.h>
#include <glm/vec3.hpp>

#include <string>
#include <vector>

namespace starmap::render {

class ShaderLoader;

struct GuideLabel {
    glm::vec3 world;   // anchor point (pc)
    std::string text;
};

class GuideRenderer {
public:
    explicit GuideRenderer(const ShaderLoader& shaders);
    ~GuideRenderer();
    GuideRenderer(const GuideRenderer&) = delete;
    GuideRenderer& operator=(const GuideRenderer&) = delete;

    void submit(bgfx::ViewId view) const;
    [[nodiscard]] const std::vector<GuideLabel>& labels() const noexcept { return labels_; }

    // Milestone 3: one extra dashed line from a to b this frame (the Sol ->
    // selection line), built in a transient vertex buffer. `color` is packed
    // as pack_abgr() returns it.
    void submit_line(bgfx::ViewId view, const glm::vec3& a, const glm::vec3& b, uint32_t color) const;
    [[nodiscard]] static uint32_t pack_abgr(uint8_t r, uint8_t g, uint8_t b, uint8_t a);

private:
    bgfx::VertexBufferHandle vb_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle program_ = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout layout_;  // position + colour; needed again for transient buffers
    uint32_t vertex_count_ = 0;
    std::vector<GuideLabel> labels_;
};

}  // namespace starmap::render
