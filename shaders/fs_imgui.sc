$input v_color0, v_texcoord0

// =============================================================================
// shaders/fs_imgui.sc — fragment shader for Dear ImGui
// =============================================================================
//
// Every ImGui triangle is "texture colour x vertex colour":
//   * text:   the font atlas holds white glyphs with coverage in alpha, so
//             the result is the text colour with anti-aliased edges;
//   * shapes: rectangles, lines and circles sample a fully white texel that
//             ImGui reserves in the atlas (its UV points there), so the result
//             is just the vertex colour — one shader for everything, no
//             texture/no-texture branch or state change needed.
//
// SAMPLER2D(s_tex, 0) declares a 2D texture + sampler bound to stage 0.
// The C++ side binds a texture to it with bgfx::setTexture(0, s_tex, handle).
// (bgfx needs the macro form because D3D11+ separates textures from
// samplers, while GLSL combines them.)
//
// Blending (src_alpha, 1 - src_alpha) is configured in C++ via setState; the
// shader only produces the colour. ImGui's colours are sRGB and our back
// buffer is a plain (non-sRGB) RGBA8 target, so no conversion is needed.
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(s_tex, 0);

void main()
{
	vec4 texel = texture2D(s_tex, v_texcoord0);
	gl_FragColor = texel * v_color0;
}
