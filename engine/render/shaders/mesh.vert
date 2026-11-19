#version 450
layout(push_constant) uniform Push { mat4 mvp; } pc;
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv;
layout(location = 3) in float light;   // lamp light: red + 256 * green + 65536 * blue, each 0..255 for 0..2
layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUv;
layout(location = 2) out float vDistance;
layout(location = 3) out vec3 vLight;
void main() {
    vNormal = normal;
    vUv = uv;
    float r = mod(light, 256.0), g = mod(floor(light / 256.0), 256.0), b = floor(light / 65536.0);
    vLight = vec3(r, g, b) / 127.5;
    gl_Position = pc.mvp * vec4(position, 1.0);
    vDistance = gl_Position.w;   // distance along the view direction, in world units
}
