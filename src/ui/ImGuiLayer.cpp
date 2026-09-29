// =============================================================================
// ui/ImGuiLayer.cpp — see ImGuiLayer.h
// =============================================================================
#include "ui/ImGuiLayer.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include <filesystem>
#include <stdexcept>
#include <system_error>

namespace starmap::ui {
namespace {

// Base font size in points. ImGui's classic default is 13 px, which is small
// on today's screens and hard to read in screenshots; 15 is a comfortable
// size for a side panel.
constexpr float kFontSize = 15.0f;

// How much bigger than "1 point" the UI should be drawn on this display.
//   SDL_GetWindowDisplayScale = pixel density x content scale, e.g.
//     macOS Retina:        2.0 x 1.0 = 2.0
//     Windows at 150 %:    1.0 x 1.5 = 1.5  (Windows windows are sized in pixels)
//     plain 96-dpi Linux:  1.0
//   ImGui already works in POINTS and gets pixel density through
//   io.DisplayFramebufferScale (set by the SDL3 backend), so the part left
//   for us is the CONTENT scale = display scale / pixel density.
//   macOS: 2.0 / 2.0 = 1 (nothing extra); Windows 150 %: 1.5 / 1.0 = 1.5.
float content_scale(SDL_Window* window) {
    const float display = SDL_GetWindowDisplayScale(window);
    const float density = SDL_GetWindowPixelDensity(window);
    if (display <= 0.0f || density <= 0.0f) return 1.0f;  // 0 = SDL couldn't tell
    return display / density;
}

void apply_style(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    // A slightly softer, more "panel-like" look than the defaults.
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.WindowPadding = ImVec2(12.0f, 10.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;  // mostly opaque: stars behind the panel don't distract
    // Popups (the scheme combo's list, tooltips) open ON TOP of the panel's
    // own widgets; the default slightly translucent background lets that
    // text show through and makes the list hard to read. Make it opaque.
    style.Colors[ImGuiCol_PopupBg].w = 1.0f;
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.95f, 0.95f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.85f, 0.35f, 1.0f);
    // Scale every padding/spacing/rounding for high-DPI content scales, and the
    // fonts via FontScaleDpi (1.92: fonts are re-rasterised at the new size
    // instead of being stretched, so they stay sharp).
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

}  // namespace

// ---- 1. context ----------------------------------------------------------------------
ImGuiLayer::Context::Context() {
    // IMGUI_CHECKVERSION compares the header version and struct sizes this
    // file was compiled with against the compiled library: catches mixing
    // headers and binaries of different versions or configurations.
    IMGUI_CHECKVERSION();
    ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // imgui.ini would persist window positions between runs. Our panel is
    // placed by code every frame, and screenshots must be reproducible, so
    // don't read or write the file.
    io.IniFilename = nullptr;
}
ImGuiLayer::Context::~Context() { ImGui::DestroyContext(ctx); }

// ---- 2. platform backend ----------------------------------------------------------------
ImGuiLayer::SdlPlatform::SdlPlatform(SDL_Window* window) {
    // "ForOther" = we render with something that is not SDL_Renderer, SDL_GPU,
    // OpenGL, Vulkan, D3D or Metal driven directly by the app — bgfx. The
    // backend then only handles input, time, cursors, clipboard and DPI.
    if (!ImGui_ImplSDL3_InitForOther(window)) throw std::runtime_error("ImGui_ImplSDL3_InitForOther failed");
}
ImGuiLayer::SdlPlatform::~SdlPlatform() { ImGui_ImplSDL3_Shutdown(); }

// ---- the layer -------------------------------------------------------------------------
// The member initialisers run in DECLARATION order (context_, platform_,
// renderer_), whatever order is written here. Font and style are set up in
// the body, after all three exist.
ImGuiLayer::ImGuiLayer(SDL_Window* window, const render::ShaderLoader& shaders, bgfx::ViewId view)
    : context_(), platform_(window), renderer_(shaders, view) {
    const float scale = content_scale(window);
    apply_style(scale);

    // Fonts: AddFontDefaultVector() is the scalable ProggyVector font embedded
    // in ImGui (no file to ship). With the 1.92 dynamic atlas, glyphs are
    // rasterised on first use at whatever size is needed; the renderer gets a
    // texture create/update request for them (ImGuiRenderer::update_texture).
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.SizePixels = kFontSize;
    io.Fonts->AddFontDefaultVector(&cfg);

    // Fallback glyphs (milestone 3). ProggyVector covers ASCII + Latin-1
    // only, but star names use Greek letters ("ε Eri"), superscripts
    // ("ο² Eri") and units use ☉ / ⊕. MergeMode = true adds a second font
    // SOURCE to the same ImFont: a glyph missing from the first source is
    // looked up in the next one. With the 1.92 dynamic atlas no glyph ranges
    // have to be listed; glyphs are rasterised on demand. The system font is
    // optional: if none of these files exists, ui::fix_units() and ImGui's
    // '?' fallback take over, and nothing breaks.
    // The font is read at RUNTIME from the machine the app runs on (it is not
    // compiled in), so a binary built in the Docker image picks up the host's
    // font. Each distro files DejaVu differently, hence the list:
    //   Debian/Ubuntu: truetype/dejavu/   Arch: TTF/
    //   Fedora:        dejavu-sans-fonts/ openSUSE: truetype/ (flat)
    //   older Fedora/others: dejavu/
    for (const char* path : {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                             "/usr/share/fonts/TTF/DejaVuSans.ttf",
                             "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
                             "/usr/share/fonts/truetype/DejaVuSans.ttf",
                             "/usr/share/fonts/dejavu/DejaVuSans.ttf"}) {
        std::error_code ec;  // non-throwing overload: a missing file is not an error here
        if (!std::filesystem::exists(path, ec)) continue;
        ImFontConfig merge;
        merge.MergeMode = true;
        merge.SizePixels = kFontSize;
        io.Fonts->AddFontFromFileTTF(path, kFontSize, &merge);
        break;
    }
    ImGui::GetStyle().FontSizeBase = kFontSize;
}

void ImGuiLayer::process_event(const SDL_Event& event) { ImGui_ImplSDL3_ProcessEvent(&event); }

void ImGuiLayer::begin_frame() {
    // Backend first (updates io.DisplaySize, DeltaTime, mouse position,
    // framebuffer scale), then ImGui itself (starts a new UI frame; from here
    // until Render() any ImGui:: call is legal).
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::end_frame() {
    // Render() finalises this frame's geometry into ImDrawData (it does not
    // touch the GPU); our backend then records the bgfx draw calls, which run
    // at bgfx::frame().
    ImGui::Render();
    renderer_.render(ImGui::GetDrawData());
}

bool ImGuiLayer::wants_mouse() const { return ImGui::GetIO().WantCaptureMouse; }
bool ImGuiLayer::wants_keyboard() const { return ImGui::GetIO().WantCaptureKeyboard; }

}  // namespace starmap::ui
