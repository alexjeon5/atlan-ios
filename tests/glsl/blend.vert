// Synthetic test shader (written for this repository).
uniform mat4 u_mvp;
attribute vec4 a_pos;
attribute vec2 a_uv0, a_uv1;
varying vec2 v_uv0, v_uv1;

void main()
{
	v_uv0 = a_uv0.st;
	v_uv1 = a_uv1.st;
	gl_Position = u_mvp * a_pos;
}
