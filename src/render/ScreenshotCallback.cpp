// =============================================================================
// render/ScreenshotCallback.cpp — writes bgfx screenshots as PNG using bimg.
// =============================================================================
#include "render/ScreenshotCallback.h"

#include <bimg/bimg.h>
#include <bx/file.h>

#include <cstdio>
#include <cstdlib>

namespace starmap::render {

void ScreenshotCallback::fatal(const char* file_path, uint16_t line, bgfx::Fatal::Enum code, const char* str) {
    std::fprintf(stderr, "bgfx fatal error %d at %s:%u: %s\n", static_cast<int>(code), file_path,
                 static_cast<unsigned>(line), str);
    // DebugCheck is raised by BX_ASSERT-style checks in debug builds; bgfx can
    // continue after it. Everything else (device lost, invalid shader...) cannot.
    if (code != bgfx::Fatal::DebugCheck) std::abort();
}

void ScreenshotCallback::traceVargs(const char* file_path, uint16_t line, const char* format, va_list args) {
    if (!verbose_) return;
    std::fprintf(stderr, "bgfx %s:%u: ", file_path, static_cast<unsigned>(line));
    std::vfprintf(stderr, format, args);
}

void ScreenshotCallback::screenShot(const char* file_path, uint32_t width, uint32_t height, uint32_t pitch,
                                    bgfx::TextureFormat::Enum format, const void* data, uint32_t /*size*/,
                                    bool yflip) {
    // `data` is the raw back buffer: usually BGRA8, `pitch` bytes per row
    // (may be larger than width*4 due to row alignment). OpenGL's origin is the
    // BOTTOM-left, so rows arrive upside down and bgfx tells us via yflip.
    // bimg::imageWritePng handles BGRA->RGBA swizzling, the pitch and the flip.
    // bx::FileWriter is bx's portable file stream (bimg writes through the
    // bx::WriterI interface rather than FILE* or std::ostream).
    bx::FileWriter writer;
    bx::Error err;
    if (!bx::open(&writer, bx::FilePath(file_path), false, &err)) {
        last_error_ = std::string("cannot open ") + file_path;
        std::fprintf(stderr, "screenshot: %s\n", last_error_.c_str());
        return;
    }
    bimg::imageWritePng(&writer, width, height, pitch, data, static_cast<bimg::TextureFormat::Enum>(format),
                        yflip, &err);
    bx::close(&writer);
    if (!err.isOk()) {
        last_error_ = std::string("PNG encoding failed for ") + file_path;
        std::fprintf(stderr, "screenshot: %s\n", last_error_.c_str());
        return;
    }
    std::printf("screenshot saved: %s (%ux%u)\n", file_path, width, height);
    saved_.fetch_add(1);
}

}  // namespace starmap::render
