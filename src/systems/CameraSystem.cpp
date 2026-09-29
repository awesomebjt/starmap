// =============================================================================
// systems/CameraSystem.cpp — orbit camera maths.
//
// MATRIX STORAGE NOTE (bx vs glm)
//   bx (bgfx's maths library) and glm store 4x4 matrices with the same memory
//   layout: 16 floats where elements 12,13,14 are the translation. glm calls
//   this "column-major, column vectors" (clip = P * V * p); bx describes the
//   same bytes as "row-major, row vectors" (clip = p * V * P). Same numbers,
//   transposed notation. So we can let bx::mtxLookAt / bx::mtxProj write
//   straight into a glm::mat4 via glm::value_ptr(), then use glm operators
//   (proj * view) on the CPU, and hand the same pointer to
//   bgfx::setViewTransform(). bgfx's shader mul() uses the matching convention.
//
// HANDEDNESS PITFALL
//   bx defaults to a LEFT-handed convention (camera looks down +z). Our data
//   are in a RIGHT-handed frame (galactic x,y,z). Rendering right-handed data
//   with a left-handed view/projection pair doesn't crash — it silently
//   MIRRORS the sky (constellations appear flipped). We therefore pass
//   bx::Handedness::Right to BOTH mtxLookAt and mtxProj; the camera then looks
//   down -z in view space, like OpenGL/glm. A unit test
//   (tests/test_app_core.cpp) checks that +x appears on the right when looking
//   down from the galactic north pole with +y up.
// =============================================================================
#include "systems/CameraSystem.h"

#include "catalog/Indexes.h"
#include "ecs/Components.h"

#include <bx/math.h>
#include <entt/entity/registry.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>

namespace starmap::systems {

using scene::OrbitCamera;

namespace {
constexpr float kOrbitDegPerPixel = 0.3f;  // mouse sensitivity for rotation
constexpr float kZoomPerStep = 0.12f;      // distance *= exp(-0.12) per wheel notch (~11 %)
}  // namespace

glm::vec3 eye_position(const OrbitCamera& cam) {
    const float yaw = glm::radians(cam.yaw_deg);
    const float pitch = glm::radians(cam.pitch_deg);
    const glm::vec3 dir(std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch));
    return cam.target + cam.distance * dir;
}

void orbit(OrbitCamera& cam, float dx_px, float dy_px) {
    // Dragging right should rotate the scene to the right, i.e. move the EYE to
    // the left around the target => decrease yaw.
    cam.yaw_deg -= dx_px * kOrbitDegPerPixel;
    cam.yaw_deg = std::remainder(cam.yaw_deg, 360.0f);  // keep it in (-180, 180]; avoids float drift after hours of spinning
    // Dragging down lifts the eye (like grabbing the sphere and pulling it down).
    cam.pitch_deg = std::clamp(cam.pitch_deg + dy_px * kOrbitDegPerPixel, scene::kMinPitchDeg, scene::kMaxPitchDeg);
}

void zoom(OrbitCamera& cam, float wheel_steps) {
    cam.distance = std::clamp(cam.distance * std::exp(-kZoomPerStep * wheel_steps),
                              scene::kMinDistancePc, scene::kMaxDistancePc);
}

void pan(OrbitCamera& cam, float dx_px, float dy_px) {
    // World units per pixel at the target's depth: the visible height of the
    // frustum at distance d is 2 * d * tan(fov/2), spread over viewport_h pixels.
    const float units_per_px = 2.0f * cam.distance * std::tan(glm::radians(cam.fov_y_deg) * 0.5f) /
                               static_cast<float>(std::max(cam.viewport_h, 1));
    // Camera basis in world space: forward points from eye to target, right =
    // forward x up, and the true "screen up" = right x forward.
    const glm::vec3 forward = glm::normalize(cam.target - eye_position(cam));
    const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0, 0, 1)));
    const glm::vec3 up = glm::cross(right, forward);
    // Drag right => the world should move right => target moves LEFT. Screen y grows downwards.
    cam.target += (-dx_px * right + dy_px * up) * units_per_px;
}

void focus(OrbitCamera& cam, const glm::vec3& p) { cam.target = p; }

void reset(OrbitCamera& cam) {
    const float fov = cam.fov_y_deg;  // keep the user's FOV choice
    cam = OrbitCamera::home();
    cam.fov_y_deg = fov;
}

void update(OrbitCamera& cam, int width, int height, bool homogeneous_depth) {
    cam.viewport_w = std::max(width, 1);
    cam.viewport_h = std::max(height, 1);
    cam.eye = eye_position(cam);

    // NEAR / FAR PLANES. Perspective depth precision is roughly proportional to
    // far/near, and anything closer than `near` is clipped. We don't use the
    // depth buffer for stars (additive blending, see StarRenderer), so precision
    // is irrelevant — but clipping is not: stars between the eye and `near`
    // would disappear. Scaling near with the orbit distance keeps close-up views
    // (0.05 pc) working while the far plane always contains the whole catalogue
    // sphere (radius ~61 pc around Sol) behind the target.
    cam.near_pc = std::max(cam.distance * 0.001f, 1.0e-4f);
    cam.far_pc = cam.distance + glm::length(cam.target) + 200.0f;

    // VIEW MATRIX: bx::mtxLookAt builds the rigid transform that moves the eye to
    // the origin and rotates `at - eye` onto the viewing axis. The up vector
    // (0,0,1) is the galactic pole: Z-up, see OrbitCamera.h.
    bx::mtxLookAt(glm::value_ptr(cam.view),
                  bx::Vec3(cam.eye.x, cam.eye.y, cam.eye.z),
                  bx::Vec3(cam.target.x, cam.target.y, cam.target.z),
                  bx::Vec3(0.0f, 0.0f, 1.0f),
                  bx::Handedness::Right);

    // PROJECTION MATRIX: a symmetric perspective frustum. fovy is in DEGREES
    // for bx::mtxProj. Aspect = width / height in pixels, so a resize keeps
    // circles round. See CameraSystem.h for `homogeneous_depth`.
    const float aspect = static_cast<float>(cam.viewport_w) / static_cast<float>(cam.viewport_h);
    bx::mtxProj(glm::value_ptr(cam.proj), cam.fov_y_deg, aspect, cam.near_pc, cam.far_pc,
                homogeneous_depth, bx::Handedness::Right);

    cam.view_proj = cam.proj * cam.view;  // glm: apply view first, then proj
}

void update(entt::registry& registry, int width, int height, bool homogeneous_depth) {
    // ctx().get<T>() returns a reference to the context variable; it asserts
    // (debug builds) if nobody emplaced one — the app does so at startup.
    update(registry.ctx().get<OrbitCamera>(), width, height, homogeneous_depth);
}

std::optional<glm::vec2> world_to_screen(const OrbitCamera& cam, const glm::vec3& p) {
    const glm::vec4 clip = cam.view_proj * glm::vec4(p, 1.0f);
    if (clip.w <= 1.0e-6f) return std::nullopt;         // behind the eye
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;       // perspective divide
    // NDC x,y in [-1,1] with +y UP; window pixels have +y DOWN.
    return glm::vec2((ndc.x * 0.5f + 0.5f) * static_cast<float>(cam.viewport_w),
                     (0.5f - ndc.y * 0.5f) * static_cast<float>(cam.viewport_h));
}

std::optional<glm::vec3> star_position(const entt::registry& registry, std::string_view name) {
    // ctx().find<T>() returns a pointer (nullptr if absent) - the NameIndex is
    // optional (LoadOptions::load_names), so don't assume it exists.
    const auto* names = registry.ctx().find<catalog::NameIndex>();
    if (!names) return std::nullopt;
    const auto e = names->find(name);
    if (!e) return std::nullopt;
    // try_get<T>() returns nullptr instead of asserting when the component is missing.
    if (const auto* pos = registry.try_get<ecs::Position>(*e)) return pos->galactic_pc;
    return std::nullopt;
}

}  // namespace starmap::systems
