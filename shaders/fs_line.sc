$input v_color0

// =============================================================================
// shaders/fs_line.sc — fragment shader for guide lines: output the
// interpolated vertex colour. Alpha is used (BGFX_STATE_BLEND_ALPHA) so the
// guides can be drawn faintly.
// =============================================================================

#include <bgfx_shader.sh>

void main()
{
	gl_FragColor = v_color0;
}
