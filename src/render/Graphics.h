#pragma once
// =============================================================================
// render/Graphics.h — owns the bgfx context (init / resize / shutdown).
//
// ROLE IN THE ARCHITECTURE
//   Window (SDL) --native handles--> Graphics (bgfx::init) --> renderers
//   Exactly one Graphics object exists while anything uses bgfx. It is an RAII
//   wrapper: constructor = bgfx::init, destructor = bgfx::shutdown.
//
// LIFETIME RULE (a very common bgfx crash):
//   Every bgfx resource (programs, vertex buffers, uniforms...) must be
//   destroyed BEFORE bgfx::shutdown(). In C++ that means: declare Graphics
//   FIRST and the renderers AFTER it, so they are destroyed first (members and
//   locals are destroyed in reverse order of declaration). App.h follows this.
// =============================================================================

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string_view>

namespace starmap::platform { class Window; }

namespace starmap::render {

struct GraphicsConfig {
    bgfx::RendererType::Enum renderer = bgfx::RendererType::Count;  // Count = let bgfx choose
    bool vsync = true;
    bgfx::CallbackI* callback = nullptr;  // e.g. ScreenshotCallback; must outlive Graphics
};

// "opengl", "vulkan", "d3d11", "d3d12", "metal", "auto" -> enum. Returns false if unknown.
bool renderer_from_string(std::string_view name, bgfx::RendererType::Enum& out);

class Graphics {
public:
    Graphics(const platform::Window& window, const GraphicsConfig& config);
    ~Graphics();
    Graphics(const Graphics&) = delete;
    Graphics& operator=(const Graphics&) = delete;

    // Call when the window's PIXEL size changed (SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED).
    void resize(uint32_t width, uint32_t height);
    void set_vsync(bool on);

    [[nodiscard]] uint32_t width() const noexcept { return width_; }
    [[nodiscard]] uint32_t height() const noexcept { return height_; }
    [[nodiscard]] bool homogeneous_depth() const;   // see CameraSystem.h
    [[nodiscard]] const char* renderer_name() const;

private:
    uint32_t width_ = 0, height_ = 0;
    uint32_t reset_flags_ = BGFX_RESET_NONE;
};

}  // namespace starmap::render
