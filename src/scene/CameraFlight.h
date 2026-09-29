#pragma once
// =============================================================================
// scene/CameraFlight.h — state of an animated camera move ("fly-to")
// =============================================================================
//
// A flight blends the orbit camera's PARAMETERS — target point, distance and
// (optionally) yaw/pitch — from a start pose to an end pose over a fixed
// duration. Why parameters and not matrices?
//   * Interpolating two view matrices element-wise does not give a valid
//     camera in between: the rotation part stops being orthonormal (it
//     shears and shrinks), so the scene visibly squashes mid-flight. You'd
//     have to decompose into translation + quaternion and slerp — and even
//     then the camera would not ORBIT, it would cut a straight line.
//   * The orbit parameters are exactly the quantities the user thinks in
//     (what am I looking at, how far away, from which side), every blend of
//     them is a valid orbit camera, and CameraSystem::update() turns them
//     into matrices every frame anyway.
//   Distance is blended in LOG space (geometric interpolation): going from
//   150 pc to 8 pc, a linear blend would spend most of the flight far away
//   and then crash in during the last frames; equal steps in log(distance)
//   look like a constant-speed zoom, the same reason zooming is exponential.
// =============================================================================

#include <glm/vec3.hpp>

namespace starmap::scene {

struct CameraPose {
    glm::vec3 target{0.0f};
    float distance = 150.0f;
    float yaw_deg = -50.0f;
    float pitch_deg = 28.0f;
};

struct CameraFlight {
    bool active = false;
    bool animate_angles = false;  // false: only target+distance move, the user keeps orbiting freely
    float elapsed_s = 0.0f;
    float duration_s = 0.8f;
    CameraPose from, to;
};

// Default flight length: long enough for the eye to follow the motion (and so
// keep its bearings), short enough not to feel like waiting.
inline constexpr float kFlightSeconds = 0.8f;
// Framing distance when focusing a star: close enough that its neighbourhood
// (a few light years around it) fills the view. Only ever zooms IN to this;
// if the user is already closer, their distance is kept.
inline constexpr float kFocusDistancePc = 8.0f;

}  // namespace starmap::scene
