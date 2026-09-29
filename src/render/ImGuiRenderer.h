#pragma once
// =============================================================================
// render/ImGuiRenderer.h — a Dear ImGui renderer backend written for bgfx
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   Dear ImGui never draws anything itself. Each frame, after ImGui::Render(),
//   it hands over an ImDrawData: a list of "command lists" (roughly one per
//   window), each holding
//     * a vertex buffer  (ImDrawVert: 2D position, UV, packed RGBA colour),
//     * an index buffer  (16-bit triangle indices),
//     * draw commands    (index range + clip rectangle + texture to use).
//   A RENDERER BACKEND turns that into GPU draw calls. Official backends exist
//   for OpenGL, D3D, Vulkan, Metal, SDL_GPU ... but not for bgfx, so this
//   class is ours. Its bgfx concepts, each explained where it is used:
//     VertexLayout (matching ImDrawVert), transient vertex/index buffers,
//     textures created/updated on ImGui's request (the font atlas),
//     an orthographic projection, per-command scissor rectangles, blending,
//     and a dedicated bgfx view in Sequential mode drawn after the 3D scene.
//
//   ui::ImGuiLayer owns one of these next to the ImGui context and the SDL3
//   platform backend, and calls render() once per frame.
//
// WHY A SEPARATE bgfx VIEW (App.cpp: kViewImGui = 2)?
//   * ORDER: bgfx executes views in increasing id order, so the UI (view 2)
//     is guaranteed to land on top of guides (0) and stars (1).
//   * DIFFERENT CAMERA: views have their own view/projection transform. The
//     UI needs a 2D pixel-space ortho projection; the stars a 3D perspective.
//   * DIFFERENT VIEWPORT: the 3D views cover only the area left of the filter
//     panel; the UI view covers the whole window.
//   * SEQUENTIAL MODE: within one view bgfx normally SORTS draw calls (by
//     state/program/depth) to reduce GPU state changes. For a UI that would be
//     wrong — overlapping windows must be drawn exactly in submission order.
//     bgfx::setViewMode(view, ViewMode::Sequential) disables the sorting for
//     this view only.
//
// WHY TRANSIENT BUFFERS?
//   ImGui regenerates ALL of its geometry every frame (immediate mode), and
//   the vertex count changes whenever the UI changes. bgfx's transient
//   buffers are per-frame scratch memory designed for exactly this: allocate,
//   fill, draw, and bgfx recycles the memory after the frame. No handles to
//   create or destroy, no resizing logic. (A DynamicVertexBuffer would work
//   too, but needs manual growth and double-buffering care.)
// =============================================================================

#include <bgfx/bgfx.h>

struct ImDrawData;
struct ImTextureData;

namespace starmap::render {

class ShaderLoader;

class ImGuiRenderer {
public:
    // Requires a current ImGui context (it registers itself in ImGuiIO) and
    // an initialised bgfx. Loads vs_imgui/fs_imgui via `shaders`.
    ImGuiRenderer(const ShaderLoader& shaders, bgfx::ViewId view);
    // Destroys the textures ImGui asked us to create, the program and the
    // sampler uniform — BEFORE bgfx::shutdown and ImGui::DestroyContext
    // (ui::ImGuiLayer's member order guarantees both).
    ~ImGuiRenderer();
    ImGuiRenderer(const ImGuiRenderer&) = delete;
    ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;

    // Records ImGui's draw data into our bgfx view. Call after ImGui::Render().
    void render(ImDrawData* draw_data);

private:
    void update_texture(ImTextureData* tex);
    static void destroy_texture(ImTextureData* tex);

    bgfx::ViewId view_;
    bgfx::VertexLayout layout_;
    bgfx::ProgramHandle program_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_tex_ = BGFX_INVALID_HANDLE;  // the SAMPLER2D(s_tex, 0) of fs_imgui.sc
};

}  // namespace starmap::render
