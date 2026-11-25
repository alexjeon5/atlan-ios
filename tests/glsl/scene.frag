// Synthetic test shader (written for this repository).
precision mediump float;
uniform sampler2D u_tex;
uniform vec4 u_lightPos[2];
uniform vec4 u_diffuse[2];
uniform vec4 u_ambient;
uniform int u_lightCount;
uniform float u_fogNear;
uniform float u_fogFar;
uniform vec4 u_fogColor;
uniform int u_fog;
uniform int u_clip;
uniform vec2 u_scale;

varying vec3 v_pos;
varying vec4 v_color;
varying vec3 v_normal;
varying vec2 v_uv;
varying float v_flat;
varying vec4 v_eye;

float fogFactor()
{
	float d = 0.0;
	if (v_flat != 1.0)
		d = length(v_eye.xyz);
	return clamp((u_fogFar - d) / (u_fogFar - u_fogNear), 0.0, 1.0);
}

void main()
{
	if (u_clip == 1 && v_pos.x > 1.0)
		discard;

	vec4 base = texture2D(u_tex, v_uv * u_scale) * v_color;
	vec4 lit = vec4(0.0);
	for (int i = 0; i < 2; i++)
	{
		if (i >= u_lightCount)
			break;
		vec3 l;
		if (u_lightPos[i][3] == 0.0)
			l = normalize(vec3(u_lightPos[i]));
		else
			l = normalize(vec3(u_lightPos[i]) / u_lightPos[i][3] - v_pos);
		lit += max(dot(v_normal, l), 0.0) * u_diffuse[i];
	}
	if (u_lightCount > 0)
		base = vec4(vec3(base * (lit + u_ambient)), base.a);

	if (u_fog == 1)
	{
		float k = fogFactor();
		base = vec4(vec3(mix(u_fogColor, base, k)), base[3]);
	}
	gl_FragColor = base;
}
