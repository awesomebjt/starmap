// =============================================================================
// shaders/varying_imgui.def.sc — inputs/outputs of the Dear ImGui shaders
// =============================================================================
//
// A varying.def.sc file declares, once, every attribute and varying that the
// shaders compiled with it may use. shaderc reads it to generate the correct
// input/output declarations for each backend (GLSL "in/out", HLSL semantics,
// Metal structs). Each line is:   type name : SEMANTIC [= default];
//
// Why a separate file from varying.def.sc (the stars' one)?
//   ImGui's vertices (ImDrawVert) are 2D: { vec2 pos; vec2 uv; uint32 col; }.
//   Declaring a_position as vec2 here matches the vertex layout exactly.
//   (A vec3 declaration would also work — the GPU fills missing components
//   with 0/1 — but "declare what you feed" is the clearer habit.)
//
// Naming conventions bgfx relies on:
//   a_*  per-vertex attributes; the name selects the bgfx::Attrib slot
//        (a_position = Attrib::Position, a_texcoord0 = Attrib::TexCoord0,
//        a_color0 = Attrib::Color0), which the C++ VertexLayout must match.
//   v_*  varyings: written by the vertex shader, interpolated across the
//        triangle, read by the fragment shader. The "= value" is a default
//        that some backends need.
// =============================================================================

vec4 v_color0    : COLOR0    = vec4(1.0, 1.0, 1.0, 1.0);
vec2 v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0);

vec2 a_position  : POSITION;
vec4 a_color0    : COLOR0;
vec2 a_texcoord0 : TEXCOORD0;
