#version 430
#extension GL_EXT_nonuniform_qualifier : require

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
    uint buffer_index;
} pc;

layout(set = 0, binding = 1) readonly buffer InstancesBuffer {
    gpu_instance instances[];
} buffers[];

layout(location = 0) flat out int out_instance;
layout(location = 1) out vec2 out_position;

vec2 get_pos(gpu_instance instance) {
    if (gl_VertexIndex == 0) return vec2(instance.x0, instance.y0);
    if (gl_VertexIndex == 1) return vec2(instance.x1, instance.y1);
    return vec2(instance.x2, instance.y2);
}

void main() {
    gpu_instance instance = buffers[nonuniformEXT(pc.buffer_index)].instances[gl_InstanceIndex];
    gl_Position   = vec4(get_pos(instance), 0.0, 1.0);
    out_instance  = gl_InstanceIndex;
    out_position  = gl_Position.xy;
}
