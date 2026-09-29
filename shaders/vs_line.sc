$input a_position, a_color0
$output v_color0

// =============================================================================
// shaders/vs_line.sc — vertex shader for the orientation guides (distance
// rings in the galactic plane and the galactic-centre direction).
//
// The simplest possible vertex shader: transform a world-space point by the
// combined view-projection matrix and pass the vertex colour through.
// u_viewProj is filled by bgfx from the matrices given to
// bgfx::setViewTransform() for the view this draw call is submitted to.
// =============================================================================

#include <bgfx_shader.sh>

void main()
{
	gl_Position = mul(u_viewProj, vec4(a_position, 1.0));
	v_color0    = a_color0;
}
