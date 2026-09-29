#pragma once
// =============================================================================
// scene/OrbitCamera.h — the camera state that lives in registry.ctx().
//
// ROLE IN THE ARCHITECTURE
//   There is exactly one camera, and it is not "a thing in the world" that
//   other systems iterate over, so it is NOT an entity with components. It is a
//   registry *context* variable:  registry.ctx().emplace<OrbitCamera>();
//   - InputHandler      writes the user-controlled part (target, distance, yaw, pitch)
//   - CameraSystem      derives eye position + view/projection matrices once per frame
//   - StarRenderSystem, GuideRenderer, HudSystem read the derived matrices.
//   Keeping inputs and derived data in one struct makes the data flow easy to
//   follow: input -> CameraSystem::update() -> renderers.
//
// COORDINATE CONVENTION (important!)
//   World space = the catalog's GALACTIC Cartesian frame, in parsecs, Sun at the origin:
//     +x  towards the Galactic centre (in Sagittarius), l = 0, b = 0
//     +y  towards l = 90 deg (the direction the Sun orbits the Galaxy), b = 0
//     +z  towards the North Galactic Pole (b = +90 deg)
//   This is a RIGHT-handed frame (x cross y = z), and the galactic plane is z = 0.
//   We therefore use a **Z-up camera**: yaw rotates around +z (i.e. around the
//   galactic pole, "longitude" of the viewpoint) and pitch lifts the eye above
//   or below the galactic plane ("latitude"). No axis swapping is needed —
//   the data are rendered in exactly the frame they are stored in.
//   (Many engines are Y-up; converting would mean swapping y/z in the loader or
//   a rotation in the view matrix, and every later piece of astro maths would
//   have to remember that. Z-up keeps "z = height above the galactic plane".)
//
// SPHERICAL COORDINATES OF THE EYE (orbit camera)
//   eye = target + distance * ( cos(pitch) * cos(yaw),
//                               cos(pitch) * sin(yaw),
//                               sin(pitch) )
//   yaw   = angle in the xy (galactic) plane measured from +x towards +y
//   pitch = elevation above the plane, clamped to (-89, +89) degrees because at
//           exactly +-90 the view direction becomes parallel to the up vector
//           (0,0,1) and "look at" can no longer tell which way is up
//           (the cross product that builds the camera's right vector is zero).
// =============================================================================

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace starmap::scene {

struct OrbitCamera {
    // ---- user-controlled state (InputHandler / command line) -----------------
    glm::vec3 target{0.0f};     // point the camera orbits around and looks at (pc)
    float distance = 150.0f;    // eye-to-target distance (pc); 150 pc frames the whole 61 pc sphere
    float yaw_deg = -50.0f;     // around +z (galactic pole)
    float pitch_deg = 28.0f;    // above the galactic plane
    float fov_y_deg = 50.0f;    // vertical field of view of the perspective projection

    // ---- derived every frame by CameraSystem::update() ------------------------
    glm::vec3 eye{0.0f};        // world position of the camera
    glm::mat4 view{1.0f};       // world -> view (eye) space
    glm::mat4 proj{1.0f};       // view -> clip space
    glm::mat4 view_proj{1.0f};  // proj * view (world -> clip), handy for CPU-side projection
    float near_pc = 0.01f;      // clip planes chosen from `distance` (see CameraSystem.cpp)
    float far_pc = 1000.0f;
    int viewport_w = 1;         // back-buffer size in PIXELS (not window points: HiDPI!)
    int viewport_h = 1;

    // The "home" pose restored by the R key.
    static OrbitCamera home() { return OrbitCamera{}; }
};

// Limits used by CameraSystem and InputHandler.
inline constexpr float kMinPitchDeg = -89.0f;
inline constexpr float kMaxPitchDeg = 89.0f;
inline constexpr float kMinDistancePc = 0.05f;   // ~10,000 AU: close enough to separate binaries
inline constexpr float kMaxDistancePc = 2000.0f;

}  // namespace starmap::scene
