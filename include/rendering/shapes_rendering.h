/*
----------------------------------------------------------------
Contents

This file provides simple system to render basic shapes: lines, triangles, rectangles, circles.

----------------------------------------------------------------
Code info:
- dshp prefix
- DEMIURG_SHAPES_IMPL macro to build
- graphics.h dependant
- linear_algebra.h dependant

----------------------------------------------------------------
Usage

- Create dshp_shared object - it contains shared read-only objects for rendering.
    Create one per hardware. In create info, link shaders (provided in shader directory).
- Create dshp_frames - it contains per frame geometry buffers you record with drawing methods.
- Use commands to draw shapes
- Upload data to gpu with dshp_upload
- Render with graphics command dshp_gcmd_render
*/

#ifndef DEMIURG_SHAPES_H
#define DEMIURG_SHAPES_H

#include "demiurg/platform/graphics.h"
#include "demiurg/mathematics/linear_algebra.h"

// Shapes Rendering Shared Object

typedef struct dshp_shared_create_info {
    dgx_pipeline_attachment_state   attachment_state;
    dgx_shader_create_info          vertex_shader_info;
    dgx_shader_create_info          pixel_shader_info;
} dshp_shared_create_info;

typedef struct dshp_shared dshp_shared;
dshp_shared* dshp_create_shared(dgx_hardware*, const dshp_shared_create_info* info);
void dshp_free_shared(dshp_shared*);

// Shapes Rendering Frame Contextes

typedef struct dshp_frames_create_info {
    dshp_shared*    shared;
    uint32_t        count;
} dshp_frames_create_info;

typedef struct dshp_frames dshp_frames;
dshp_frames* dshp_create_frames(dgx_hardware*, const dshp_frames_create_info* info);
void dshp_free_frames(dshp_frames*);

// shorthand not to pass frame in flight to every function
typedef struct dshp_context {
    dshp_frames*    frames;
    uint32_t        index;
    float           r, g, b, a;
    float           line_thickness;
} dshp_context;

// Actuall Draw Operation

// resets draw requests in context
void dshp_reset(
    dshp_context* context
);

// Uploads draw requests to gpu
// Returns non-zero at success
int dshp_upload(
    dshp_context*       context,
    uint8_t             transfer_work_group_index,
    uint8_t             command_list_allocator_index,
    dgx_staging_memory* staging_memory,
    uint64_t            staging_memory_region_offset,
    uint64_t            staging_memory_region_size,
    dgx_timeline*       signal_timeline,
    uint64_t            signal_value
);

// command to draw from context
// needs to be re recorded every frame as amount of
// drawn primitives may change
// call with render target bound, viewport and scissors set
void dshp_gcmd_render(dshp_context* context);

// Shapes Draw Function

// set drawn shapes color
void dshp_set_color(
    dshp_context* context,
    float r, float g, float b, float a
);

void dshp_set_line_thickness(
    dshp_context* context,
    float line_thickness
);

void dshp_line(
    dshp_context* context,
    dla_vec2 begin, dla_vec2 end
);

void dshp_triangle(
    dshp_context* context,
    dla_vec2 a, dla_vec2 b, dla_vec2 c
);

void dshp_rect(
    dshp_context* context,
    dla_vec2 first_corner, dla_vec2 second_corner
);

void dshp_circle(
    dshp_context* context,
    dla_vec2 center, float radius
);

#endif // DEMIURG_SHAPES_H

#ifdef DEMIURG_SHAPES_IMPL

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

struct dshp_shared {
    dgx_hardware*   owning_hardware;
    dgx_pipeline*   pipeline;
};

dshp_shared* dshp_create_shared(dgx_hardware* hardware, const dshp_shared_create_info* info) {
    dshp_shared* shared = calloc(1, sizeof(dshp_shared)); if (!shared) return NULL;
    shared->owning_hardware = hardware;

    // Pipeline Shaders
    dgx_shader* vertex_shader = dgx_create_shader(shared->owning_hardware, &info->vertex_shader_info);
    dgx_shader* pixel_shader  = dgx_create_shader(shared->owning_hardware, &info->pixel_shader_info);

    if (!vertex_shader || !pixel_shader) {
        dgx_free_shader(vertex_shader);
        dgx_free_shader(pixel_shader);
        goto _fail;
    }

    shared->pipeline = dgx_create_pipeline(shared->owning_hardware, &(dgx_pipeline_create_info){
        .attachment_state = info->attachment_state,
        .shader_stages  = {
            .shaders[dgx_shader_stage_vertex]   = vertex_shader,
            .constants[dgx_shader_stage_vertex] = sizeof(gpu_constants),
            .shaders[dgx_shader_stage_pixel]    = pixel_shader,
            .constants[dgx_shader_stage_pixel]  = sizeof(gpu_constants)
        },
        .input_assembler_state = {
            .topology = dgx_primitive_topology_triangle_list
        },
        .rasterizer_state = {
            .scissor_enable     = 0,
            .depth_clamp_enable = 0,
            .fill_mode          = dgx_fill_mode_solid,
            .cull_mode          = dgx_cull_mode_none
        },
        .blend_state = {
            .blend_enable   = 1,
            .blend_op       = dgx_blend_op_add,
            .src_factor     = dgx_blend_factor_src_alpha,
            .dst_factor     = dgx_blend_factor_one_minus_src_alpha,
        },
        .depth_stencil_state = {
            .depth_test_enable      = 0,
            .depth_write_enable     = 0,
            .stencil_test_enable    = 0
        }
    });

    dgx_free_shader(vertex_shader);
    dgx_free_shader(pixel_shader);

    if (!shared->pipeline) goto _fail;
    return shared;

_fail:
    dshp_free_shared(shared); 
    return NULL;
}

void dshp_free_shared(dshp_shared* shared) {
    if (!shared) return;
    dgx_free_pipeline(shared->pipeline);
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
    dgx_buffer*         buffer;         // GPU instances buffer
    uint32_t            bind;           // Buffer bind point
    dgx_command_list*   upload_list;    // List used to upload instances
} single_frame;

struct dshp_frames {
    dgx_hardware*   owning_hardware;
    dshp_shared*    owning_shared;
    uint32_t        in_flight;
    single_frame*   frames;
};

dshp_frames* dshp_create_frames(dgx_hardware* hardware, const dshp_frames_create_info* info) {
    if (!hardware || !info->shared) goto _fail;

    dshp_frames* frames = calloc(1, sizeof(dshp_frames)); 
    if (!frames) goto _fail;

    *frames = (dshp_frames) {
        .owning_hardware = hardware,
        .owning_shared   = info->shared,
        .in_flight       = info->count
    };
    
    // Frames
    frames->frames = calloc(info->count, sizeof(single_frame));
    if (!frames->frames) goto _fail;

    return frames;

_fail:
    dshp_free_frames(frames);
    return NULL;
}

void dshp_free_frames(dshp_frames* frames) {
    if (!frames) return;
    for (uint32_t i = 0; i < frames->in_flight; i++) {
        dgx_free_buffer(frames->frames[i].buffer);
        dgx_free_command_list(frames->frames[i].upload_list);
        free(frames->frames[i].arena);
    }
    free(frames->frames);
    free(frames);
}

void dshp_reset(dshp_context* context) {
    single_frame* frame = &context->frames->frames[context->index];
    frame->position = 0;
}

typedef struct upload_params {
    dgx_staging_memory* staging;
    dgx_buffer*         buffer;
    uint64_t            uploaded;
    uint64_t            upload;
} upload_params;

static void upload_record(void* raw_params) {
    upload_params* params = raw_params;
    dgx_tcmd_copy_staging_memory_to_buffer(
        params->staging, params->buffer,
        0, params->uploaded * sizeof(gpu_instance), params->upload * sizeof(gpu_instance)
    );
}

static uint64_t min_u64(uint64_t l, uint64_t r) {
    return l < r ? l : r;
}

int dshp_upload(
    dshp_context*       context,
    uint8_t             transfer_work_group_index,
    uint8_t             command_list_allocator_index,
    dgx_staging_memory* staging_memory,
    uint64_t            staging_memory_region_offset,
    uint64_t            staging_memory_region_size,
    dgx_timeline*       signal_timeline,
    uint64_t            signal_value
) {
    single_frame* frame    = &context->frames->frames[context->index];
    dgx_hardware* hardware = context->frames->owning_hardware;

    // Nothing to upload
    if (frame->position == 0) {
        dgx_timeline_signal(signal_timeline, signal_value); return 1;
    }

    // Whether succeeded to bind resources
    int bind_success = 1;

    // Ensure buffer space
    if (!frame->buffer || dgx_buffer_query_bytes(frame->buffer) < frame->position * sizeof(gpu_instance)) {
        dgx_buffer* new_buffer = NULL;
        uint64_t    new_cap[2] = {frame->capacity, frame->position};
        for (int i = 0; i < 2; i++) {
            new_buffer = dgx_create_buffer(hardware, &(dgx_buffer_create_info){
                .bytes  = new_cap[i] * sizeof(gpu_instance),
                .usage  = dgx_buffer_usage_storage,
                .access = dgx_memory_access_staging_write
            });
            if (new_buffer) break;
        }
        if (new_buffer) {
            dgx_free_buffer(frame->buffer);
            frame->buffer = new_buffer;
            frame->bind = dgx_shader_resource_bind(hardware, dgx_resource_type_storage_buffer, new_buffer, &bind_success);
        }
    }

    // Safe return
    if (!frame->buffer) {
        dgx_timeline_signal(signal_timeline, signal_value); return 0;
    }

    // Cap written instances to buffer capacity
    uint64_t instances_to_write = min_u64(dgx_buffer_query_bytes(frame->buffer) / sizeof(gpu_instance), frame->position);
    uint64_t staging_capacity   = staging_memory_region_size / sizeof(gpu_instance);
    uint64_t buffer_uploaded    = 0;

    // If wont do in single upload, alloc internal timeline
    dgx_timeline* internal = instances_to_write > staging_capacity ? dgx_create_timeline(hardware, &(dgx_timeline_create_info){
        .initial_value = 0
    }) : NULL; if (!internal) instances_to_write = min_u64(instances_to_write, staging_capacity);
    uint64_t internal_itr = 0;

    // Upload loop
    while (instances_to_write) {
        uint64_t upload = min_u64(instances_to_write, staging_capacity);

        char* mem = dgx_staging_memory_map(staging_memory, staging_memory_region_offset, staging_memory_region_size);
        memcpy(mem, &frame->arena[buffer_uploaded], upload * sizeof(gpu_instance));
        dgx_staging_memory_unmap(staging_memory);

        frame->upload_list = dgx_create_command_list(hardware, &(dgx_command_list_create_info){
            .domain = dgx_command_domain_transfer,
            .aindex = command_list_allocator_index,
            .parent = frame->upload_list,
            .record = upload_record,
            .params = &(upload_params){
                .staging  = staging_memory,
                .buffer   = frame->buffer,
                .uploaded = buffer_uploaded,
                .upload   = upload
            }
        });

        int last_upload = instances_to_write <= staging_capacity;

        dgx_timeline* timeline = last_upload ? signal_timeline : internal;
        dgx_command_list_submit(1, &frame->upload_list, &(dgx_submit_info){
            .domain_work_group  = transfer_work_group_index,
            .signal_count       = timeline ? 1 : 0,
            .signal_timelines   = &timeline,
            .signal_values      = last_upload ? &signal_value    : (uint64_t[]){++internal_itr}
        });

        // Wait for upload to end
        if (!last_upload) dgx_timeline_wait(internal, internal_itr);

        instances_to_write -= upload;
        buffer_uploaded    += upload;
    }

    // Draw only uploaded
    frame->to_draw = buffer_uploaded;

    // Free temporary
    dgx_free_timeline(internal);

    // Successful if succeeded to bind
    return bind_success;
}

void dshp_gcmd_render(dshp_context* context) {
    single_frame* frame = &context->frames->frames[context->index];
    if (frame->to_draw) {
        dgx_gcmd_bind_graphics_pipeline(context->frames->owning_shared->pipeline);
        gpu_constants constants = {.buffer_index = frame->bind};
        dgx_gcmd_write_constants(
            context->frames->owning_shared->pipeline, dgx_shader_stage_vertex, 0, (sizeof(gpu_constants)), &constants
        );
        dgx_gcmd_write_constants(
            context->frames->owning_shared->pipeline, dgx_shader_stage_pixel, 0, (sizeof(gpu_constants)), &constants
        );
        dgx_gcmd_draw(0, 3, 0, frame->to_draw);
    }
}

/*
    Methods
*/

static inline void emit_triangle(
    dshp_context* context, 
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

void dshp_set_color(dshp_context* context, float r, float g, float b, float a) {
    context->r = r; context->g = g; context->b = b; context->a = a;
}

void dshp_set_line_thickness(dshp_context* context, float line_thickness) {
    context->line_thickness = line_thickness;
}

void dshp_line(dshp_context* context, dla_vec2 begin, dla_vec2 end) {
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

void dshp_triangle(dshp_context* context, dla_vec2 a, dla_vec2 b, dla_vec2 c) {
    emit_triangle(context, a.x, a.y, b.x, b.y, c.x, c.y, 0, 0, -1.0f); // unrounded
}

void dshp_rect(dshp_context* context, dla_vec2 first_corner, dla_vec2 second_corner) {
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

void dshp_circle(dshp_context* context, dla_vec2 center, float radius) {
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

#endif // DEMIURG_SHAPES_IMPL
