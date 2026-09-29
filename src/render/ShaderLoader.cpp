// =============================================================================
// render/ShaderLoader.cpp — load the shaderc output for the running backend
// =============================================================================
//
// cmake/Shaders.cmake compiles each .sc file at build time into one binary
// per backend: shaders/glsl/, spirv/, dxbc/, metal/. bgfx decides the backend
// at bgfx::init() (auto-selected, or forced with --renderer), so the loader
// must be called AFTER init. Before that, bgfx::getRendererType() returns
// Noop. The loader then picks the directory for that backend.
// Alternative: embed the binaries in the executable (shaderc --bin2c plus
// BGFX_EMBEDDED_SHADER). That makes one self-contained file, but every
// shader edit then means recompiling C++. Loading from disk keeps the
// edit -> rebuild -> run loop short, and the files ship next to the exe.
// =============================================================================
#include "render/ShaderLoader.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

#ifndef STARMAP_SHADER_DIR
#define STARMAP_SHADER_DIR ""  // set by CMake to <build>/shaders
#endif

namespace starmap::render {

ShaderLoader::ShaderLoader(std::vector<std::filesystem::path> search_roots) : roots_(std::move(search_roots)) {}

std::vector<std::filesystem::path> ShaderLoader::default_roots(const std::filesystem::path& exe_dir) {
    std::vector<std::filesystem::path> roots{exe_dir / "shaders", exe_dir / ".." / "shaders"};
    if (const std::string build_dir = STARMAP_SHADER_DIR; !build_dir.empty()) roots.emplace_back(build_dir);
    return roots;
}

const char* ShaderLoader::backend_dir(bgfx::RendererType::Enum type) {
    // Must match the directory names bgfx.cmake's bgfx_compile_shaders() writes
    // (profile 430 -> glsl, s_5_0 -> dxbc, ...). One compiled binary per API.
    switch (type) {
        case bgfx::RendererType::OpenGL:     return "glsl";
        case bgfx::RendererType::OpenGLES:   return "essl";
        case bgfx::RendererType::Vulkan:     return "spirv";
        case bgfx::RendererType::Direct3D11: return "dxbc";
        case bgfx::RendererType::Direct3D12: return "dxbc";  // bgfx's D3D12 backend also accepts DXBC (s_5_0)
        case bgfx::RendererType::Metal:      return "metal";
        default:                             return "unknown";  // Noop/Agc/Gnm/Nvn/WebGPU: not built here
    }
}

bgfx::ShaderHandle ShaderLoader::load_shader(std::string_view name) const {
    const char* backend = backend_dir(bgfx::getRendererType());
    const std::string file = std::string(name) + ".sc.bin";
    std::string tried;
    for (const auto& root : roots_) {
        const std::filesystem::path path = (root / backend / file).lexically_normal();
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            tried += "\n  " + path.string();
            continue;
        }
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        // bgfx::copy() allocates bgfx-owned memory and copies our bytes into it.
        // bgfx processes resource creation asynchronously (at the next frame()),
        // so it must own the data — never pass a pointer to a local buffer
        // without copy() (or makeRef() with a lifetime guarantee).
        // The extra '\0' matters for the GLSL backend, whose "binary" is source
        // text that the driver compiles; the NUL terminator keeps it a valid C string.
        const bgfx::Memory* mem = bgfx::alloc(static_cast<uint32_t>(bytes.size() + 1));
        std::copy(bytes.begin(), bytes.end(), mem->data);
        mem->data[bytes.size()] = '\0';

        const bgfx::ShaderHandle h = bgfx::createShader(mem);
        if (!bgfx::isValid(h)) throw std::runtime_error("bgfx::createShader failed for " + path.string());
        // Names show up in graphics debuggers (RenderDoc, Xcode, PIX): cheap and invaluable.
        bgfx::setName(h, file.c_str(), static_cast<int32_t>(file.size()));
        return h;
    }
    throw std::runtime_error("shader '" + file + "' for backend '" + backend + "' not found; tried:" + tried +
                             "\n(build the 'starmap_shaders' target, or run from the build directory)");
}

bgfx::ProgramHandle ShaderLoader::load_program(std::string_view vs_name, std::string_view fs_name) const {
    const bgfx::ShaderHandle vs = load_shader(vs_name);
    const bgfx::ShaderHandle fs = load_shader(fs_name);
    // destroyShaders = true: the shader handles are released together with the
    // program. (Keep them if you link one vertex shader with many fragment shaders.)
    const bgfx::ProgramHandle p = bgfx::createProgram(vs, fs, true);
    if (!bgfx::isValid(p)) throw std::runtime_error("bgfx::createProgram failed for " + std::string(vs_name));
    return p;
}

}  // namespace starmap::render
