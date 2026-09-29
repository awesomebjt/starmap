// =============================================================================
// render/Graphics.cpp — bgfx initialisation, resize and shutdown.
// =============================================================================
#include "render/Graphics.h"

#include "platform/Window.h"

#include <stdexcept>
#include <string>

namespace starmap::render {

bool renderer_from_string(std::string_view name, bgfx::RendererType::Enum& out) {
    using RT = bgfx::RendererType;
    if (name == "auto") out = RT::Count;
    else if (name == "opengl" || name == "gl") out = RT::OpenGL;
    else if (name == "vulkan" || name == "vk") out = RT::Vulkan;
    else if (name == "d3d11" || name == "dx11") out = RT::Direct3D11;
    else if (name == "d3d12" || name == "dx12") out = RT::Direct3D12;
    else if (name == "metal" || name == "mtl") out = RT::Metal;
    else if (name == "noop") out = RT::Noop;  // renders nothing; useful for headless CPU profiling
    else return false;
    return true;
}

Graphics::Graphics(const platform::Window& window, const GraphicsConfig& config) {
    // ---------------------------------------------------------------------
    // 1) SINGLE-THREADED MODE: call bgfx::renderFrame() BEFORE bgfx::init().
    //
    // By default bgfx spawns its own render thread: the "API thread" (ours)
    // records draw calls, the render thread talks to the GPU. That is great for
    // performance but it means the render thread touches the window — and on
    // several platforms (macOS/Cocoa in particular) window/GL/Metal calls must
    // happen on the main thread, SDL's event pump too. Calling renderFrame()
    // once before init tells bgfx "I will drive rendering myself on this
    // thread"; bgfx then does not create a render thread and bgfx::frame()
    // renders inline. Simpler to debug, no cross-thread surprises, and plenty
    // fast for a star map. (Remove this line later if profiling shows the CPU
    // side of frame() is the bottleneck — and then keep all SDL window calls on
    // the main thread anyway.)
    // ---------------------------------------------------------------------
    bgfx::renderFrame();

    const platform::NativeHandles handles = window.native_handles();
    const platform::PixelSize px = window.pixel_size();
    width_ = static_cast<uint32_t>(px.width);
    height_ = static_cast<uint32_t>(px.height);

    // ---------------------------------------------------------------------
    // 2) Describe what we want in bgfx::Init.
    // ---------------------------------------------------------------------
    bgfx::Init init;  // the constructor fills sensible defaults for every field
    init.type = config.renderer;            // Count = auto (D3D on Windows, Metal on macOS, Vulkan/GL on Linux)
    init.callback = config.callback;        // nullptr = bgfx's default (no screenshots)

    // Native window: bgfx >= 1.13x describes the main window as a SwapChain
    // (older tutorials set bgfx::PlatformData::nwh/ndt — same information).
    init.platformData.type = handles.wayland ? bgfx::NativeWindowHandleType::Wayland
                                             : bgfx::NativeWindowHandleType::Default;
    init.swapChain.nwh = handles.window;
    init.swapChain.ndt = handles.display;
    init.swapChain.width = width_;          // PIXELS, not points (HiDPI!)
    init.swapChain.height = height_;

    // Reset flags = global presentation settings. VSYNC caps the frame rate to
    // the display refresh (no tearing, less heat); disable for benchmarking.
    reset_flags_ = config.vsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE;
#if defined(__APPLE__)
    // On macOS bgfx's Metal backend sizes its layer from the window's backing
    // scale factor only when asked to (untested here; harmless elsewhere).
    reset_flags_ |= BGFX_RESET_HIDPI;
#endif
    init.reset = reset_flags_;

    // Transient buffers are per-frame scratch memory for data we regenerate
    // every frame (our 92k star instances: 92k * 48 B = 4.4 MB). The default
    // maximum is 6 MB; raise it so the catalogue (and a future larger one)
    // fits comfortably in one draw call.
    init.limits.maxTransientVbSize = 32u << 20;

    // ---------------------------------------------------------------------
    // 3) bgfx::init creates the device/context and the swap chain.
    // ---------------------------------------------------------------------
    if (!bgfx::init(init)) {
        throw std::runtime_error("bgfx::init failed (try --renderer opengl or --renderer vulkan)");
    }
    // Debug flags: BGFX_DEBUG_TEXT enables bgfx's built-in 8x16 px console
    // font overlay (dbgTextPrintf). Milestone 1 used it for the HUD; since
    // milestone 2 all text is drawn with Dear ImGui (HiDPI-aware, proper
    // fonts, never under the panel), so the overlay stays off. Flip this to
    // BGFX_DEBUG_STATS for bgfx's own profiler overlay when curious.
    bgfx::setDebug(BGFX_DEBUG_NONE);
}

Graphics::~Graphics() {
    // Destroys the device and swap chain. All bgfx handles must already be
    // destroyed (see the lifetime rule in Graphics.h).
    bgfx::shutdown();
}

void Graphics::resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;  // minimised window: keep the old back buffer
    width_ = width;
    height_ = height;
    // bgfx::reset() resizes the back buffer (and applies reset flags). With the
    // SwapChain API, fields we leave at their defaults keep their current value.
    // It does NOT resize the OS window — SDL already did that; we follow it.
    bgfx::SwapChain sc;
    sc.width = width;
    sc.height = height;
    bgfx::reset(reset_flags_, &sc);
}

void Graphics::set_vsync(bool on) {
    reset_flags_ = on ? (reset_flags_ | BGFX_RESET_VSYNC) : (reset_flags_ & ~BGFX_RESET_VSYNC);
    bgfx::reset(reset_flags_, nullptr);
}

bool Graphics::homogeneous_depth() const { return bgfx::getCaps()->homogeneousDepth; }

const char* Graphics::renderer_name() const { return bgfx::getRendererName(bgfx::getRendererType()); }

}  // namespace starmap::render
