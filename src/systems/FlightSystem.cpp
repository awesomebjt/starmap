// =============================================================================
// systems/FlightSystem.cpp — see FlightSystem.h
// =============================================================================
#include "systems/FlightSystem.h"

#include <entt/entity/registry.hpp>
#include <glm/common.hpp>

#include <algorithm>
#include <cmath>

namespace starmap::systems {

namespace {
scene::CameraFlight& flight(entt::registry& registry) {
    return registry.ctx().contains<scene::CameraFlight>() ? registry.ctx().get<scene::CameraFlight>()
                                                          : registry.ctx().emplace<scene::CameraFlight>();
}
}  // namespace

float ease_in_out_cubic(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    if (t < 0.5f) return 4.0f * t * t * t;
    const float u = -2.0f * t + 2.0f;
    return 1.0f - u * u * u * 0.5f;
}

scene::CameraPose pose_of(const scene::OrbitCamera& cam) {
    return {cam.target, cam.distance, cam.yaw_deg, cam.pitch_deg};
}

void apply_pose(scene::OrbitCamera& cam, const scene::CameraPose& pose) {
    cam.target = pose.target;
    cam.distance = std::clamp(pose.distance, scene::kMinDistancePc, scene::kMaxDistancePc);
    cam.yaw_deg = pose.yaw_deg;
    cam.pitch_deg = std::clamp(pose.pitch_deg, scene::kMinPitchDeg, scene::kMaxPitchDeg);
}

scene::CameraPose interpolate(const scene::CameraPose& a, const scene::CameraPose& b, float s, bool animate_angles) {
    scene::CameraPose p;
    p.target = glm::mix(a.target, b.target, s);  // mix(a, b, s) = a + (b - a) * s, component-wise
    // Geometric blend: exp(lerp(log a, log b, s)) = a * (b/a)^s.
    const float da = std::max(a.distance, 1e-6f), db = std::max(b.distance, 1e-6f);
    p.distance = da * std::pow(db / da, s);
    if (animate_angles) {
        // std::remainder(x, 360) maps the difference into [-180, 180]: the short way round.
        const float dyaw = std::remainder(b.yaw_deg - a.yaw_deg, 360.0f);
        p.yaw_deg = std::remainder(a.yaw_deg + dyaw * s, 360.0f);
        p.pitch_deg = a.pitch_deg + (b.pitch_deg - a.pitch_deg) * s;  // pitch is clamped to +-89: no wrap
    } else {
        p.yaw_deg = a.yaw_deg;  // caller overwrites angles with the live ones (see update_flight)
        p.pitch_deg = a.pitch_deg;
    }
    return p;
}

float framing_distance(float current) { return std::min(current, scene::kFocusDistancePc); }

void fly_to(entt::registry& registry, const glm::vec3& target, std::optional<float> distance, float duration_s) {
    auto& cam = registry.ctx().get<scene::OrbitCamera>();
    scene::CameraPose to = pose_of(cam);
    to.target = target;
    to.distance = distance.value_or(framing_distance(cam.distance));
    auto& f = flight(registry);
    if (duration_s <= 0.0f) {
        f.active = false;
        apply_pose(cam, to);
        return;
    }
    // Start from wherever the camera is NOW — mid-flight included — so a
    // retarget never jumps.
    f = scene::CameraFlight{true, false, 0.0f, duration_s, pose_of(cam), to};
}

void fly_to_pose(entt::registry& registry, const scene::CameraPose& pose, float duration_s) {
    auto& cam = registry.ctx().get<scene::OrbitCamera>();
    auto& f = flight(registry);
    if (duration_s <= 0.0f) {
        f.active = false;
        apply_pose(cam, pose);
        return;
    }
    f = scene::CameraFlight{true, true, 0.0f, duration_s, pose_of(cam), pose};
}

void cancel_flight(entt::registry& registry) { flight(registry).active = false; }

void cancel_angle_flight(entt::registry& registry) {
    auto& f = flight(registry);
    if (f.animate_angles) f.active = false;
}

bool flight_active(const entt::registry& registry) {
    const auto* f = registry.ctx().find<scene::CameraFlight>();
    return f && f->active;
}

bool update_flight(entt::registry& registry, float dt_s) {
    auto& f = flight(registry);
    if (!f.active) return false;
    auto& cam = registry.ctx().get<scene::OrbitCamera>();
    // Clamp dt: after a stall (window dragged, debugger breakpoint) a huge dt
    // would just end the flight, which is fine; a NEGATIVE dt (clock glitch)
    // must never run it backwards.
    f.elapsed_s += std::max(dt_s, 0.0f);
    const float t = f.duration_s > 0.0f ? f.elapsed_s / f.duration_s : 1.0f;
    scene::CameraPose p = interpolate(f.from, f.to, ease_in_out_cubic(t), f.animate_angles);
    if (!f.animate_angles) {
        // Keep the LIVE yaw/pitch: the user may be orbiting during the flight.
        p.yaw_deg = cam.yaw_deg;
        p.pitch_deg = cam.pitch_deg;
    }
    apply_pose(cam, p);
    if (t >= 1.0f) f.active = false;  // landed exactly on `to` (ease(1) == 1)
    return true;
}

}  // namespace starmap::systems
