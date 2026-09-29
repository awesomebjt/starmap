#pragma once
// =============================================================================
// input/InputHandler.h — translates SDL events into changes of ctx state.
//
// ROLE IN THE ARCHITECTURE
//   SDL_PollEvent (App::run) --SDL_Event--> InputHandler::handle()
//       -> OrbitCamera (orbit/zoom/pan/focus/reset via CameraSystem helpers)
//       -> ColorSettings (keys 1-6; sets the dirty flag)
//       -> ViewSettings (G, H, P/Tab, F12, +/-, Esc)
//   Dear ImGui sees every event FIRST (ImGuiLayer::process_event in App::run).
//   ImGui then tells us whether it wants the mouse / keyboard, and we pass
//   those answers in here so a slider drag on the filter panel does not also
//   orbit the camera, and typing "5" into a number field does not switch to
//   colour scheme 5.
//   The handler never renders and never iterates entities; it only mutates
//   context variables. Systems pick the changes up on the same frame. This
//   "input writes state, systems read state" split makes it trivial to add
//   other input sources later (gamepad, Dear ImGui buttons, scripted tours):
//   they call the same helpers.
// =============================================================================

#include <entt/entity/fwd.hpp>

union SDL_Event;  // SDL3's event type is a union; forward-declare to keep SDL out of this header

namespace starmap::input {

// Which input devices Dear ImGui has claimed this frame
// (io.WantCaptureMouse / io.WantCaptureKeyboard). A namespace-level struct
// rather than a nested one: a nested struct with default member initialisers
// cannot be used as a default argument ("= {}") inside its own enclosing
// class, because the class is still incomplete at that point.
struct UiCapture {
    bool mouse = false;     // pointer is over / dragging an ImGui window
    bool keyboard = false;  // a text/number field has keyboard focus
};

class InputHandler {
public:
    // `pixel_density` = pixels per point (Window::pixel_density()). SDL reports
    // mouse motion in POINTS; panning must follow the cursor in PIXELS.
    void handle(const SDL_Event& event, entt::registry& registry, float pixel_density, UiCapture ui = {});

private:
    // Click-vs-drag tracking. The left button is orbit-or-select; the right
    // button is pan-or-tag. A click can only be recognised by remembering
    // where and when the press happened. Middle button has no click, so it
    // pans on the first motion and needs no memory.
    bool left_down_ = false;    // press started over the 3D view (not over ImGui)
    bool dragging_ = false;     // moved beyond the click slop since the press
    float press_pt_x_ = 0.0f;   // press position in POINTS (SDL's mouse unit)
    float press_pt_y_ = 0.0f;
    unsigned long long press_ms_ = 0;  // press time (ms, from the event timestamp)
    bool right_down_ = false;
    bool right_dragging_ = false;
    float right_press_pt_x_ = 0.0f;
    float right_press_pt_y_ = 0.0f;
    unsigned long long right_press_ms_ = 0;
};

}  // namespace starmap::input
