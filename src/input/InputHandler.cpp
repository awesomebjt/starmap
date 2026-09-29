// =============================================================================
// input/InputHandler.cpp — SDL3 events -> changes to registry ctx state
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   App::run drains the SDL event queue at the top of every frame and hands
//   each event here. The handler never draws and never touches the GPU. It
//   only edits plain data in registry.ctx():
//     OrbitCamera    (orbit / pan / zoom / focus / reset)
//     ColorSettings  (scheme + dirty flag -> ColorSystem recolours next update)
//     ViewSettings   (guides, help, panel, exposure, quit, focus_search)
//     PointerInput   (milestone 3: "a click happened at pixel (x,y)" and the
//                     hover position; InteractionSystem turns those into a
//                     pick + selection + fly-to AFTER all events are drained)
//     CameraFlight   (via FlightSystem: F / H / R start flights; wheel, pan
//                     and orbit cancel them so the user's hand always wins)
//   Keeping input -> state -> systems one-directional means the same state
//   could be driven by a script, a UI button or a network message with no
//   extra code — and indeed the ImGui filter panel's scheme combo box calls
//   the very same ColorSettings::set() as the 1-6 keys.
//
// SHARING INPUT WITH DEAR IMGUI
//   Every SDL event is first given to ImGui (ImGui_ImplSDL3_ProcessEvent via
//   ImGuiLayer). ImGui never "consumes" events — SDL has no such concept —
//   instead it publishes two flags each frame:
//     io.WantCaptureMouse    : the pointer is over an ImGui window, or a drag
//                              that STARTED on one is in progress;
//     io.WantCaptureKeyboard : an ImGui widget has keyboard focus (e.g. the
//                              min/max number fields).
//   The app is expected to honour them, which is what UiCapture is for:
//     * mouse captured  -> ignore motion/wheel/buttons (no orbit while
//       dragging a slider handle, no zoom while scrolling the panel);
//     * keyboard captured -> ignore our hotkeys (typing "5000" into the
//       min-temperature field must not switch colour scheme 5 and then 1...).
//   Because a drag that started over the 3D view keeps WantCaptureMouse
//   false until the button is released, orbiting that sweeps across the
//   panel keeps working; the reverse also holds. The flags come from the
//   previous ImGui::NewFrame() (events are processed before the new frame
//   starts), a one-frame lag that is imperceptible in practice.
//
// EVENTS VS POLLING
//   SDL offers both: events (discrete: "key R went down") and state queries
//   (SDL_GetKeyboardState / SDL_GetMouseState: "is W held right now?").
//   Toggles and clicks want events, so nothing is missed between frames.
//   Continuous movement such as WASD flying would use per-frame polling
//   scaled by frame time. A drag uses motion events, which carry the button
//   state along with them.
//
// CLICK VERSUS DRAG (milestone 3)
//   Left button does two jobs: drag = orbit, click = pick a star. We can't
//   know which one the user means at press time, so the press is only
//   RECORDED (position + timestamp). Motion beyond kClickSlopPt turns it into
//   a drag (and the whole accumulated movement is then applied to the orbit,
//   so nothing is lost); a release that never became a drag and came within
//   kClickMaxMs is a click. The pick itself is NOT done here: the handler has
//   no business knowing about projection matrices or star sizes. It just
//   leaves PointerInput::click_px for InteractionSystem, which runs once per
//   frame after the event loop.
//   Why not pick on press? Because then every orbit drag would also select
//   whatever star happened to be under the cursor when the drag began.
//
//   The right button is the same gesture with the other outcome: a drag pans
//   (as it did before tagging existed), a click reports right_click_px. The
//   thresholds are shared on purpose. A different slop for the right button
//   would make one button feel "stickier" than the other. The middle button
//   still pans on the first pixel of motion: it has no click to protect.
//
// UNITS
//   Mouse coordinates and deltas are in POINTS. Orbiting uses degrees per
//   point, so it feels the same on any display. Panning must move the target
//   so the scene follows the cursor, and CameraSystem::pan works in pixels
//   (it knows the viewport in pixels), so the deltas are scaled by
//   pixel_density.
// =============================================================================
#include "input/InputHandler.h"

#include "scene/ColorScheme.h"
#include "scene/OrbitCamera.h"
#include "scene/Tags.h"
#include "scene/ViewSettings.h"
#include "scene/Selection.h"
#include "systems/CameraSystem.h"
#include "systems/FlightSystem.h"
#include "systems/InteractionSystem.h"
#include "systems/SelectionSystem.h"

#include <SDL3/SDL.h>
#include <entt/entity/registry.hpp>

#include <algorithm>
#include <cstdint>

namespace starmap::input {

namespace {
// Click-vs-drag thresholds. A "click" is a press and release that moved less
// than kClickSlopPt points and took less than kClickMaxMs. Anything else is a
// drag (orbit). Without the movement threshold every tiny hand tremor during
// a click would rotate the view a little; without the time limit a slow
// "press, think, release" would pick a star the user never meant to pick.
constexpr float kClickSlopPt = 4.0f;
constexpr std::uint64_t kClickMaxMs = 500;

template <typename T>
T& ctx_get_or_emplace(entt::registry& registry) {
    return registry.ctx().contains<T>() ? registry.ctx().get<T>() : registry.ctx().emplace<T>();
}
}  // namespace

void InputHandler::handle(const SDL_Event& event, entt::registry& registry, float pixel_density, UiCapture ui) {
    auto& cam = registry.ctx().get<scene::OrbitCamera>();
    auto& view = registry.ctx().get<scene::ViewSettings>();
    auto& pointer = ctx_get_or_emplace<scene::PointerInput>(registry);

    // SDL_Event is a union; event.type says which member is valid
    // (event.motion for mouse motion, event.key for keys, ...).
    switch (event.type) {
        case SDL_EVENT_MOUSE_MOTION: {
            // `state` is a bit mask of the buttons held DURING this motion.
            // xrel/yrel: movement since the last motion event, in points (floats in SDL3).
            const SDL_MouseButtonFlags buttons = event.motion.state;
            if (ui.mouse && !left_down_ && !right_down_) {
                // ImGui owns the pointer (slider drag, panel hover, tag menu):
                // no orbit, and no hover tooltip for stars hidden under the panel.
                // A drag that STARTED on the 3D view keeps going when it crosses
                // the panel, which is why left_down_ / right_down_ bypass this.
                if (pointer.hover_px) pointer.hover_moved = true;
                pointer.hover_px.reset();
                break;
            }
            if (left_down_ && (buttons & SDL_BUTTON_LMASK)) {
                if (!dragging_) {
                    // Not a drag YET: wait until the pointer has moved more
                    // than the click slop, then hand the whole accumulated
                    // movement to orbit() so no motion is lost.
                    const float dx = event.motion.x - press_pt_x_, dy = event.motion.y - press_pt_y_;
                    if (dx * dx + dy * dy > kClickSlopPt * kClickSlopPt) {
                        dragging_ = true;
                        systems::cancel_angle_flight(registry);
                        systems::orbit(cam, dx, dy);
                    }
                } else {
                    systems::orbit(cam, event.motion.xrel, event.motion.yrel);
                }
            } else if (right_down_ && (buttons & SDL_BUTTON_RMASK)) {
                // Same slop as the left button. Until it is crossed this is
                // still a possible click, so the camera must not move: a tag
                // gesture that nudges the view feels broken. Once it is a
                // drag, the accumulated movement is applied in one go (pan
                // works in pixels; SDL reports points).
                if (!right_dragging_) {
                    const float dx = event.motion.x - right_press_pt_x_;
                    const float dy = event.motion.y - right_press_pt_y_;
                    if (dx * dx + dy * dy > kClickSlopPt * kClickSlopPt) {
                        right_dragging_ = true;
                        systems::cancel_flight(registry);  // panning fights the flight for the target
                        systems::pan(cam, dx * pixel_density, dy * pixel_density);
                    }
                } else {
                    systems::pan(cam, event.motion.xrel * pixel_density, event.motion.yrel * pixel_density);
                }
            } else if (buttons & SDL_BUTTON_MMASK) {
                systems::cancel_flight(registry);
                systems::pan(cam, event.motion.xrel * pixel_density, event.motion.yrel * pixel_density);
            } else if (buttons == 0) {
                // Plain hover over the 3D view: remember where (pixels) for the tooltip pick.
                pointer.hover_px = glm::vec2(event.motion.x, event.motion.y) * pixel_density;
                pointer.hover_moved = true;
            }
            break;
        }
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            pointer.hover_px.reset();
            pointer.hover_moved = true;
            break;
        case SDL_EVENT_MOUSE_WHEEL: {
            if (ui.mouse) break;  // wheel over the panel scrolls the panel, not the camera
            // Some systems ("natural scrolling") report inverted wheel directions;
            // SDL tells us, and we undo it so "wheel up" always zooms in.
            float steps = event.wheel.y;
            if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) steps = -steps;
            systems::cancel_flight(registry);  // zooming during a flight: the user's hand wins
            systems::zoom(cam, steps);
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (ui.mouse) break;
            if (event.button.button == SDL_BUTTON_LEFT) {
                left_down_ = true;
                dragging_ = false;
                press_pt_x_ = event.button.x;
                press_pt_y_ = event.button.y;
                // SDL3 event timestamps are nanoseconds (SDL_GetTicksNS clock).
                press_ms_ = event.button.timestamp / 1000000u;
                pointer.hover_px.reset();  // no tooltip while a button is held
                pointer.hover_moved = true;
            } else if (event.button.button == SDL_BUTTON_RIGHT) {
                right_down_ = true;
                right_dragging_ = false;
                right_press_pt_x_ = event.button.x;
                right_press_pt_y_ = event.button.y;
                right_press_ms_ = event.button.timestamp / 1000000u;
                pointer.hover_px.reset();
                pointer.hover_moved = true;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.button == SDL_BUTTON_LEFT && left_down_) {
                left_down_ = false;
                const std::uint64_t held_ms = event.button.timestamp / 1000000u - press_ms_;
                if (!dragging_ && held_ms <= kClickMaxMs) {
                    // A click: report where it STARTED (in pixels). SDL counts
                    // rapid clicks for us (the OS double-click time):
                    // clicks == 2 on the second release of a double-click.
                    pointer.click_px = glm::vec2(press_pt_x_, press_pt_y_) * pixel_density;
                    pointer.double_click = event.button.clicks >= 2;
                }
                dragging_ = false;
            } else if (event.button.button == SDL_BUTTON_RIGHT && right_down_) {
                // Release counts even if it lands on the panel: the press
                // started on the 3D view, and the pick uses the press position.
                right_down_ = false;
                const std::uint64_t held_ms = event.button.timestamp / 1000000u - right_press_ms_;
                if (!right_dragging_ && held_ms <= kClickMaxMs) {
                    pointer.right_click_px = glm::vec2(right_press_pt_x_, right_press_pt_y_) * pixel_density;
                }
                right_dragging_ = false;
            }
            break;
        case SDL_EVENT_KEY_DOWN: {
            if (ui.keyboard) break;       // a text field has focus: keys are text, not hotkeys
            if (event.key.repeat) break;  // ignore auto-repeat for toggles
            // event.key.key is the *keycode* (layout-dependent: the key labelled
            // "F" on an AZERTY keyboard). event.key.scancode would be the
            // physical position (use that for WASD-style movement later).
            const SDL_Keycode k = event.key.key;
            // event.key.mod: modifier state at the time of the key press.
            // SDL_KMOD_CTRL covers left or right Ctrl; macOS users would expect
            // Cmd (SDL_KMOD_GUI), so accept both for the search shortcut.
            const bool ctrl = (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
            auto& colors = registry.ctx().get<scene::ColorSettings>();
            if ((ctrl && k == SDLK_F) || k == SDLK_SLASH) {
                // Focus the search box (the panel applies it next frame via
                // ImGui::SetKeyboardFocusHere); show the panel if it was hidden.
                view.show_panel = true;
                view.focus_search = true;
            } else if (k >= SDLK_1 && k <= SDLK_9) {
                const auto index = static_cast<int>(k - SDLK_1);
                if (index < static_cast<int>(scene::ColorScheme::Count)) {
                    colors.set(static_cast<scene::ColorScheme>(index));  // marks colours dirty
                }
            } else if (k == SDLK_F) {
                systems::focus_selection(registry);  // the selection, or Sol if nothing is selected
            } else if (k == SDLK_H || k == SDLK_HOME) {
                systems::fly_home(registry);
            } else if (k == SDLK_R) {
                systems::reset_view(registry);
            } else if (k == SDLK_G) {
                view.show_guides = !view.show_guides;
            } else if (k == SDLK_F1) {
                view.show_help = !view.show_help;  // (was H before milestone 3; H now means "home")
            } else if (k == SDLK_P || k == SDLK_TAB) {
                // Tab is also ImGui's keyboard-navigation key, but we don't
                // enable ImGuiConfigFlags_NavEnableKeyboard, and when a field
                // has focus ui.keyboard is set and we never get here.
                view.show_panel = !view.show_panel;
            } else if (k == SDLK_F12) {
                view.show_imgui_demo = !view.show_imgui_demo;  // ImGui's widget gallery: handy while learning
            } else if (k == SDLK_EQUALS || k == SDLK_PLUS || k == SDLK_KP_PLUS) {
                view.exposure = std::min(view.exposure * 1.25f, 20.0f);
            } else if (k == SDLK_MINUS || k == SDLK_KP_MINUS) {
                view.exposure = std::max(view.exposure / 1.25f, 0.05f);
            } else if (k == SDLK_ESCAPE) {
                // Esc first dismisses the tag menu (ImGui already saw the key
                // and will close the popup). `visible` is last frame's state,
                // which is the frame the user was looking at when they pressed
                // the key. Only with no menu does Esc clear the selection, and
                // with nothing selected it quits (M1 behaviour). Q always quits.
                const auto* menu = registry.ctx().find<scene::TagMenuRequest>();
                const bool menu_up = menu && menu->visible;
                if (!menu_up && systems::selected_entity(registry) != entt::null) systems::clear_selection(registry);
                else if (!menu_up) view.quit_requested = true;
            } else if (k == SDLK_Q) {
                view.quit_requested = true;
            }
            break;
        }
        default:
            break;
    }
}

}  // namespace starmap::input
