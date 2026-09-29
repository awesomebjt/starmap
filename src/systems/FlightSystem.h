#pragma once
// =============================================================================
// systems/FlightSystem.h — smooth, frame-rate independent camera flights
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   Anything that wants to move the camera "nicely" (clicking a star, search,
//   F / H / R keys) calls fly_to_*(). That only WRITES a CameraFlight into
//   ctx; update_flight(dt) then advances it once per frame, before
//   CameraSystem::update() turns the orbit parameters into matrices.
//
// FRAME-RATE INDEPENDENCE
//   Progress is measured in SECONDS (elapsed += dt), never in frames. At 144
//   fps the flight takes the same 0.8 s as at 20 fps under llvmpipe; only the
//   number of in-between images differs. The eased curve is evaluated from
//   the normalised time t = elapsed / duration directly (not integrated step
//   by step), so a long frame just jumps further along the same curve instead
//   of accumulating error — and a flight always ends exactly on its target.
//
// EASING
//   ease_in_out_cubic(t): starts at zero velocity, accelerates, decelerates
//   to zero velocity at the end (a smooth "S"). Linear motion starts and
//   stops with a jerk the eye reads as mechanical; ease-in-out mimics how a
//   physical camera operator would move.
//       t < 0.5 : 4 t^3                (accelerating)
//       t >= 0.5: 1 - (-2t + 2)^3 / 2  (mirror image, decelerating)
//
// INTERRUPTIONS ("retarget or cancel cleanly")
//   * A new fly_to during a flight RETARGETS: the new flight starts from the
//     camera's current (mid-air) pose, so there is never a jump.
//   * Zoom (wheel) or pan cancel the flight: they edit the same parameters
//     and the user's hand wins. The camera simply stays where it is.
//   * Orbiting (yaw/pitch) does not touch target/distance, so it coexists
//     with an ordinary flight; it cancels only flights that animate angles
//     (the R "reset view" flight).
// =============================================================================

#include "scene/CameraFlight.h"
#include "scene/OrbitCamera.h"

#include <entt/entity/fwd.hpp>
#include <glm/vec3.hpp>

#include <optional>

namespace starmap::systems {

[[nodiscard]] float ease_in_out_cubic(float t);

[[nodiscard]] scene::CameraPose pose_of(const scene::OrbitCamera& cam);
void apply_pose(scene::OrbitCamera& cam, const scene::CameraPose& pose);

// Blend two poses at eased parameter s in [0,1]: target linearly, distance
// geometrically (log space), yaw along the SHORTER arc (from 170 deg to -170 deg
// goes through 180, 20 deg, not back through 0, 340 deg) when animate_angles.
[[nodiscard]] scene::CameraPose interpolate(const scene::CameraPose& a, const scene::CameraPose& b, float s,
                                            bool animate_angles);

// Distance to use when focusing a star from `current` distance: zoom in to
// kFocusDistancePc, never zoom out.
[[nodiscard]] float framing_distance(float current);

// Start a flight to `target` (distance: given, or framing_distance(current)).
// duration_s <= 0 applies the end pose immediately (used by --select).
void fly_to(entt::registry& registry, const glm::vec3& target, std::optional<float> distance = std::nullopt,
            float duration_s = scene::kFlightSeconds);
// Start a flight to a full pose, angles included (R = reset view).
void fly_to_pose(entt::registry& registry, const scene::CameraPose& pose, float duration_s = scene::kFlightSeconds);
void cancel_flight(entt::registry& registry);
[[nodiscard]] bool flight_active(const entt::registry& registry);
// Only cancel flights that animate yaw/pitch (called when the user orbits).
void cancel_angle_flight(entt::registry& registry);

// Advance by dt seconds; returns true while a flight is in progress.
bool update_flight(entt::registry& registry, float dt_s);

}  // namespace starmap::systems
