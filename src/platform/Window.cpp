// =============================================================================
// platform/Window.cpp — SDL3 window creation and native handle extraction.
// =============================================================================
//
// See Window.h for the role of this class. Things worth knowing about SDL3 here:
//   * THREADING: SDL_Init, SDL_CreateWindow and the event pump must run on the
//     MAIN thread. macOS enforces this (Cocoa crashes otherwise), and it is
//     good practice everywhere. bgfx then renders on the same thread (see
//     render/Graphics.cpp), so this program is entirely single-threaded.
//   * POINTS VS PIXELS: window sizes and mouse coordinates are in points
//     (logical units). The GPU back buffer is in pixels. On a 200% display
//     1 point = 2 pixels. Mixing them up is the #1 HiDPI bug: a quarter-size
//     image in one corner, or clicks landing in the wrong place.
//   * ERRORS: SDL3 functions return bool (true = success) or a null pointer,
//     and the reason is in SDL_GetError(), a thread-local string.
// =============================================================================
#include "platform/Window.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace starmap::platform {

Window::Window(const std::string& title, int width, int height) {
    // SDL is initialised per SUBSYSTEM. We only need video (which implies events).
    // Audio/gamepads would be added here later with SDL_INIT_AUDIO | SDL_INIT_GAMEPAD.
    // SDL3 changed the return convention: functions return true on success
    // (SDL2 returned 0 on success — a classic porting bug).
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    }
    // SDL_WINDOW_HIGH_PIXEL_DENSITY: on macOS/Wayland/Windows-with-scaling, ask
    // for a back buffer at the FULL pixel resolution of the display instead of a
    // blurry, upscaled low-resolution one. The window size stays in points.
    // SDL_WINDOW_RESIZABLE: the user may resize; we handle it in the event loop.
    // Note: no SDL_WINDOW_OPENGL / SDL_WINDOW_VULKAN — bgfx creates the context.
    const SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    window_ = SDL_CreateWindow(title.c_str(), width, height, flags);
    if (!window_) {
        const std::string err = SDL_GetError();
        SDL_Quit();
        throw std::runtime_error("SDL_CreateWindow failed: " + err);
    }
}

Window::~Window() {
    // Order matters: bgfx must already be shut down, because it renders into
    // this window. App guarantees it by declaring graphics_ AFTER window_:
    // members are destroyed in reverse declaration order, so Graphics
    // (bgfx::shutdown) runs first.
    // SDL_Quit also undoes SDL_Init. One Window per process is assumed.
    SDL_DestroyWindow(window_);
    SDL_Quit();
}

PixelSize Window::pixel_size() const {
    PixelSize s;
    // SDL_GetWindowSize would return POINTS; the swap chain needs PIXELS.
    SDL_GetWindowSizeInPixels(window_, &s.width, &s.height);
    return s;
}

float Window::pixel_density() const { return SDL_GetWindowPixelDensity(window_); }

void Window::set_title(const std::string& title) { SDL_SetWindowTitle(window_, title.c_str()); }

NativeHandles Window::native_handles() const {
    // SDL3 exposes platform handles through a generic PROPERTIES object instead
    // of SDL2's SDL_GetWindowWMInfo() struct. Each property has a documented
    // name (a string constant) and type (pointer / number).
    NativeHandles h;
    const SDL_PropertiesID props = SDL_GetWindowProperties(window_);
#if defined(SDL_PLATFORM_WIN32)
    h.window = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#elif defined(SDL_PLATFORM_MACOS)
    // bgfx accepts the NSWindow* and creates its own CAMetalLayer inside it.
    // (Alternative: SDL_Metal_CreateView() + SDL_Metal_GetLayer() and pass the
    // CAMetalLayer*; useful if you mix SDL's and bgfx's rendering.)
    h.window = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
#elif defined(SDL_PLATFORM_LINUX) || defined(SDL_PLATFORM_FREEBSD)
    // Linux has two display servers and SDL picks one at run time (Wayland if
    // available, else X11; override with SDL_VIDEO_DRIVER=x11). Ask which.
    const char* driver = SDL_GetCurrentVideoDriver();
    if (driver && std::strcmp(driver, "wayland") == 0) {
        h.display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        h.window = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
        h.wayland = true;
    } else {
        // An X11 "Window" is an integer id (XID), not a pointer. bgfx's API takes
        // void*, so the id is smuggled through a pointer-sized integer cast.
        h.display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        const auto xid = SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
        h.window = reinterpret_cast<void*>(static_cast<std::uintptr_t>(xid));
    }
#else
#error "starmap: unsupported platform (add its SDL window property here)"
#endif
    if (!h.window) throw std::runtime_error("could not get the native window handle from SDL");
    return h;
}

std::filesystem::path executable_dir() {
    // Returns a UTF-8 path ending in a separator, owned by SDL (don't free it).
    // (SDL2's SDL_GetBasePath returned a malloc'd string the caller had to
    // SDL_free; SDL3 caches it internally, another porting pitfall.)
    // Why not argv[0]? It may be relative or just a name found through PATH,
    // and it isn't UTF-8 on Windows. SDL asks the OS properly
    // (GetModuleFileNameW, /proc/self/exe, NSBundle).
    const char* base = SDL_GetBasePath();
    if (!base) return std::filesystem::current_path();
    const std::string s(base);
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

}  // namespace starmap::platform
