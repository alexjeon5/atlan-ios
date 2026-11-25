// Synthetic test shader (written for this repository).
precision mediump float;
uniform sampler2D u_a, u_b;
uniform float u_mixAmount;
uniform vec2 u_blurDir;
uniform int u_mode;
uniform int u_swap;
varying vec2 v_uv0, v_uv1;

void main()
{
	vec4 ca = texture2D(u_a, v_uv0);
	vec4 cb = texture2D(u_b, v_uv1);
	if (u_mode == 0)
	{
		gl_FragColor = mix(ca, cb, u_mixAmount);
	}
	else if (u_mode == 1)
	{
		vec4 sum = vec4(0.0, 0.0, 0.0, 0.0);
		sum += texture2D(u_a, vec2(v_uv0.x - u_blurDir.x, v_uv0.y)) * 0.25;
		sum += texture2D(u_a, v_uv0) * 0.5;
		sum += texture2D(u_a, vec2(v_uv0.x + u_blurDir.x, v_uv0.y)) * 0.25;
		gl_FragColor = sum;
	}
	else
	{
		gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
	}

	if (u_swap == 1)
	{
		vec4 d = gl_FragColor;
		gl_FragColor = vec4(d.z, d.y, d.x, d.w);
	}
}
