#version 450
// One full-screen triangle scaled to keep the image's aspect ratio.
layout(push_constant) uniform Push { vec2 scale; } pc;
layout(location = 0) out vec2 uv;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));   // (0,0) (2,0) (0,2)
    uv = p;
    gl_Position = vec4((p * 2.0 - 1.0) * pc.scale, 0.0, 1.0);
}
