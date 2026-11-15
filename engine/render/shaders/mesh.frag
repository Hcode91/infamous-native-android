#version 450
layout(set = 0, binding = 0) uniform sampler2D colorMap;
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUv;
layout(location = 0) out vec4 color;
void main() {
    // Preview shading: colour map times a fixed sun (two-sided) plus a little sky light from above.
    vec4 t = texture(colorMap, vUv);
    vec3 n = normalize(vNormal);
    float sun = abs(dot(n, normalize(vec3(0.35, 0.45, 0.82))));
    float sky = 0.5 + 0.5 * n.z;
    color = vec4(t.rgb * (0.25 + 0.60 * sun + 0.25 * sky), 1.0);
}
