#version 450

layout(location = 0) out vec3 vColor;

layout(push_constant) uniform PushConstants {
    float angle;
} pc;

vec2 positions[9] = vec2[](
    vec2(0.0, -0.5),
    vec2(0.5, 0.5),
    vec2(-0.5, 0.5),
    vec2(-0.5, -0.5),
    vec2(0.5, -0.5),
    vec2(0.5, 0.5),
    vec2(0.5, 0.5),
    vec2(-0.5, 0.5),
    vec2(-0.5, -0.5)
);

vec3 colors[9] = vec3[](
    vec3(1.0, 0.0, 0.0),
    vec3(0.0, 1.0, 0.0),
    vec3(0.0, 0.0, 1.0),
    vec3(1.0, 0.0, 0.0),
    vec3(0.0, 1.0, 0.0),
    vec3(0.0, 0.0, 1.0),
    vec3(0.0, 0.0, 1.0),
    vec3(1.0, 1.0, 0.0),
    vec3(1.0, 0.0, 0.0)
);

void main()
{
    vec2 p = positions[gl_VertexIndex];
    float c = cos(pc.angle);
    float s = sin(pc.angle);
    vec2 rotated = vec2(c * p.x - s * p.y, s * p.x + c * p.y);
    gl_Position = vec4(rotated, 0.0, 1.0);
    vColor = colors[gl_VertexIndex];
}
