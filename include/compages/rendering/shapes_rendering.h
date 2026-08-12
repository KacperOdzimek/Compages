/*
----------------------------------------------------------------
Contents
This file provides simple system to render basic shapes: lines, triangles, rectangles, circles.

----------------------------------------------------------------
Code info:
- cmg_shp prefix
- COMPAGES_SHAPES_RENDERING_IMPL macro to build
- fundatio/graphics.h dependant
- fundatio/linear_algebra.h dependant

----------------------------------------------------------------
Usage

- Create cmg_shp_shared object - it contains shared read-only objects for rendering.
    Create one per hardware. In create info, link shaders (provided in shader directory).
- Create cmg_shp_frames - it contains per frame geometry buffers you record with drawing methods.
- Use commands to draw shapes
- Upload data to gpu with cmg_shp_upload
- Render with graphics command cmg_shp_gcmd_render
*/

#ifndef COMPAGES_SHAPES_H
#define COMPAGES_SHAPES_H

#include "fundatio/platform/graphics.h"
#include "fundatio/mathematics/linear_algebra.h"

// Shapes Rendering Shared Object

typedef struct cmg_shp_shared_create_info {
    fnd_gfx_pipeline_attachment_state   attachment_state;
    fnd_gfx_shader_create_info          vertex_shader_info;
    fnd_gfx_shader_create_info          pixel_shader_info;
} cmg_shp_shared_create_info;

typedef struct cmg_shp_shared cmg_shp_shared;
cmg_shp_shared* cmg_shp_create_shared(fnd_gfx_hardware*, const cmg_shp_shared_create_info* info);
void cmg_shp_free_shared(cmg_shp_shared*);

// Shapes Rendering Frame Contextes

typedef struct cmg_shp_frames_create_info {
    cmg_shp_shared* shared;
    uint32_t        count;
} cmg_shp_frames_create_info;

typedef struct cmg_shp_frames cmg_shp_frames;
cmg_shp_frames* cmg_shp_create_frames(fnd_gfx_hardware*, const cmg_shp_frames_create_info* info);
void cmg_shp_free_frames(cmg_shp_frames*);

// shorthand not to pass frame in flight to every function
typedef struct cmg_shp_context {
    cmg_shp_frames*    frames;
    uint32_t        index;
    float           r, g, b, a;
    float           line_thickness;
} cmg_shp_context;

// Actuall Draw Operation

// resets draw requests in context
void cmg_shp_reset(
    cmg_shp_context* context
);

// Uploads draw requests to gpu
// Returns non-zero at success
int cmg_shp_upload(
    cmg_shp_context*    context,
    uint8_t             transfer_work_group_index,
    uint8_t             commands_allocator_index,
    fnd_gfx_staging*    staging,
    uint64_t            staging_region_offset,
    uint64_t            staging_region_size,
    fnd_gfx_timeline*   signal_timeline,
    uint64_t            signal_value
);

// command to draw from context
// needs to be re recorded every frame as amount of
// drawn primitives may change
// call with render target bound, viewport and scissors set
void cmg_shp_gcmd_render(cmg_shp_context* context);

// Shapes Draw Function

// set drawn shapes color
void cmg_shp_set_color(
    cmg_shp_context* context,
    float r, float g, float b, float a
);

void cmg_shp_set_line_thickness(
    cmg_shp_context* context,
    float line_thickness
);

void cmg_shp_line(
    cmg_shp_context* context,
    fnd_lia_vec2 begin, fnd_lia_vec2 end
);

void cmg_shp_triangle(
    cmg_shp_context* context,
    fnd_lia_vec2 a, fnd_lia_vec2 b, fnd_lia_vec2 c
);

void cmg_shp_rect(
    cmg_shp_context* context,
    fnd_lia_vec2 first_corner, fnd_lia_vec2 second_corner
);

void cmg_shp_circle(
    cmg_shp_context* context,
    fnd_lia_vec2 center, float radius
);

#endif // COMPAGES_SHAPES_H

#ifdef COMPAGES_SHAPES_RENDERING_IMPL

#include <stdlib.h>
#include <string.h>
#include <math.h>

/*
    Config
*/

typedef struct gpu_instance {
    float x0, y0;       // First  vertex pos
    float x1, y1;       // Second vertex pos
    float x2, y2;       // Third  vertex pos
    float r, g, b, a;   // RGBA color
    float cx, cy;       // Bounding circle center
    float radius;       // Circle radius
    float pad[3];
} gpu_instance;

typedef struct gpu_constants {
    uint32_t buffer_index;
} gpu_constants;

/*
    Shared
*/

struct cmg_shp_shared {
    fnd_gfx_hardware*   owning_hardware;
    fnd_gfx_pipeline*   pipeline;
};

cmg_shp_shared* cmg_shp_create_shared(fnd_gfx_hardware* hardware, const cmg_shp_shared_create_info* info) {
    cmg_shp_shared* shared = calloc(1, sizeof(cmg_shp_shared)); if (!shared) return NULL;
    shared->owning_hardware = hardware;

    // Pipeline Shaders
    fnd_gfx_shader* vertex_shader = fnd_gfx_create_shader(shared->owning_hardware, &info->vertex_shader_info);
    fnd_gfx_shader* pixel_shader  = fnd_gfx_create_shader(shared->owning_hardware, &info->pixel_shader_info);

    if (!vertex_shader || !pixel_shader) {
        fnd_gfx_free_shader(vertex_shader);
        fnd_gfx_free_shader(pixel_shader);
        goto _fail;
    }

    shared->pipeline = fnd_gfx_create_pipeline(shared->owning_hardware, &(fnd_gfx_pipeline_create_info){
        .attachment_state = info->attachment_state,
        .shader_stages  = {
            .shaders[fnd_gfx_shader_stage_vertex]   = vertex_shader,
            .constants[fnd_gfx_shader_stage_vertex] = sizeof(gpu_constants),
            .shaders[fnd_gfx_shader_stage_pixel]    = pixel_shader,
            .constants[fnd_gfx_shader_stage_pixel]  = sizeof(gpu_constants)
        },
        .input_assembler_state = {
            .topology = fnd_gfx_primitive_topology_triangle_list
        },
        .rasterizer_state = {
            .scissor_enable     = 0,
            .depth_clamp_enable = 0,
            .fill_mode          = fnd_gfx_fill_mode_solid,
            .cull_mode          = fnd_gfx_cull_mode_none
        },
        .blend_state = {
            .blend_enable   = 1,
            .blend_op       = fnd_gfx_blend_op_add,
            .src_factor     = fnd_gfx_blend_factor_src_alpha,
            .dst_factor     = fnd_gfx_blend_factor_one_minus_src_alpha,
        },
        .depth_stencil_state = {
            .depth_test_enable      = 0,
            .depth_write_enable     = 0,
            .stencil_test_enable    = 0
        }
    });

    fnd_gfx_free_shader(vertex_shader);
    fnd_gfx_free_shader(pixel_shader);

    if (!shared->pipeline) goto _fail;
    return shared;

_fail:
    cmg_shp_free_shared(shared); 
    return NULL;
}

void cmg_shp_free_shared(cmg_shp_shared* shared) {
    if (!shared) return;
    fnd_gfx_free_pipeline(shared->pipeline);
    free(shared);
}

/*
    Frames
*/

typedef struct single_frame {
    uint64_t            position;       // Arena position in gpu instances
    uint64_t            capacity;       // Arena capacity in gpu instances
    gpu_instance*       arena;          // Arena data
    uint32_t            to_draw;        // GPU instances to draw
    fnd_gfx_buffer*         buffer;         // GPU instances buffer
    uint32_t            bind;           // Buffer bind point
    fnd_gfx_commands*   upload_list;    // List used to upload instances
} single_frame;

struct cmg_shp_frames {
    fnd_gfx_hardware*   owning_hardware;
    cmg_shp_shared*    owning_shared;
    uint32_t        in_flight;
    single_frame*   frames;
};

cmg_shp_frames* cmg_shp_create_frames(fnd_gfx_hardware* hardware, const cmg_shp_frames_create_info* info) {
    if (!hardware || !info->shared) goto _fail;

    cmg_shp_frames* frames = calloc(1, sizeof(cmg_shp_frames)); 
    if (!frames) goto _fail;

    *frames = (cmg_shp_frames) {
        .owning_hardware = hardware,
        .owning_shared   = info->shared,
        .in_flight       = info->count
    };
    
    // Frames
    frames->frames = calloc(info->count, sizeof(single_frame));
    if (!frames->frames) goto _fail;

    return frames;

_fail:
    cmg_shp_free_frames(frames);
    return NULL;
}

void cmg_shp_free_frames(cmg_shp_frames* frames) {
    if (!frames) return;
    for (uint32_t i = 0; i < frames->in_flight; i++) {
        fnd_gfx_free_buffer(frames->frames[i].buffer);
        fnd_gfx_free_commands(frames->frames[i].upload_list);
        free(frames->frames[i].arena);
    }
    free(frames->frames);
    free(frames);
}

void cmg_shp_reset(cmg_shp_context* context) {
    single_frame* frame = &context->frames->frames[context->index];
    frame->position = 0;
}

typedef struct upload_params {
    fnd_gfx_staging* staging;
    fnd_gfx_buffer*         buffer;
    uint64_t            uploaded;
    uint64_t            upload;
} upload_params;

static void upload_record(void* raw_params) {
    upload_params* params = raw_params;
    fnd_gfx_tcmd_copy_staging_to_buffer(
        params->staging, params->buffer,
        0, params->uploaded * sizeof(gpu_instance), params->upload * sizeof(gpu_instance)
    );
}

static uint64_t min_u64(uint64_t l, uint64_t r) {
    return l < r ? l : r;
}

int cmg_shp_upload(
    cmg_shp_context*       context,
    uint8_t             transfer_work_group_index,
    uint8_t             commands_allocator_index,
    fnd_gfx_staging* staging,
    uint64_t            staging_region_offset,
    uint64_t            staging_region_size,
    fnd_gfx_timeline*       signal_timeline,
    uint64_t            signal_value
) {
    single_frame* frame    = &context->frames->frames[context->index];
    fnd_gfx_hardware* hardware = context->frames->owning_hardware;

    // Nothing to upload
    if (frame->position == 0) {
        fnd_gfx_timeline_signal(signal_timeline, signal_value); return 1;
    }

    // Whether succeeded to bind resources
    int bind_success = 1;

    // Ensure buffer space
    if (!frame->buffer || fnd_gfx_buffer_query_bytes(frame->buffer) < frame->position * sizeof(gpu_instance)) {
        fnd_gfx_buffer* new_buffer = NULL;
        uint64_t    new_cap[2] = {frame->capacity, frame->position};
        for (int i = 0; i < 2; i++) {
            new_buffer = fnd_gfx_create_buffer(hardware, &(fnd_gfx_buffer_create_info){
                .bytes  = new_cap[i] * sizeof(gpu_instance),
                .usage  = fnd_gfx_buffer_usage_storage,
                .access = fnd_gfx_memory_access_staging_write
            });
            if (new_buffer) break;
        }
        if (new_buffer) {
            fnd_gfx_free_buffer(frame->buffer);
            frame->buffer = new_buffer;
            frame->bind = fnd_gfx_shader_resource_bind(hardware, fnd_gfx_resource_type_storage_buffer, new_buffer, &bind_success);
        }
    }

    // Safe return
    if (!frame->buffer) {
        fnd_gfx_timeline_signal(signal_timeline, signal_value); return 0;
    }

    // Cap written instances to buffer capacity
    uint64_t instances_to_write = min_u64(fnd_gfx_buffer_query_bytes(frame->buffer) / sizeof(gpu_instance), frame->position);
    uint64_t staging_capacity   = staging_region_size / sizeof(gpu_instance);
    uint64_t buffer_uploaded    = 0;

    // If wont do in single upload, alloc internal timeline
    fnd_gfx_timeline* internal = instances_to_write > staging_capacity ? fnd_gfx_create_timeline(hardware, &(fnd_gfx_timeline_create_info){
        .initial_value = 0
    }) : NULL; if (!internal) instances_to_write = min_u64(instances_to_write, staging_capacity);
    uint64_t internal_itr = 0;

    // Upload loop
    while (instances_to_write) {
        uint64_t upload = min_u64(instances_to_write, staging_capacity);

        char* mem = fnd_gfx_staging_map(staging, staging_region_offset, staging_region_size);
        memcpy(mem, &frame->arena[buffer_uploaded], upload * sizeof(gpu_instance));
        fnd_gfx_staging_unmap(staging);

        frame->upload_list = fnd_gfx_create_commands(hardware, &(fnd_gfx_commands_create_info){
            .domain = fnd_gfx_command_domain_transfer,
            .aindex = commands_allocator_index,
            .parent = frame->upload_list,
            .record = upload_record,
            .params = &(upload_params){
                .staging  = staging,
                .buffer   = frame->buffer,
                .uploaded = buffer_uploaded,
                .upload   = upload
            }
        });

        int last_upload = instances_to_write <= staging_capacity;

        fnd_gfx_timeline* timeline = last_upload ? signal_timeline : internal;
        fnd_gfx_commands_submit(1, &frame->upload_list, &(fnd_gfx_submit_info){
            .domain_work_group  = transfer_work_group_index,
            .signal_count       = timeline ? 1 : 0,
            .signal_timelines   = &timeline,
            .signal_values      = last_upload ? &signal_value    : (uint64_t[]){++internal_itr}
        });

        // Wait for upload to end
        if (!last_upload) fnd_gfx_timeline_wait(internal, internal_itr);

        instances_to_write -= upload;
        buffer_uploaded    += upload;
    }

    // Draw only uploaded
    frame->to_draw = buffer_uploaded;

    // Free temporary
    fnd_gfx_free_timeline(internal);

    // Successful if succeeded to bind
    return bind_success;
}

void cmg_shp_gcmd_render(cmg_shp_context* context) {
    single_frame* frame = &context->frames->frames[context->index];
    if (frame->to_draw) {
        fnd_gfx_gcmd_bind_graphics_pipeline(context->frames->owning_shared->pipeline);
        gpu_constants constants = {.buffer_index = frame->bind};
        fnd_gfx_gcmd_write_constants(
            context->frames->owning_shared->pipeline, fnd_gfx_shader_stage_vertex, 0, (sizeof(gpu_constants)), &constants
        );
        fnd_gfx_gcmd_write_constants(
            context->frames->owning_shared->pipeline, fnd_gfx_shader_stage_pixel, 0, (sizeof(gpu_constants)), &constants
        );
        fnd_gfx_gcmd_draw(0, 3, 0, frame->to_draw);
    }
}

/*
    Methods
*/

static inline void emit_triangle(
    cmg_shp_context* context, 
    float x0,   float y0,
    float x1,   float y1,
    float x2,   float y2,
    float rcx,  float rcy, // radius center
    float radius
) {
    single_frame* frame = &context->frames->frames[context->index];

    // Ensure arena space
    if (frame->position == frame->capacity) {
        uint64_t      new_cap   = frame->capacity ? frame->capacity * 2 : 128;
        gpu_instance* new_arena = realloc(frame->arena, new_cap * sizeof(gpu_instance));
        if (!new_arena) return; // realloc failed
        frame->capacity = new_cap;
        frame->arena    = new_arena;
    }

    // Write instance arena
    frame->arena[frame->position++] = (gpu_instance){
        .x0 = x0, .y0 = y0,
        .x1 = x1, .y1 = y1,
        .x2 = x2, .y2 = y2,
        .r  = context->r, 
        .g  = context->g, 
        .b  = context->b, 
        .a  = context->a,
        .cx = rcx, 
        .cy = rcy,
        .radius = radius,
    };
};

void cmg_shp_set_color(cmg_shp_context* context, float r, float g, float b, float a) {
    context->r = r; context->g = g; context->b = b; context->a = a;
}

void cmg_shp_set_line_thickness(cmg_shp_context* context, float line_thickness) {
    context->line_thickness = line_thickness;
}

void cmg_shp_line(cmg_shp_context* context, fnd_lia_vec2 begin, fnd_lia_vec2 end) {
    single_frame* frame = &context->frames->frames[context->index];

    float dx = end.x - begin.x;
    float dy = end.y - begin.y;

    float len = sqrtf(dx * dx + dy * dy);
    if (len == 0.0f) return;

    float nx = -dy / len;
    float ny =  dx / len;

    float ox = nx * context->line_thickness * 0.5f;
    float oy = ny * context->line_thickness * 0.5f;

    emit_triangle(context,
        begin.x + ox, begin.y + oy,
        begin.x - ox, begin.y - oy,
        end.x   + ox, end.y   + oy,
        -1.0f, -1.0f, -1.0f // unrounded
    );

    emit_triangle(context,
        end.x   - ox, end.y   - oy,
        begin.x - ox, begin.y - oy,
        end.x   + ox, end.y   + oy,
        -1.0f, -1.0f, -1.0f // unrounded
    );
}

void cmg_shp_triangle(cmg_shp_context* context, fnd_lia_vec2 a, fnd_lia_vec2 b, fnd_lia_vec2 c) {
    emit_triangle(context, a.x, a.y, b.x, b.y, c.x, c.y, 0, 0, -1.0f); // unrounded
}

void cmg_shp_rect(cmg_shp_context* context, fnd_lia_vec2 first_corner, fnd_lia_vec2 second_corner) {
    emit_triangle(
        context,
        first_corner.x,  first_corner.y,
        second_corner.x, first_corner.y,
        second_corner.x, second_corner.y,
        0.0f, 0.0f, 0.0f // unrounded
    );

    emit_triangle(
        context,
        first_corner.x,  first_corner.y,
        second_corner.x, second_corner.y,
        first_corner.x,  second_corner.y,
        0.0f, 0.0f, 0.0f // unrounded
    );
}

void cmg_shp_circle(cmg_shp_context* context, fnd_lia_vec2 center, float radius) {
    emit_triangle(context, 
        center.x - radius, center.y - radius, 
        center.x - radius, center.y + radius, 
        center.x + radius, center.y - radius,
        center.x, center.y, radius
    );

    emit_triangle(context, 
        center.x + radius, center.y + radius, 
        center.x - radius, center.y + radius, 
        center.x + radius, center.y - radius,
        center.x, center.y, radius
    );
}

#endif // COMPAGES_SHAPES_RENDERING_IMPL
