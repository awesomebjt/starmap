# =============================================================================
# cmake/Shaders.cmake — compile the .sc shaders offline with bgfx's `shaderc`.
#
# WHY OFFLINE?
# GPUs don't run .sc files. Each graphics API wants its own format:
#   OpenGL -> GLSL source text     Vulkan    -> SPIR-V bytecode
#   D3D11  -> DXBC bytecode        Metal     -> Metal Shading Language
# bgfx could, in theory, ship a runtime compiler, but that would add megabytes
# (glslang, SPIRV-Cross, ...) to every app and make shader errors appear only
# when a user launches the game. Instead `shaderc` (built from source as part of
# bgfx.cmake, see AppDependencies.cmake) runs at BUILD time and writes one small
# binary per backend. At run time, ShaderLoader picks the one that matches
# bgfx::getRendererType().
#
# OUTPUT LAYOUT (produced by bgfx.cmake's bgfx_compile_shaders helper):
#   <build>/shaders/glsl/vs_star.sc.bin     (profile 430  -> OpenGL 4.3 core)
#   <build>/shaders/spirv/vs_star.sc.bin    (profile spirv -> Vulkan)
#   <build>/shaders/dxbc/vs_star.sc.bin     (profile s_5_0 -> Direct3D 11/12)
#   <build>/shaders/metal/vs_star.sc.bin    (profile metal -> Metal)
# With single-config generators (Ninja, Makefiles) the executable lives in
# <build>/ too, so "shaders/" is right next to it. Multi-config generators
# (Visual Studio, Xcode) put the exe in <build>/Release/, and ShaderLoader also
# looks one directory up.
#
# PROFILES PER PLATFORM: only what that platform can actually run.
#   Linux:   OpenGL + Vulkan
#   Windows: Direct3D (DXBC works for both the D3D11 and D3D12 backends)
#            + Vulkan + OpenGL (handy with --renderer to compare backends)
#   macOS:   Metal (Apple deprecated OpenGL; bgfx's Metal backend is the default)
# =============================================================================

function(starmap_compile_shaders)
    set(varying ${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc)
    set(out_dir ${CMAKE_BINARY_DIR}/shaders)

    if(WIN32)
        set(profiles s_5_0 spirv 430)
    elseif(APPLE)
        set(profiles metal)
    else()
        set(profiles 430 spirv)
    endif()

    # One call per shader stage: shaderc must be told whether it compiles a
    # VERTEX or FRAGMENT program (they have different inputs/outputs).
    bgfx_compile_shaders(
        TYPE VERTEX
        SHADERS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vs_star.sc
                ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vs_line.sc
        VARYING_DEF ${varying}
        OUTPUT_DIR ${out_dir}
        PROFILES ${profiles}
        OUT_FILES_VAR vs_outputs)
    bgfx_compile_shaders(
        TYPE FRAGMENT
        SHADERS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_star.sc
                ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_line.sc
        VARYING_DEF ${varying}
        OUTPUT_DIR ${out_dir}
        PROFILES ${profiles}
        OUT_FILES_VAR fs_outputs)

    # Dear ImGui's shaders use their own varying definition file: ImGui's
    # vertices are 2D (vec2 a_position) with a texture coordinate, a different
    # input signature from the star/line shaders. bgfx_compile_shaders()
    # takes ONE varying.def.sc per call, so ImGui gets its own two calls.
    set(imgui_varying ${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying_imgui.def.sc)
    bgfx_compile_shaders(
        TYPE VERTEX
        SHADERS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vs_imgui.sc
        VARYING_DEF ${imgui_varying}
        OUTPUT_DIR ${out_dir}
        PROFILES ${profiles}
        OUT_FILES_VAR vs_imgui_outputs)
    bgfx_compile_shaders(
        TYPE FRAGMENT
        SHADERS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_imgui.sc
        VARYING_DEF ${imgui_varying}
        OUTPUT_DIR ${out_dir}
        PROFILES ${profiles}
        OUT_FILES_VAR fs_imgui_outputs)

    # A custom target that "owns" the generated files. The add_custom_command()s
    # created by bgfx_compile_shaders only run when some target depends on their
    # OUTPUT files; `starmap` will add_dependencies() on this target, so editing
    # a .sc file recompiles it on the next build.
    add_custom_target(starmap_shaders ALL DEPENDS ${vs_outputs} ${fs_outputs} ${vs_imgui_outputs} ${fs_imgui_outputs}
        SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vs_star.sc
                ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_star.sc
                ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vs_line.sc
                ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_line.sc
                ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vs_imgui.sc
                ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_imgui.sc
                ${varying} ${imgui_varying})
    set(STARMAP_SHADER_DIR ${out_dir} PARENT_SCOPE)
endfunction()
