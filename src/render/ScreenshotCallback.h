#pragma once
// =============================================================================
// render/ScreenshotCallback.h — our implementation of bgfx::CallbackI.
//
// ROLE IN THE ARCHITECTURE
//   bgfx reports back to the application through ONE interface object passed in
//   bgfx::Init::callback: fatal errors, trace/log messages, a shader cache, and
//   the results of bgfx::requestScreenShot(). If you pass no callback, bgfx uses
//   an internal default that ignores screenshots — so to save a PNG we must
//   provide our own.
//
// THREADING
//   The callbacks run on bgfx's RENDER thread. In our single-threaded setup
//   (see Graphics.cpp) that is the main thread, inside bgfx::frame(). In a
//   multi-threaded bgfx they would run concurrently with the game loop, which is
//   why the "saved" flag is a std::atomic — cheap insurance.
// =============================================================================

#include <bgfx/bgfx.h>

#include <atomic>
#include <cstdarg>
#include <string>

namespace starmap::render {

class ScreenshotCallback final : public bgfx::CallbackI {
public:
    explicit ScreenshotCallback(bool verbose_trace = false) : verbose_(verbose_trace) {}
    ~ScreenshotCallback() override = default;

    // Number of screenshots written so far and the last path/error.
    [[nodiscard]] int saved_count() const noexcept { return saved_.load(); }
    [[nodiscard]] std::string last_error() const { return last_error_; }

    // ---- bgfx::CallbackI ---------------------------------------------------
    // Called on unrecoverable errors (and on debug checks). For most codes bgfx
    // cannot continue, so we print and abort.
    void fatal(const char* file_path, uint16_t line, bgfx::Fatal::Enum code, const char* str) override;
    // bgfx's internal log (BX_TRACE). Very chatty; printed only if verbose.
    void traceVargs(const char* file_path, uint16_t line, const char* format, va_list args) override;
    // Profiler hooks (for tools like Tracy/Remotery). Unused.
    void profilerBegin(const char*, uint32_t, const char*, uint16_t) override {}
    void profilerBeginLiteral(const char*, uint32_t, const char*, uint16_t) override {}
    void profilerEnd() override {}
    // Shader/program binary cache (lets D3D/Vulkan skip recompiling pipelines
    // on the next run). Unused: returning 0/false means "not cached".
    uint32_t cacheReadSize(uint64_t) override { return 0; }
    bool cacheRead(uint64_t, void*, uint32_t) override { return false; }
    void cacheWrite(uint64_t, const void*, uint32_t) override {}
    // The one we care about: pixels of the back buffer after the frame was rendered.
    void screenShot(const char* file_path, uint32_t width, uint32_t height, uint32_t pitch,
                    bgfx::TextureFormat::Enum format, const void* data, uint32_t size, bool yflip) override;
    // Video capture (BGFX_RESET_CAPTURE). Unused.
    void captureBegin(uint32_t, uint32_t, uint32_t, bgfx::TextureFormat::Enum, bool) override {}
    void captureEnd() override {}
    void captureFrame(const void*, uint32_t) override {}

private:
    bool verbose_ = false;
    std::atomic<int> saved_{0};
    std::string last_error_;
};

}  // namespace starmap::render
