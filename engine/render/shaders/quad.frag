#version 450
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main() {
    if (uv.x > 1.0 || uv.y > 1.0) discard;
    vec4 t = texture(tex, uv);
    // checkerboard behind transparent texels so alpha is visible
    vec2 c = floor(gl_FragCoord.xy / 16.0);
    float k = mod(c.x + c.y, 2.0) * 0.08 + 0.10;
    color = vec4(mix(vec3(k), t.rgb, t.a), 1.0);
}
