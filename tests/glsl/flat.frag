uniform lowp vec4 u_color;
uniform bool u_enabled;
const float k_half = 0.5;
void main() { gl_FragColor = u_enabled ? u_color * k_half : vec4(mod(7.0, 4.0), 0.0, 0.0, 1.0); }
