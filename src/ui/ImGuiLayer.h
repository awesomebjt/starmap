#pragma once
// =============================================================================
// ui/ImGuiLayer.h — Dear ImGui's lifetime, input and per-frame calls
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   Dear ImGui has three parts that must be created and destroyed in order:
//     1. the CONTEXT        (ImGui::CreateContext: all UI state, fonts, style)
//     2. a PLATFORM backend (imgui_impl_sdl3: feeds mouse/keyboard/time/DPI
//                            from SDL3 into ImGui, sets cursors, clipboard)
//     3. a RENDERER backend (render::ImGuiRenderer: draws ImDrawData with bgfx)
//   ImGuiLayer owns all three as members, declared in that order, so C++
//   constructs them 1-2-3 and destroys them 3-2-1 automatically (the renderer
//   must free its bgfx textures while the context still exists, and the
//   context must outlive both backends).
//
// PER FRAME (App::run):
//     for each SDL event: process_event(e) -> then, unless ImGui wants the
//                         mouse/keyboard, our InputHandler gets it too
//     begin_frame()      -> ImGui_ImplSDL3_NewFrame + ImGui::NewFrame
//     ... any ImGui:: calls (FilterPanel, HUD) ...
//     end_frame()        -> ImGui::Render + ImGuiRenderer::render (bgfx view)
//
// INPUT ARBITRATION: wants_mouse() / wants_keyboard() expose
// io.WantCaptureMouse / io.WantCaptureKeyboard. ImGui sets them when the
// pointer is over one of its windows (or a drag started on one), or a text
// field has keyboard focus. The app must then NOT also react — otherwise
// dragging a slider would orbit the camera, and typing "1" into a number
// field would switch the colour scheme. Note that these flags are computed
// in NewFrame() from the PREVIOUS frame's state; events are processed before
// NewFrame, so they use last frame's answer. That one-frame latency is how
// every ImGui app works and is imperceptible.
// =============================================================================

#include "render/ImGuiRenderer.h"

#include <bgfx/bgfx.h>

struct SDL_Window;
union SDL_Event;
struct ImGuiContext;

namespace starmap::render { class ShaderLoader; }

namespace starmap::ui {

class ImGuiLayer {
public:
    // Creates the context, initialises the SDL3 backend for `window`, loads
    // the font and style (scaled for the display, see ImGuiLayer.cpp) and
    // the bgfx renderer that draws into `view`.
    ImGuiLayer(SDL_Window* window, const render::ShaderLoader& shaders, bgfx::ViewId view);
    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    // Forward one SDL event to ImGui (always call it, for every event).
    void process_event(const SDL_Event& event);
    void begin_frame();
    void end_frame();

    [[nodiscard]] bool wants_mouse() const;
    [[nodiscard]] bool wants_keyboard() const;

private:
    // Tiny RAII wrappers so member order expresses the lifetime rules.
    struct Context {
        ImGuiContext* ctx = nullptr;
        Context();
        ~Context();
        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;
    };
    struct SdlPlatform {
        explicit SdlPlatform(SDL_Window* window);
        ~SdlPlatform();
        SdlPlatform(const SdlPlatform&) = delete;
        SdlPlatform& operator=(const SdlPlatform&) = delete;
    };

    Context context_;               // 1. constructed first, destroyed last
    SdlPlatform platform_;          // 2.
    render::ImGuiRenderer renderer_;  // 3. constructed last, destroyed first
};

}  // namespace starmap::ui
