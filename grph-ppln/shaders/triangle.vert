#version 450

const vec2[3] position = vec2[3](vec2(0.0f, -0.5f), vec2(0.5f, 0.5f), vec2(-0.5f, 0.5f));

const vec3[3] color = vec3[3](vec3(1.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f), vec3(0.0f, 0.0f, 1.0f));

layout(location = 0) out vec3 fragColor;

void main() {
    int id = gl_InstanceIndex;

    vec2 offset = id == 0 ? vec2(-0.15, 0.0) : vec2(0.15, 0.0);
    float depth = id == 0 ? 0.2 : 0.8;

    vec2 pos = position[gl_VertexIndex] * 0.9 + offset;

    gl_Position = vec4(pos, depth, 1.0);

    fragColor = id == 0 ? vec3(1.0, 0.2, 0.2) : vec3(0.2, 0.5, 1.0);
}
