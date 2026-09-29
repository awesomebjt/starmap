$input a_position, a_texcoord0, a_color0
$output v_color0, v_texcoord0

// =============================================================================
// shaders/vs_imgui.sc — vertex shader for Dear ImGui's triangles
// =============================================================================
//
// ImGui produces vertices in "screen space points": (0,0) is the top-left of
// the window and x/y grow right/down, in logical units (not pixels).
// render/ImGuiRenderer.cpp sets an ORTHOGRAPHIC projection for ImGui's bgfx
// view with bx::mtxOrtho(left = 0, right = width, bottom = height, top = 0),
// i.e. y flipped so that "down" in ImGui is "down" on screen. u_viewProj is
// that matrix (the view matrix is identity), so one multiply maps a point to
// clip space.
//
// z: ImGui is flat and drawn with depth test off, so z is forced to 0.
// Using 0 (and not, say, 0.5) is safe in both depth conventions (OpenGL's
// [-1, 1] and D3D/Metal/Vulkan's [0, 1]).
//
// The .sc dialect: $input/$output lists name the attributes/varyings from
// varying_imgui.def.sc this shader uses. mul(matrix, vector) instead of '*'
// because HLSL and GLSL disagree on the operator for matrix * vector;
// bgfx_shader.sh maps mul() correctly for every backend.
// =============================================================================

#include <bgfx_shader.sh>

void main()
{
	vec4 pos = mul(u_viewProj, vec4(a_position.xy, 0.0, 1.0));
	gl_Position = vec4(pos.xy, 0.0, 1.0);
	v_texcoord0 = a_texcoord0;
	// a_color0 arrives as 4 normalised bytes (0..255 -> 0..1) because the C++
	// vertex layout declares Color0 as Uint8 x4 with normalized = true.
	v_color0 = a_color0;
}
