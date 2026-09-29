#pragma once
// =============================================================================
// platform/Window.h — the OS window, created with SDL3.
//
// ROLE IN THE ARCHITECTURE
//   SDL3 owns the *platform* side: the window, input events, DPI information.
//   bgfx owns the *GPU* side: device, swap chain, drawing. The only thing that
//   crosses the boundary is the native window handle (plus a display
//   connection on Linux), returned here as a plain struct so that this header
//   does not depend on bgfx and render/Graphics.cpp does not depend on SDL's
//   property names.
//
// WHY NOT LET SDL CREATE THE OpenGL/Vulkan CONTEXT?
//   SDL can (SDL_GL_CreateContext, SDL_Vulkan_CreateSurface), but bgfx wants to
//   create and own the device/context itself so that the same code runs on
//   D3D, Metal, Vulkan or GL. So the window is created WITHOUT SDL_WINDOW_OPENGL
//   / SDL_WINDOW_VULKAN flags and we just give bgfx the raw handle.
// =============================================================================

#include <filesystem>
#include <string>

struct SDL_Window;  // forward declaration: users of this header don't need SDL's headers

namespace starmap::platform {

// What bgfx needs to attach its swap chain to our window.
struct NativeHandles {
    void* window = nullptr;   // HWND | NSWindow* | X11 Window id (cast to void*) | wl_surface*
    void* display = nullptr;  // X11 Display* or wl_display* on Linux; nullptr elsewhere
    bool wayland = false;     // true => bgfx must be told the handle is a Wayland surface
};

struct PixelSize {
    int width = 0;
    int height = 0;
};

class Window {
public:
    // Initialises SDL's video subsystem and opens a resizable, HiDPI-aware window.
    // `width`/`height` are in *points* (logical units); on a 200 % display the
    // back buffer will be twice as many pixels — see pixel_size().
    // Throws std::runtime_error with SDL_GetError() on failure.
    Window(const std::string& title, int width, int height);
    ~Window();

    Window(const Window&) = delete;             // owns an OS resource: not copyable
    Window& operator=(const Window&) = delete;

    [[nodiscard]] SDL_Window* sdl() const noexcept { return window_; }

    // Size of the drawable area in PIXELS. This is what bgfx must render at.
    [[nodiscard]] PixelSize pixel_size() const;
    // Pixels per point (1.0 on classic displays, 2.0 on "Retina"/200 %).
    // Mouse coordinates arrive in points, so input code multiplies by this.
    [[nodiscard]] float pixel_density() const;

    [[nodiscard]] NativeHandles native_handles() const;
    void set_title(const std::string& title);

private:
    SDL_Window* window_ = nullptr;
};

// Directory containing the running executable (SDL_GetBasePath). Used to find
// shaders/ and stars.db next to the program regardless of the working directory.
[[nodiscard]] std::filesystem::path executable_dir();

}  // namespace starmap::platform
