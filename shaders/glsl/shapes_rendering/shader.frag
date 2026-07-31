#version 430
#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) flat in int in_instance;
layout(location = 1) in vec2 in_position;

struct gpu_instance {
    float x0, y0;       // First  vertex pos
    float x1, y1;       // Second vertex pos
    float x2, y2;       // Third  vertex pos
    float r, g, b, a;   // RGBA color
    float cx, cy;       // Bounding circle center
    float radius;       // Circle radius
    float pad[3];
};

layout(push_constant) uniform PushConstants {
    layout(offset = 4) uint buffer_index;
} pc;

layout(set = 0, binding = 1) readonly buffer InstancesBuffer {
    gpu_instance instances[];
} buffers[];

layout(location = 0) out vec4 out_color;

void main() {
    gpu_instance instance = buffers[(pc.buffer_index)].instances[in_instance];
    if (instance.radius > 0.0 && distance(in_position, vec2(instance.cx, instance.cy)) > instance.radius) discard;
    out_color = vec4(instance.r, instance.g, instance.b, instance.a);
}
