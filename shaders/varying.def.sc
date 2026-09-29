// =============================================================================
// shaders/varying.def.sc — the "interface contract" for ALL starmap shaders.
//
// WHY THIS FILE EXISTS
// --------------------
// bgfx compiles one shader source for many backends (GLSL, SPIR-V/Vulkan,
// HLSL/Direct3D, Metal). Those languages declare vertex inputs and
// vertex->fragment outputs very differently (GLSL `in/out`, HLSL semantics in
// structs, Metal [[attribute(n)]]...). bgfx's answer: declare every variable
// ONCE here, with a type and an HLSL-style *semantic*, and let `shaderc`
// generate the backend-specific declarations. Each .sc shader then only lists
// the names it uses in its `$input` / `$output` lines.
//
// NAMING RULES (enforced by shaderc — other names are rejected!)
//   a_*  vertex attributes, i.e. per-VERTEX data read from a vertex buffer.
//        Allowed names are fixed: a_position, a_normal, a_tangent, a_bitangent,
//        a_color0..3, a_indices, a_weight, a_texcoord0..15. They must match the
//        bgfx::Attrib::* you put in the C++ bgfx::VertexLayout.
//   i_*  per-INSTANCE data (i_data0, i_data1, ...), see below.
//   v_*  "varyings": outputs of the vertex shader that the rasterizer
//        interpolates across the triangle and feeds to the fragment shader.
//        The "= value" part is a default used by some backends if the vertex
//        shader forgets to write the varying.
//
// INSTANCE DATA CONVENTION (important, and a classic source of confusion)
// -----------------------------------------------------------------------
// bgfx hands per-instance data to the vertex shader as consecutive vec4
// attributes named i_data0, i_data1, ... Each one is exactly 16 bytes, which is
// why the instance *stride* passed to bgfx::allocInstanceDataBuffer() must be a
// multiple of 16 (we use 48 bytes = 3 x vec4 per star).
//   * Classic bgfx (and most tutorials you'll find) allowed 5 of them,
//     i_data0..i_data4, bound to the semantics TEXCOORD7 down to TEXCOORD3 —
//     that's why old tutorials say "max 80 bytes per instance" and why you must
//     not use a_texcoord3..7 together with instancing there.
//   * The bgfx version pinned by this project (API 1.161) raised this to 16
//     (i_data0..i_data15) and moved them to TEXCOORD31, 30, 29, ... counting
//     DOWN (see BGFX_CONFIG_INSTANCE_DATA_FIRST_TEXCOORD in bgfx/src/config.h),
//     so they no longer collide with a_texcoord0..15.
// The semantic numbers below MUST follow that rule: i_data0 = TEXCOORD31,
// i_data1 = TEXCOORD30, i_data2 = TEXCOORD29. (Copying the old TEXCOORD7/6 values from an older
// tutorial compiles fine but the instance data silently arrives as zeros.)
// =============================================================================

// ---- varyings (vertex shader -> fragment shader) -----------------------------
vec4 v_color0    : COLOR0    = vec4(1.0, 1.0, 1.0, 1.0);  // star / line colour
vec4 v_texcoord0 : TEXCOORD0 = vec4(0.0, 0.0, 0.0, 0.0);  // xy = quad corner in [-1,1], z = ring marker, w = radius px

// ---- per-vertex attributes -----------------------------------------------------
vec3 a_position  : POSITION;   // star quad: corner (x,y in [-1,1], z = 0); lines: world position (pc)
vec4 a_color0    : COLOR0;     // lines only: RGBA8, normalized to 0..1 by the vertex layout

// ---- per-instance attributes (stars only) ----------------------------------------
vec4 i_data0     : TEXCOORD31; // xyz = galactic position (parsecs), w = display radius (pc)
vec4 i_data1     : TEXCOORD30; // rgb = colour, w = intensity (DisplayColor.a; > 1 = emphasised)
vec4 i_data2     : TEXCOORD29; // x = ring marker (1 for Sol), yzw = reserved (selection, twinkle phase...)
