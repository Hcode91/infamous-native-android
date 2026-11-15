#version 450
layout(location = 0) in vec3 vNormal;
layout(location = 1) in float vHeight;
layout(location = 0) out vec4 color;
void main() {
    // Untextured preview shading: a fixed sun, two-sided, plus a little sky light from above.
    vec3 n = normalize(vNormal);
    float sun = abs(dot(n, normalize(vec3(0.35, 0.45, 0.82))));
    float sky = 0.5 + 0.5 * n.z;
    vec3 base = vec3(0.74, 0.72, 0.68);
    color = vec4(base * (0.18 + 0.62 * sun + 0.20 * sky), 1.0);
}
