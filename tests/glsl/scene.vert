// Synthetic test shader (written for this repository).
uniform mat4 u_mvp;
uniform mat4 u_view;
uniform mat4 u_normal;
uniform vec4 u_tint;
uniform vec4 u_eye;

attribute vec4 a_pos;
attribute vec3 a_normal;
attribute vec2 a_uv;
attribute vec4 a_extra;

varying vec3 v_pos;
varying vec4 v_color;
varying vec3 v_normal;
varying vec2 v_uv;
varying float v_flat;
varying vec4 v_eye;

void main()
{
	v_pos = vec3(u_mvp * a_pos);
	v_color = vec4(vec3(1.0, 1.0, 1.0), a_extra / 100.0);
	v_normal = normalize(vec3(u_normal * vec4(a_normal, 0.0)));
	if (u_view == mat4(1.0))
	{
		v_flat = 1.0;
	}
	else
	{
		v_flat = 0.0;
		v_eye = u_view * a_pos - u_eye;
	}
	v_uv = a_uv.st;
	gl_Position = u_mvp * a_pos;
}
