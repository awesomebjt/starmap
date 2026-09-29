// =============================================================================
// render/ImGuiRenderer.cpp — Dear ImGui -> bgfx (see ImGuiRenderer.h for the
// big picture: why a dedicated Sequential view and transient buffers).
// =============================================================================
#include "render/ImGuiRenderer.h"

#include "render/ShaderLoader.h"

#include <bx/math.h>
#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace starmap::render {
namespace {

// ---- ImTextureID <-> bgfx::TextureHandle -------------------------------------
// ImGui refers to textures through an opaque ImTextureID (a 64-bit integer in
// 1.92). It reserves the value 0 as "invalid" (ImTextureID_Invalid). A bgfx
// TextureHandle is a 16-bit index for which 0 IS valid (the first texture
// ever created). So we store idx + 1: never 0 for a real texture, and the
// mapping stays a trivial, allocation-free integer conversion.
ImTextureID to_imgui_id(bgfx::TextureHandle h) { return static_cast<ImTextureID>(h.idx) + 1; }
bgfx::TextureHandle to_bgfx(ImTextureID id) { return {static_cast<uint16_t>(id - 1)}; }

// ---- standard draw callbacks ----------------------------------------------------
// User code can insert a callback into a draw list (ImDrawList::AddCallback)
// to run custom rendering between ImGui's triangles. ImGui 1.92.8+ also
// defines a few STANDARD callbacks that each backend provides through
// ImGuiPlatformIO, e.g. DrawCallback_ResetRenderState ("put your render state
// back to default"). The function body is irrelevant: the backend recognises
// the callback by its ADDRESS in the render loop (the same trick the official
// OpenGL/SDL_GPU backends use). Our render state is set from scratch for
// every draw command anyway, so resetting is a no-op for us.
void reset_render_state_marker(const ImDrawList*, const ImDrawCmd*) {}

}  // namespace

ImGuiRenderer::ImGuiRenderer(const ShaderLoader& shaders, bgfx::ViewId view) : view_(view) {
    // ---- vertex layout: must describe ImDrawVert byte for byte --------------
    //   struct ImDrawVert { ImVec2 pos; ImVec2 uv; ImU32 col; };  // 20 bytes
    // Order of add() calls = order in memory. Colour is 4 unsigned bytes that
    // the GPU normalises to 0..1 (the `true` argument), so the shader sees a
    // vec4. ImU32 colours are packed as 0xAABBGGRR, i.e. bytes R,G,B,A in
    // memory on little-endian machines — the order Color0/Uint8x4 expects.
    layout_.begin()
        .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .end();
    // A layout/struct mismatch would draw garbage without any error message,
    // so check it at run time (sizeof is compile-time, the stride isn't).
    IM_ASSERT(layout_.getStride() == sizeof(ImDrawVert));

    // "s_tex" must match the SAMPLER2D name in fs_imgui.sc. Sampler uniforms
    // tell bgfx which shader slot a texture binds to.
    s_tex_ = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);
    program_ = shaders.load_program("vs_imgui", "fs_imgui");

    // ---- tell ImGui what this backend can do -----------------------------------
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "starmap_bgfx";
    // RendererHasVtxOffset: we honour ImDrawCmd::VtxOffset (bgfx supports a
    // start vertex per draw), so a single window may exceed 65,536 vertices
    // even though indices are 16-bit.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    // RendererHasTextures (new in 1.92): we process texture create/update/
    // destroy requests from ImDrawData::Textures. This is what lets ImGui
    // rasterise glyphs lazily at the size and DPI actually used, instead of
    // baking one fixed-size font atlas up front.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    // Largest texture bgfx allows on this GPU, so ImGui never asks for more.
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    pio.Renderer_TextureMaxWidth = pio.Renderer_TextureMaxHeight = static_cast<int>(bgfx::getCaps()->limits.maxTextureSize);
    pio.DrawCallback_ResetRenderState = reset_render_state_marker;
}

ImGuiRenderer::~ImGuiRenderer() {
    // Destroy every texture we created that is still alive. RefCount == 1
    // means only ImGui's own atlas references it (the pattern used by the
    // official backends' shutdown code).
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
        if (tex->RefCount == 1 && tex->TexID != ImTextureID_Invalid) destroy_texture(tex);
    }
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = nullptr;
    ImGui::GetPlatformIO().DrawCallback_ResetRenderState = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
    if (bgfx::isValid(program_)) bgfx::destroy(program_);
    if (bgfx::isValid(s_tex_)) bgfx::destroy(s_tex_);
}

void ImGuiRenderer::destroy_texture(ImTextureData* tex) {
    bgfx::destroy(to_bgfx(tex->TexID));
    // Tell ImGui the GPU copy is gone, so it could re-create it later.
    tex->SetTexID(ImTextureID_Invalid);
    tex->SetStatus(ImTextureStatus_Destroyed);
}

void ImGuiRenderer::update_texture(ImTextureData* tex) {
    // ImGui 1.92's texture protocol: each ImTextureData carries a Status that
    // tells the backend what to do this frame.
    //   WantCreate  -> create a GPU texture and upload all pixels
    //   WantUpdates -> upload the changed sub-rectangles (new glyphs baked)
    //   WantDestroy -> free it (once it's no longer used by in-flight frames)
    // After handling, the backend sets the status back to OK.
    if (tex->Status == ImTextureStatus_WantCreate) {
        IM_ASSERT(tex->Format == ImTextureFormat_RGBA32);  // the default; we never ask for Alpha8
        // Pitfall: a bgfx texture created WITH initial data (a non-null
        // bgfx::Memory) is IMMUTABLE — updateTexture2D on it fails. The font
        // atlas grows and changes, so create it empty (nullptr) ...
        const bgfx::TextureHandle h = bgfx::createTexture2D(
            static_cast<uint16_t>(tex->Width), static_cast<uint16_t>(tex->Height), false, 1,
            bgfx::TextureFormat::RGBA8, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, nullptr);
        bgfx::setName(h, "ImGui texture");
        // ... and upload the pixels with an update. bgfx::copy() because bgfx
        // processes the upload later (at frame()), after ImGui may already
        // have changed its buffer.
        const uint32_t bytes = static_cast<uint32_t>(tex->Width * tex->Height * tex->BytesPerPixel);
        bgfx::updateTexture2D(h, 0, 0, 0, 0, static_cast<uint16_t>(tex->Width), static_cast<uint16_t>(tex->Height),
                              bgfx::copy(tex->GetPixels(), bytes));
        tex->SetTexID(to_imgui_id(h));
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantUpdates) {
        const bgfx::TextureHandle h = to_bgfx(tex->TexID);
        // Each update is a rectangle inside the atlas. Its rows are not
        // contiguous in ImGui's buffer (the atlas row is wider), so pack the
        // rectangle's rows into a tight block for bgfx.
        for (const ImTextureRect& r : tex->Updates) {
            const uint32_t row_bytes = static_cast<uint32_t>(r.w) * static_cast<uint32_t>(tex->BytesPerPixel);
            const bgfx::Memory* mem = bgfx::alloc(row_bytes * r.h);
            for (int y = 0; y < r.h; ++y) {
                std::memcpy(mem->data + static_cast<std::size_t>(y) * row_bytes, tex->GetPixelsAt(r.x, r.y + y), row_bytes);
            }
            bgfx::updateTexture2D(h, 0, 0, r.x, r.y, r.w, r.h, mem, static_cast<uint16_t>(row_bytes));
        }
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0) {
        // UnusedFrames > 0: no draw command of the frame being built uses it.
        destroy_texture(tex);
    }
}

void ImGuiRenderer::render(ImDrawData* draw_data) {
    if (!draw_data) return;

    // ---- 1. texture requests first -----------------------------------------------
    // Must happen BEFORE the draw calls below read tex IDs (a brand-new
    // atlas has no ID until we create it).
    if (draw_data->Textures) {
        for (ImTextureData* tex : *draw_data->Textures) {
            if (tex->Status != ImTextureStatus_OK) update_texture(tex);
        }
    }

    // ---- 2. view setup ----------------------------------------------------------------
    // DisplaySize is in POINTS (logical units, what ImGui lays out in);
    // FramebufferScale converts to PIXELS (2.0 on a Retina display). The
    // viewport and scissor rectangles are in pixels; the projection maps
    // ImGui's point coordinates onto that pixel viewport.
    const float fb_w = draw_data->DisplaySize.x * draw_data->FramebufferScale.x;
    const float fb_h = draw_data->DisplaySize.y * draw_data->FramebufferScale.y;
    if (fb_w <= 0.0f || fb_h <= 0.0f) return;  // minimised window

    bgfx::setViewName(view_, "ImGui");
    bgfx::setViewMode(view_, bgfx::ViewMode::Sequential);  // keep submission order (see header)
    bgfx::setViewRect(view_, 0, 0, static_cast<uint16_t>(fb_w), static_cast<uint16_t>(fb_h));

    // Orthographic projection from ImGui space to clip space:
    //   x: DisplayPos.x .. +DisplaySize.x  ->  -1 .. +1
    //   y: DisplayPos.y .. +DisplaySize.y  ->  +1 .. -1   (bottom/top swapped:
    //      ImGui's y points DOWN, clip space's y points UP)
    // homogeneousDepth selects the depth range convention of the backend
    // ([-1,1] for OpenGL, [0,1] for the others). z doesn't matter for a flat
    // UI, but passing the right flag keeps the matrix valid everywhere.
    const ImVec2 pos = draw_data->DisplayPos;
    float ortho[16];
    bx::mtxOrtho(ortho, pos.x, pos.x + draw_data->DisplaySize.x, pos.y + draw_data->DisplaySize.y, pos.y, 0.0f, 1000.0f,
                 0.0f, bgfx::getCaps()->homogeneousDepth);
    bgfx::setViewTransform(view_, nullptr, ortho);  // nullptr view matrix = identity

    // Render state, the same for every ImGui draw:
    //   WRITE_RGB, but NOT WRITE_A: keep the back buffer's alpha at 1 (the
    //     clear value). Blending alpha into it would make PNG screenshots
    //     (which store alpha) semi-transparent where the UI is.
    //   BLEND_ALPHA: out = src.rgb * src.a + dst.rgb * (1 - src.a), classic
    //     "over" compositing for anti-aliased text and translucent windows.
    //   No depth test/write: the UI is always on top and ordered by submission.
    //   MSAA: harmless if the back buffer has no multisampling.
    const uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_MSAA | BGFX_STATE_BLEND_ALPHA;
    const bool index32 = sizeof(ImDrawIdx) == 4;  // ImGui can be compiled with 32-bit indices

    // ---- 3. one pass per command list (roughly: per ImGui window) -----------------
    for (const ImDrawList* list : draw_data->CmdLists) {
        const auto num_vertices = static_cast<uint32_t>(list->VtxBuffer.Size);
        const auto num_indices = static_cast<uint32_t>(list->IdxBuffer.Size);

        // Transient memory is a fixed per-frame pool (Graphics.cpp sets its
        // size). Ask before allocating: if the pool is exhausted, skip the
        // rest of the UI for this frame rather than crash.
        if (bgfx::getAvailTransientVertexBuffer(num_vertices, layout_) < num_vertices ||
            bgfx::getAvailTransientIndexBuffer(num_indices, index32) < num_indices) {
            break;
        }
        bgfx::TransientVertexBuffer tvb;
        bgfx::TransientIndexBuffer tib;
        bgfx::allocTransientVertexBuffer(&tvb, num_vertices, layout_);
        bgfx::allocTransientIndexBuffer(&tib, num_indices, index32);
        std::memcpy(tvb.data, list->VtxBuffer.Data, num_vertices * sizeof(ImDrawVert));
        std::memcpy(tib.data, list->IdxBuffer.Data, num_indices * sizeof(ImDrawIdx));

        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback) {
                // Callbacks let user code inject custom rendering into the UI.
                // The ResetRenderState marker means "restore your state" —
                // our state is set per draw anyway, so nothing to do.
                if (cmd.UserCallback != reset_render_state_marker) cmd.UserCallback(list, &cmd);
                continue;
            }
            if (cmd.ElemCount == 0) continue;

            // Clip rectangle: ImGui space -> framebuffer pixels, clamped to the
            // viewport. The scissor test discards fragments outside it — this
            // is how a scrolled panel hides the widgets scrolled out of view.
            const ImVec2 scale = draw_data->FramebufferScale;
            const float x0 = std::max((cmd.ClipRect.x - pos.x) * scale.x, 0.0f);
            const float y0 = std::max((cmd.ClipRect.y - pos.y) * scale.y, 0.0f);
            const float x1 = std::min((cmd.ClipRect.z - pos.x) * scale.x, fb_w);
            const float y1 = std::min((cmd.ClipRect.w - pos.y) * scale.y, fb_h);
            if (x1 <= x0 || y1 <= y0) continue;  // fully clipped
            bgfx::setScissor(static_cast<uint16_t>(x0), static_cast<uint16_t>(y0),
                             static_cast<uint16_t>(x1 - x0), static_cast<uint16_t>(y1 - y0));

            bgfx::setState(state);
            // GetTexID() resolves ImGui's texture reference to the ID we
            // assigned in update_texture().
            bgfx::setTexture(0, s_tex_, to_bgfx(cmd.GetTexID()));
            // VtxOffset: this command's indices are relative to that vertex
            // (the RendererHasVtxOffset feature). IdxOffset/ElemCount select
            // the command's slice of the index buffer.
            bgfx::setVertexBuffer(0, &tvb, cmd.VtxOffset, num_vertices - cmd.VtxOffset);
            bgfx::setIndexBuffer(&tib, cmd.IdxOffset, cmd.ElemCount);
            // submit() consumes the state/buffers/texture/scissor set above;
            // bgfx resets them for the next draw.
            bgfx::submit(view_, program_);
        }
    }
}

}  // namespace starmap::render
