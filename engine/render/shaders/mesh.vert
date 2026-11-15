#version 450
layout(push_constant) uniform Push { mat4 mvp; } pc;
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv;
layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUv;
void main() {
    vNormal = normal;
    vUv = uv;
    gl_Position = pc.mvp * vec4(position, 1.0);
}
