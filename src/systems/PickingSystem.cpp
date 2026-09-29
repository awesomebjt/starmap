// =============================================================================
// systems/PickingSystem.cpp — see PickingSystem.h for the approach
// =============================================================================
#include "systems/PickingSystem.h"

#include "ecs/Components.h"
#include "scene/Filters.h"
#include "scene/RenderComponents.h"

#include <entt/entity/registry.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace starmap::systems {

float focal_length_px(const scene::OrbitCamera& cam) {
    return 0.5f * static_cast<float>(cam.viewport_h) / std::tan(glm::radians(cam.fov_y_deg) * 0.5f);
}

float billboard_radius_px(float radius_pc, float depth, float focal_px, float intensity, bool marked,
                          const PickParams& p) {
    // Line for line the same as vs_star.sc, so what you click is what you see:
    //   truePx   = radiusPc * focal / depth
    //   emphasis = max(1, sqrt(intensity))
    //   minPx    = (marked ? marker_min : min) * emphasis
    //   px       = clamp(truePx, minPx, max(maxPx, minPx))
    const float true_px = radius_pc * focal_px / std::max(depth, 1.0e-4f);
    const float emphasis = std::max(1.0f, std::sqrt(std::max(intensity, 0.0f)));
    const float min_px = (marked ? p.marker_min_px : p.min_px) * emphasis;
    return std::clamp(true_px, min_px, std::max(p.max_px, min_px));
}

float visibility(float true_px, float drawn_px, float intensity, const PickParams& p) {
    // Same three factors as the shader's colour: the scheme's intensity
    // (capped), the "dim" factor for stars drawn larger than their true size
    // (floored at min_intensity) and the global exposure.
    const float ratio = std::clamp(true_px / std::max(drawn_px, 1.0e-6f), 0.0f, 1.0f);
    const float dim = std::max(std::pow(ratio, p.dim_exponent), p.min_intensity);
    return std::min(intensity, p.max_intensity) * dim * p.exposure;
}

float hit_radius_px(float billboard_px, const PickParams& p) {
    return std::max(billboard_px * p.glow_fraction, p.min_hit_px) + p.slack_px;
}

float hit_radius_px(float billboard_px, float star_visibility, const PickParams& p) {
    return star_visibility < p.faint_visibility ? p.faint_hit_px : hit_radius_px(billboard_px, p);
}

PickResult pick_star(const entt::registry& registry, const scene::OrbitCamera& cam, const glm::vec2& cursor_px,
                     const PickParams& params) {
    const auto t0 = std::chrono::steady_clock::now();
    PickResult best;
    float best_d2 = 0.0f, best_depth = 0.0f, best_score = 0.0f;
    const float focal = focal_length_px(cam);
    const float w = static_cast<float>(cam.viewport_w), h = static_cast<float>(cam.viewport_h);
    // The largest hit radius any star can have: lets us reject most stars
    // with two comparisons before doing any square roots.
    const float max_hit = hit_radius_px(params.max_px * 2.0f, params);
    // A click outside the 3D viewport (e.g. on the panel strip) picks
    // nothing. Normally ImGui has already claimed such clicks, but --pick
    // and tests call us directly.
    if (cursor_px.x < 0.0f || cursor_px.y < 0.0f || cursor_px.x >= w || cursor_px.y >= h) {
        best.elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return best;
    }

    // Same view (minus DisplayColor's colour, plus its intensity) as the
    // renderer: filtered-out stars are invisible, so they are not pickable.
    const auto stars = registry.view<const ecs::Position, const scene::DisplaySize, const ecs::DisplayColor>(
        entt::exclude<scene::FilteredOut>);
    // On a CONST registry, storage<T>() returns a pointer that is null if no
    // entity ever had a T (a const registry cannot create the pool on demand).
    const auto* marked = registry.storage<scene::Marked>();
    for (auto [e, pos, size, color] : stars.each()) {
        ++best.tested;
        // world -> clip: the same multiplication the GPU does with u_viewProj.
        const glm::vec4 clip = cam.view_proj * glm::vec4(pos.galactic_pc, 1.0f);
        if (clip.w <= 1.0e-6f) continue;  // behind the camera (w = depth along the view axis)
        // Perspective divide -> NDC, then NDC -> pixels (y flipped: NDC up, pixels down).
        const float inv_w = 1.0f / clip.w;
        const float sx = (clip.x * inv_w * 0.5f + 0.5f) * w;
        const float sy = (0.5f - clip.y * inv_w * 0.5f) * h;
        const float dx = sx - cursor_px.x, dy = sy - cursor_px.y;
        if (std::fabs(dx) > max_hit || std::fabs(dy) > max_hit) continue;  // cheap early out
        if (sx < 0.0f || sy < 0.0f || sx >= w || sy >= h) continue;      // centre outside the viewport: not visible
        const float intensity = color.rgba.a;
        const bool is_marked = marked && marked->contains(e);
        const float px = billboard_radius_px(size.radius_pc, clip.w, focal, intensity, is_marked, params);
        const float true_px = size.radius_pc * focal / std::max(clip.w, 1.0e-4f);
        // Marked stars (Sol) carry a ring and are always "visible" targets.
        const float vis = is_marked ? 1.0f : visibility(true_px, px, intensity, params);
        const float hit = hit_radius_px(px, vis, params);
        const float d2 = dx * dx + dy * dy;
        if (d2 > hit * hit) continue;
        // Smallest normalised distance (d / hit)^2 wins; on an exact tie the
        // star nearer the camera.
        const float score = d2 / (hit * hit);
        if (best.entity == entt::null || score < best_score || (score == best_score && clip.w < best_depth)) {
            best.entity = e;
            best_score = score;
            best_d2 = d2;
            best_depth = clip.w;
            best.hit_radius_px = hit;
        }
    }
    best.screen_dist_px = std::sqrt(best_d2);
    best.elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return best;
}

}  // namespace starmap::systems
