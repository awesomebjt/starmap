$input a_position, i_data0, i_data1, i_data2
$output v_color0, v_texcoord0

// =============================================================================
// shaders/vs_star.sc — vertex shader for instanced, camera-facing star billboards.
//
// ROLE: turns ONE unit quad (4 vertices, shared by all stars) + ONE instance
// record per star (position, size, colour) into a screen-aligned square around
// each star. The GPU runs this program 4 x N times per frame (4 corners for each
// of the N ~ 92,000 instances); the rasterizer then fills the squares and runs
// fs_star.sc for every covered pixel.
//
// THE .sc DIALECT IN 30 SECONDS
//   * It is GLSL-flavoured C, preprocessed by shaderc, then translated to the
//     target language. Types are GLSL (vec2/vec3/vec4/mat4, float).
//   * `$input` / `$output` (first lines, must come BEFORE any #include) list the
//     variables from varying.def.sc this stage consumes / produces.
//   * `#include <bgfx_shader.sh>` defines portability helpers and the uniforms
//     bgfx fills in automatically from bgfx::setViewTransform():
//       u_view (world->view), u_proj (view->clip), u_viewProj (both),
//       u_viewRect (viewport x,y,w,h in pixels) and friends.
//   * Always multiply with mul(matrix, vector), never `matrix * vector`:
//     HLSL and GLSL disagree on what `*` means for matrices, mul() is the
//     portable spelling that bgfx maps correctly for each backend.
//   * gl_Position is the required output: the vertex in CLIP space.
//
// SPACES A VERTEX GOES THROUGH
//   world (galactic parsecs) --u_view--> view/eye space (camera at origin,
//   looking down -Z because we build a RIGHT-handed view matrix) --u_proj-->
//   clip space (x,y,z,w). The GPU then divides by w ("perspective divide") to
//   get normalized device coordinates (NDC, x and y in [-1,1]) and maps NDC to
//   pixels using the viewport.
//
// BILLBOARDING — WHY IN CLIP SPACE?
// A billboard is a quad that always faces the camera. Two classic ways:
//   (a) world space: centre + right*x + up*y, with the camera's right/up vectors
//       taken from the view matrix (rows 0 and 1 of its rotation part). Size is
//       then in world units, i.e. perspective makes far stars tiny.
//   (b) screen/clip space (what we do): project the CENTRE only, then push the
//       four corners apart by a size measured in PIXELS. The quad is parallel to
//       the screen by construction, and we control the on-screen size directly.
// We want (b) because stars are far smaller than a pixel at most distances;
// a pure world-space size would make almost every star vanish or flicker. We
// still compute the "true" (perspective) size, clamp it to a pixel range, and
// dim stars that were enlarged by the clamp, so apparent size and brightness
// still fall off with distance (a cheap stand-in for real photometry).
// =============================================================================

#include <bgfx_shader.sh>

// Uniforms are per-draw-call constants set from C++ with bgfx::setUniform().
// bgfx uniforms are always vec4 / mat3 / mat4 / sampler, so scalar parameters
// are packed four to a vec4 (documented next to each).
uniform vec4 u_starParams;  // x = pixels per parsec at distance 1 (= 0.5 * viewportHeight * proj[1][1])
                            // y = minimum radius in pixels, z = maximum radius in pixels
                            // w = global brightness (exposure)
uniform vec4 u_starParams2; // x = exponent applied to the size-clamp dimming, y = minimum intensity
                            // z = minimum pixel radius for ring-marked stars (Sol), w = max colour intensity
uniform vec4 u_viewportPx;  // x = width, y = height (pixels), zw unused

void main()
{
	vec3  center    = i_data0.xyz;  // galactic position in parsecs
	float radiusPc  = i_data0.w;    // display radius in parsecs (see SizeSystem.cpp)
	float intensity = i_data1.w;    // brightness multiplier chosen by the colour scheme
	float ring      = i_data2.x;    // 1 = draw the highlight ring (Sol)

	// 1) Project the star centre: world -> view -> clip.
	vec4 viewPos = mul(u_view, vec4(center, 1.0));
	vec4 clip    = mul(u_proj, viewPos);

	// For a perspective projection clip.w equals the distance along the viewing
	// axis (-viewPos.z in our right-handed view space). Stars behind the camera
	// have w <= 0; clamp to avoid dividing by zero. (Those vertices are clipped by
	// the GPU anyway, because their z/w lands outside the clip volume.)
	float depth = max(clip.w, 1.0e-4);

	// 2) On-screen radius. Pinhole camera: size_px = size_world * focal_px / depth,
	//    with focal_px = 0.5 * viewportHeight / tan(fovY/2) (precomputed on the CPU).
	float truePx = radiusPc * u_starParams.x / depth;
	// Emphasised stars (intensity > 1, e.g. planet hosts in the Exoplanets
	// scheme) also get a larger minimum size, so a sparse subset stays findable.
	float emphasis = max(1.0, sqrt(intensity));
	float minPx  = mix(u_starParams.y, u_starParams2.z, step(0.5, ring)) * emphasis;
	float px     = clamp(truePx, minPx, max(u_starParams.z, minPx));

	// 3) A star drawn bigger than its true size would look too bright; scale its
	//    intensity down by (true/drawn)^k. k = 2 would conserve energy exactly
	//    (area ratio) but makes distant stars invisible; k ~ 1 reads better.
	float dim = pow(clamp(truePx / px, 0.0, 1.0), u_starParams2.x);
	dim = max(dim, u_starParams2.y);

	// 4) Offset this corner in NDC: a_position.xy is -1 or +1, a pixel is
	//    2/width NDC units wide. Clip-space offset = NDC offset * w (so that the
	//    perspective divide the GPU performs later cancels out exactly).
	vec2 ndcOffset = a_position.xy * px * 2.0 / u_viewportPx.xy;
	clip.xy += ndcOffset * clip.w;

	gl_Position = clip;
	v_texcoord0 = vec4(a_position.xy, ring, px);
	// Additive blending ignores alpha, so intensity is folded into rgb here.
	v_color0    = vec4(i_data1.rgb * (min(intensity, u_starParams2.w) * dim * u_starParams.w), 1.0);
}
