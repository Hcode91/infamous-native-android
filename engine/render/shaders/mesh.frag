#version 450
layout(set = 0, binding = 0) uniform sampler2D colorMap;
// Scene lighting, set from the time of day: ambient light, a directional light and distance haze.
layout(push_constant) uniform Push {
    layout(offset = 64) vec4 ambient;   // rgb, w = distance where the haze starts
    vec4 sun;                           // rgb, w = distance where the haze is complete
    vec4 toSun;                         // unit vector towards the light, w = draw mode (0 opaque, 1 cutout, 2 decal)
    vec4 haze;                          // rgb, w = haze opacity half way between the two distances
} pc;
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUv;
layout(location = 2) in float vDistance;
layout(location = 3) in vec3 vLight;
layout(location = 0) out vec4 color;
void main() {
    int mode = int(pc.toSun.w + 0.5);
    vec4 t = texture(colorMap, vUv);
    if (mode == 1 && t.a < 0.5) discard;
    // The normal's length carries the vertex's baked ambient light: length = 2 - ambient (3: see below).
    float nl = length(vNormal);
    float baked = clamp(2.0 - nl, 0.0, 1.0);
    vec3 n = vNormal / max(nl, 1e-4);
    // Many meshes are single-sided cards or have flipped normals: light them from both sides, the back at half.
    float d = dot(n, pc.toSun.xyz);
    float lit = d > 0.0 ? d : -0.5 * d;
    vec3 c = t.rgb * (pc.ambient.rgb * baked + pc.sun.rgb * lit + vLight);
    if (nl > 2.5) c = t.rgb;   // a surface with its own light (length 3): shown as it is
    // Haze: none before the start distance, complete at the end distance, a curve through the middle value.
    float span = max(pc.sun.w - pc.ambient.w, 1.0);
    float x = clamp((vDistance - pc.ambient.w) / span, 0.0, 1.0);
    float mid = pc.haze.w;
    float f = x * (4.0 * mid - 1.0) + x * x * (2.0 - 4.0 * mid);   // 0 at 0, mid at 0.5, 1 at 1
    c = mix(c, pc.haze.rgb, clamp(f, 0.0, 1.0));
    // Only decals blend; elsewhere the alpha channel often holds other data (specular masks).
    color = vec4(c, mode == 2 ? t.a : 1.0);
}
