$input v_color0, v_texcoord0

// =============================================================================
// shaders/fs_star.sc — fragment ("pixel") shader for star billboards.
//
// ROLE: runs once for every pixel covered by a star quad and decides its
// colour. The quad itself is a hard-edged square; this shader turns it into a
// soft round glow so stars look like points of light, not tiles.
//
// BLENDING CONTEXT (set in C++, StarRenderer::submit): BGFX_STATE_BLEND_ADD,
// i.e. framebuffer = framebuffer + our output. Consequences:
//   * black (0,0,0) output means "no contribution", so the quad's corners need
//     no alpha and no `discard` — they just add zero;
//   * overlapping stars brighten each other, like real light does;
//   * addition is commutative, so draw ORDER doesn't matter: no sorting of
//     92k stars by depth is needed (alpha "over" blending would require it).
// =============================================================================

#include <bgfx_shader.sh>

void main()
{
	vec2  uv    = v_texcoord0.xy;  // -1..+1 across the quad (interpolated from the corners)
	float ring  = v_texcoord0.z;   // 1 for Sol
	float r2    = dot(uv, uv);     // squared distance from the quad centre (0 centre, 1 edge midpoints)

	// Gaussian falloff exp(-r^2 / (2 sigma^2)). A Gaussian is the classic model of
	// a star image blurred by optics/atmosphere (the "point spread function").
	// Two Gaussians: a tight bright core plus a wide faint halo.
	float core = exp(-r2 * 10.0);          // sigma ~ 0.22 of the quad radius
	float halo = exp(-r2 * 3.0) * 0.18;    // sigma ~ 0.41, faint
	// The Gaussian never reaches exactly 0, so fade it out before the quad edge;
	// otherwise the square outline would be faintly visible on dense regions.
	float window = 1.0 - smoothstep(0.6, 1.0, r2);
	float glow = (core + halo) * window;

	vec3 color = v_color0.rgb * glow;
	// Hot white centre: real stars saturate the eye/sensor in the middle, which
	// makes them read as "bright" even when their hue is deep red. Scaled by the
	// star's average brightness so dim stars don't get a white dot.
	color += vec3_splat(core * core * 0.25) * dot(v_color0.rgb, vec3_splat(0.333));

	// Sol marker: a thin gold ring at radius 0.8 of the quad, on top of the glow.
	float r    = sqrt(r2);
	float band = exp(-((r - 0.78) * (r - 0.78)) / 0.0025) * ring;
	color += vec3(1.0, 0.8, 0.35) * band;

	// Alpha is ignored by additive blending; write 1 for tidiness.
	gl_FragColor = vec4(color, 1.0);
}
