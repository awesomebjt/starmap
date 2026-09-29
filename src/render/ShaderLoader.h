#pragma once
// =============================================================================
// render/ShaderLoader.h — loads the shader binaries compiled at build time.
//
// ROLE IN THE ARCHITECTURE
//   cmake/Shaders.cmake runs bgfx's `shaderc` to produce, for every .sc file,
//   one binary per backend under shaders/<backend>/<name>.sc.bin. At run time
//   only bgfx knows which backend it picked (bgfx::getRendererType()), so this
//   class maps that to the directory name and turns the file into a
//   bgfx::ShaderHandle, then two shaders into a bgfx::ProgramHandle.
//
// PROGRAM = vertex shader + fragment shader linked together. Draw calls
// reference programs, never individual shaders.
// =============================================================================

#include <bgfx/bgfx.h>

#include <filesystem>
#include <string_view>
#include <vector>

namespace starmap::render {

class ShaderLoader {
public:
    // `search_roots`: directories that may contain the "shaders/" folder's
    // contents (e.g. <exe>/shaders). The first root with the file wins.
    explicit ShaderLoader(std::vector<std::filesystem::path> search_roots);

    // Default roots: <exe_dir>/shaders, <exe_dir>/../shaders (multi-config
    // generators) and the build tree's shader dir baked in at compile time.
    [[nodiscard]] static std::vector<std::filesystem::path> default_roots(const std::filesystem::path& exe_dir);

    // Subdirectory name for a renderer: "glsl", "spirv", "dxbc", "metal", ...
    [[nodiscard]] static const char* backend_dir(bgfx::RendererType::Enum type);

    // Load "<root>/<backend>/<name>.sc.bin". Throws std::runtime_error listing
    // every path tried if none exists.
    [[nodiscard]] bgfx::ShaderHandle load_shader(std::string_view name) const;
    // Load both and link. The program takes ownership of the shaders
    // (createProgram(..., destroyShaders = true)), so callers destroy only the program.
    [[nodiscard]] bgfx::ProgramHandle load_program(std::string_view vs_name, std::string_view fs_name) const;

private:
    std::vector<std::filesystem::path> roots_;
};

}  // namespace starmap::render
