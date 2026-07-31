#ifndef COMPAGES_ARBOR_RENDERING_H
#define COMPAGES_ARBOR_RENDERING_H

// ===========================
// Header Depedency

#include "arbor/arbor.h"
#include "demiurg/platform/graphics.h"
#include "compages/resources/font.h"
#include <stdint.h>

// ===========================
// Implementation Injections - User define those functions
// Functions shall return non-zero at successful find

int cmg_abr_injection_query_image(const char* image, dmg_gfx_texture** texture_out, dmg_gfx_uv_2d* uv_out);
int cmg_abr_injection_query_font(const char* font, cmg_fnt_font** font_out);

// ===========================
// Text Layout

arb_text_layout_func_signature cmg_abr_text_layout_func;

// ===========================
// Shared

typedef struct cmg_abr_shared_create_info {
    dmg_gfx_pipeline_attachment_state   attachment_state;
    dmg_gfx_shader_create_info          vertex_shader_info;
    dmg_gfx_shader_create_info          pixel_shader_info;
} cmg_abr_shared_create_info;

typedef struct cmg_abr_shared cmg_abr_shared;
cmg_abr_shared* cmg_abr_create_shared(dmg_gfx_hardware*, const cmg_abr_shared_create_info*);
void cmg_abr_free_shared(cmg_abr_shared*);

// ===========================
// Frames

typedef struct cmg_abr_frames_create_info {
    cmg_abr_shared* shared;
    uint32_t    count;
} cmg_abr_frames_create_info;

typedef struct cmg_abr_frames cmg_abr_frames;
cmg_abr_frames* cmg_abr_create_frames(dmg_gfx_hardware*, const cmg_abr_frames_create_info*);
void cmg_abr_free_frames(cmg_abr_frames*);

// ===========================
// Rendering Functions

// Returns non-zero at success
int cmg_abr_upload_cache(
    arb_upload_access       access,
    cmg_abr_shared*         shared,
    cmg_abr_frames*         frames,
    uint32_t                frame_idx,
    uint8_t                 transfer_work_group_index,
    uint8_t                 command_list_allocator_index,
    dmg_gfx_staging_memory* staging_memory,
    uint64_t                staging_memory_region_offset,
    uint64_t                staging_memory_region_size,
    dmg_gfx_timeline*       signal_timeline,
    uint64_t                signal_value
);

void cmg_abr_gcmd_render(
    cmg_abr_frames*         frames,
    uint32_t                frame
);

#endif // COMPAGES_ARBOR_RENDERING_H

#ifdef COMPAGES_ARBOR_RENDERING_IMPL

// ===========================
// Implementation Depedency

#include "compages/resources/font.h"
#include "demiurg/algorithm/partitioner.h"
#include "demiurg/algorithm/segmenter.h"
#include <stdlib.h>
#include <string.h>

// ===========================
// Glyph Typedef

typedef struct gpu_glyph {
    dmg_gfx_uv_2d   atlas_position;
    float       off_x,  off_y;
    float       size_x, size_y;
} gpu_glyph;

// ===========================
// Text Layout

void cmg_abr_text_layout_func(
    const arb_text_data*    text_data,          // Text data to layout
    int                     width_constrain,    // Given width, 0 == unlimited width
    size_t*                 out_count,          // Out count of glyphs
    void**                  out_glyphs,         // Glyphs malloc'ated array, shall be NULL if count == 0
    int*                    out_width,          // Out pixel width of text box
    int*                    out_height          // Out pixel height of text box
) {
    cmg_fnt_font* font; if (!cmg_abr_injection_query_font(text_data->font, &font)) return;
    const char* text = text_data->text;

    // If text empty or font invalid
    // Sent empty text request
    if (!text || !font) {
        *out_count  = 0; *out_glyphs = NULL;
        *out_width  = 0; *out_height = 0;
        return;
    }

    // Count glyphs to allocate
    size_t glyph_count = 0; size_t extra_lines_count = 0;
    for (size_t i = 0; text[i] != '\0';) {
        uint32_t cp; i += cmg_fnt_utf8_decode(text, i, &cp);
        if (cp != '\n') glyph_count++; 
        else extra_lines_count++;
    }

    // Allocate glyphs buffer, possibly empty
    // (text box may be filled with spaces, still we measure)
    gpu_glyph* glyphs = glyph_count ? malloc(sizeof(gpu_glyph) * glyph_count) : NULL;

    // Find font scale
    const float font_scale = text_data->size / cmg_fnt_get_base_size(font);

    // Populate glyphs buffer
    const float ascent      = cmg_fnt_get_base_ascent(font)   * font_scale;
    const float descent     = cmg_fnt_get_base_descent(font)  * font_scale;
    const float line_gap    = cmg_fnt_get_base_line_gap(font) * font_scale;
    const float line_height = ascent - descent + line_gap;

    float    pen_x      = 0.0f;
    float    pen_y      = 0.0f;
    float    text_width = 0.0f; // max line width across all lines
    size_t   glyph_idx  = 0;
    uint32_t prev_cp    = 0;    // for kerning; 0 = no previous glyph

    for (size_t itr = 0; text[itr] != '\0';) {
        uint32_t cp; itr += cmg_fnt_utf8_decode(text, itr, &cp);

        // Handle newline
        if (cp == '\n') {
            if (pen_x > text_width) text_width = pen_x;
            pen_x   = 0.0f;
            pen_y  -= line_height;
            prev_cp = 0; // reset kerning across lines
            continue;
        }

        // Kerning between consecutive glyphs on the same line
        if (prev_cp) pen_x += cmg_fnt_get_kerning(font, prev_cp, cp);

        // Write glyph
        const cmg_fnt_glyph g = cmg_fnt_get_glyph(font, cp);
        glyphs[glyph_idx++] = (gpu_glyph){
            .atlas_position = g.atlas_position,
            .off_x          = pen_x + g.bearing_x * font_scale,
            .off_y          = (extra_lines_count * line_height + pen_y) - g.bearing_y * font_scale,
            .size_x         = g.size_x * font_scale,
            .size_y         = g.size_y * font_scale,
        };

        // Advance
        pen_x  += g.advance_x * font_scale;
        prev_cp = cp;
    }

    // Account for final line (no trailing newline)
    if (pen_x > text_width) text_width = pen_x;

    // Total pixel height: baseline of last line + full single-line cap height
    float text_height = -pen_y + ascent;

    // Store text dimensions
    *out_width  = text_width;
    *out_height = text_height;

    // Result glyphs
    *out_count  = glyph_count;
    *out_glyphs = glyphs;
}

// ===========================
// Rendering Common

#define INITIAL_INSTANCES_BUFFER_SIZE   (1024 * sizeof(gpu_instance))
#define INITIAL_DRAW_ITEM_BUFFER_SIZE   (1024 * sizeof(gpu_draw_item))
#define INITIAL_CLIPBOXES_BUFFER_SIZE   (16 * sizeof(gpu_clipbox))
#define INITIAL_GLYPH_BUFFER_SIZE       (2024 * sizeof(gpu_glyph))
#define GLYPH_STRUCTURE_ALIGN           4

typedef struct gpu_instance {
    int item;
    int glyph;
} gpu_instance;

typedef struct gpu_draw_item {
    arb_mat3x2  transform;
    dmg_gfx_uv_2d   atlas_position;
    int         texture_index;
    int         clipbox_index;
    uint32_t    shader_index;
    int         rounding_pixel;
    float       r, g, b, a;
} gpu_draw_item;

typedef struct gpu_clipbox {
    arb_mat3x2  transform;
} gpu_clipbox;

typedef struct gpu_vertex_constants {
    uint32_t    resolution_width;
    uint32_t    resolution_height;
    uint32_t    instances_buffer_index;
    uint32_t    draw_items_buffer_index;
    uint32_t    glyphs_buffer_index;
} gpu_vertex_constants;

typedef struct gpu_pixel_constants {
    uint32_t    resolution_width;
    uint32_t    resolution_height;
    uint32_t    clips_buffer_index;
    uint32_t    sampler_index;
} gpu_pixel_constants;

// ===========================
// Helper Methods

static inline dmg_gfx_buffer* create_ssbo(dmg_gfx_hardware* hardware, uint64_t bytes) {
    return dmg_gfx_create_buffer(hardware, &(dmg_gfx_buffer_create_info){
        .bytes  = bytes,
        .access = dmg_gfx_memory_access_staging_write,
        .usage  = dmg_gfx_buffer_usage_storage
    });
}

static inline dmg_gfx_buffer* create_glyph_ssbo(dmg_gfx_hardware* hardware, uint64_t bytes) {
    return dmg_gfx_create_buffer(hardware, &(dmg_gfx_buffer_create_info){
        .bytes  = bytes,
        .access = dmg_gfx_memory_access_staging_read_and_write,
        .usage  = dmg_gfx_buffer_usage_storage
    });
}

// ===========================
// Shared Object

struct cmg_abr_shared {
    dmg_gfx_hardware*       owning_hardware;
    dmg_gfx_sampler*        sampler;
    dmg_gfx_pipeline*       pipeline;
    dmg_par_partitioner*    glyph_buffer_partitioner;
    dmg_gfx_buffer*         glyph_buffer;
};

cmg_abr_shared* cmg_abr_create_shared(dmg_gfx_hardware* hardware, const cmg_abr_shared_create_info* info) {
    cmg_abr_shared* shared = calloc(1, sizeof(cmg_abr_shared)); if (!shared) return NULL;
    shared->owning_hardware = hardware;

    // Sampler
    shared->sampler = dmg_gfx_create_sampler(hardware, &(dmg_gfx_sampler_create_info){
        .mag_filter                 = dmg_gfx_sampler_filter_linear,
        .min_filter                 = dmg_gfx_sampler_filter_linear,
        .mipmap_filter              = dmg_gfx_sampler_filter_linear,
        .x_coord_wrapping           = dmg_gfx_sampler_wrapping_repeat,
        .y_coord_wrapping           = dmg_gfx_sampler_wrapping_repeat,
        .z_coord_wrapping           = dmg_gfx_sampler_wrapping_repeat,
        .unnormalized_coordinates   = 0,
        .min_lod                    = 0,
        .max_lod                    = 1,
        .mip_lod_bias               = 0,
    }); if (!shared->sampler) goto _fail;

    // Glyphs buffer
    shared->glyph_buffer = create_glyph_ssbo(hardware, INITIAL_GLYPH_BUFFER_SIZE);
    if (!shared->glyph_buffer) goto _fail;

    // Glyph buffer partitioner
    shared->glyph_buffer_partitioner = dmg_par_create_partitioner(&(dmg_par_partitioner_create_info){
        .memory_bytes = INITIAL_GLYPH_BUFFER_SIZE
    }); if (!shared->glyph_buffer_partitioner) goto _fail;

    // Pipeline Shaders
    dmg_gfx_shader* vertex_shader = dmg_gfx_create_shader(shared->owning_hardware, &info->vertex_shader_info);
    dmg_gfx_shader* pixel_shader  = dmg_gfx_create_shader(shared->owning_hardware, &info->pixel_shader_info);

    if (!vertex_shader || !pixel_shader) {
        dmg_gfx_free_shader(vertex_shader);
        dmg_gfx_free_shader(pixel_shader);
        goto _fail;
    }

    // Pipeline
    shared->pipeline = dmg_gfx_create_pipeline(shared->owning_hardware, &(dmg_gfx_pipeline_create_info){
        .attachment_state = info->attachment_state,
        .shader_stages = {
            .shaders[dmg_gfx_shader_stage_vertex]   = vertex_shader,
            .constants[dmg_gfx_shader_stage_vertex] = sizeof(gpu_vertex_constants),
            .shaders[dmg_gfx_shader_stage_pixel]    = pixel_shader,
            .constants[dmg_gfx_shader_stage_pixel]  = sizeof(gpu_pixel_constants)
        },
        .input_assembler_state = {
            .topology = dmg_gfx_primitive_topology_triangle_strip
        },
        .rasterizer_state = {
            .scissor_enable     = 0,
            .depth_clamp_enable = 0,
            .fill_mode          = dmg_gfx_fill_mode_solid,
            .cull_mode          = dmg_gfx_cull_mode_none
        },
        .blend_state = {
            .blend_enable   = 1,
            .blend_op       = dmg_gfx_blend_op_add,
            .src_factor     = dmg_gfx_blend_factor_src_alpha,
            .dst_factor     = dmg_gfx_blend_factor_one_minus_src_alpha,
        },
        .depth_stencil_state = {
            .depth_test_enable      = 0,
            .depth_write_enable     = 0,
            .stencil_test_enable    = 0
        }
    });  dmg_gfx_free_shader(vertex_shader); dmg_gfx_free_shader(pixel_shader);
    if (!shared->pipeline) goto _fail;

    return shared;

_fail:
    cmg_abr_free_shared(shared);
    return NULL;
}

void cmg_abr_free_shared(cmg_abr_shared* shared) {
    if (!shared) return;
    dmg_gfx_free_sampler(shared->sampler);
    dmg_gfx_free_pipeline(shared->pipeline);
    dmg_gfx_free_buffer(shared->glyph_buffer);
    dmg_par_free_partitioner(shared->glyph_buffer_partitioner);
    free(shared);
}

// ===========================
// Frames

typedef struct single_frame {
    uint32_t                instances_to_render;
    dmg_gfx_buffer*             instances_buffer;
    dmg_gfx_buffer*             draw_items_buffer;
    dmg_gfx_buffer*             clipboxes_buffer;
    gpu_vertex_constants    vertex_constants;
    gpu_pixel_constants     pixel_constants;
    dmg_gfx_command_list*       upload_list;
} single_frame;

struct cmg_abr_frames {
    cmg_abr_shared*     owning_shared;
    uint32_t        count;
    single_frame*   frames;
};

cmg_abr_frames* cmg_abr_create_frames(dmg_gfx_hardware* hardware, const cmg_abr_frames_create_info* info) {
    cmg_abr_shared* shared = info->shared;

    cmg_abr_frames* frames = calloc(1, sizeof(cmg_abr_frames));  if (!frames) return NULL;
    frames->owning_shared = shared;
    
    // create frames
    frames->count  = info->count;
    frames->frames = calloc(info->count, sizeof(single_frame));
    if (!frames->frames) goto _fail;

    // populate frames
    for (uint32_t i = 0; i < info->count; i++) {
        single_frame* frame = &frames->frames[i];
        *frame = (single_frame){
            .instances_buffer   = create_ssbo(hardware, INITIAL_INSTANCES_BUFFER_SIZE),
            .draw_items_buffer  = create_ssbo(hardware, INITIAL_DRAW_ITEM_BUFFER_SIZE),
            .clipboxes_buffer   = create_ssbo(hardware, INITIAL_CLIPBOXES_BUFFER_SIZE)
        };

        if (!frame->instances_buffer || !frame->draw_items_buffer || !frame->clipboxes_buffer) goto _fail;
    }

    return frames;

_fail:
    cmg_abr_free_frames(frames);
    return NULL;
}

void cmg_abr_free_frames(cmg_abr_frames* frames) {
    if (!frames) return;
    for (uint32_t i = 0; i < frames->count; i++) {
        single_frame* frame = &frames->frames[i];
        dmg_gfx_free_buffer(frame->instances_buffer);
        dmg_gfx_free_buffer(frame->draw_items_buffer);
        dmg_gfx_free_buffer(frame->clipboxes_buffer);
        dmg_gfx_free_command_list(frame->upload_list);
    }
    free(frames->frames);
    free(frames);
}

// ===========================
// Rendering Functions

typedef struct ui_upload_params {
    uint64_t                count;
    dmg_seg_upload_request* requests;
    dmg_gfx_staging_memory* staging;
    uint64_t                offset;
} ui_upload_params;

static void ui_upload_record(void* raw_params) {
    ui_upload_params* params = raw_params;
    uint64_t offset = 0;
    for (uint64_t i = 0; i < params->count; i++) {
        dmg_seg_upload_request req = params->requests[i];
        dmg_gfx_tcmd_copy_staging_memory_to_buffer(
            params->staging, (dmg_gfx_buffer*)req.target,
            params->offset + offset, req.offset, req.bytes
        );
        offset += req.bytes;
    }
}

typedef struct glyphs_rewrite_params {
    dmg_gfx_buffer* old_buffer;
    dmg_gfx_buffer* new_buffer;
} glyphs_rewrite_params;

static void glyphs_rewrite_record(void* raw_params) {
    glyphs_rewrite_params* params = raw_params;
    dmg_gfx_tcmd_copy_buffer_to_buffer(
        params->old_buffer, params->new_buffer, 0, 0, 
        dmg_gfx_buffer_query_bytes(params->old_buffer)
    );
}

int cmg_abr_upload_cache(
    arb_upload_access   access,
    cmg_abr_shared*         shared,
    cmg_abr_frames*         frames,
    uint32_t            frame_idx,
    uint8_t             transfer_work_group_index,
    uint8_t             command_list_allocator_index,
    dmg_gfx_staging_memory* staging_memory,
    uint64_t            staging_memory_region_offset,
    uint64_t            staging_memory_region_size,
    dmg_gfx_timeline*       signal_timeline,
    uint64_t            signal_value
) {
    dmg_gfx_hardware* hardware = shared->owning_hardware;
    single_frame* frame    = &frames->frames[frame_idx];

    // Function-wide success flag
    int success = 1;

    // Create segmenter
    dmg_seg_segmenter* segmenter = dmg_seg_create_segmenter(&(dmg_seg_segmenter_create_info){
        .bandwidth = staging_memory_region_size
    }); if (!segmenter) goto _cleanup;

    // Free garbage text
    for (size_t i = 0; i < access.text_free_count; i++) {
        dmg_par_partition* part = access.text_free_requests[i].text_pointer;
        dmg_par_partitioner_free_partition(shared->glyph_buffer_partitioner, part);
    }

    // Allocate new text
    for (size_t i = 0; i < access.text_alloc_count; i++) {
    _try_partition:
        arb_text_alloc_request req = access.text_alloc_requests[i];

        // New text is empty - creation of 0 bytes partition is forbidden
        if (!req.glyphs_count) continue;

        // Request new partition
        dmg_par_partition* text_partition = dmg_par_partitioner_alloc_partition(
            shared->glyph_buffer_partitioner,
            req.glyphs_count * sizeof(gpu_glyph), 
            GLYPH_STRUCTURE_ALIGN
        );

        // Failed to create partition - create bigger text buffer
        if (!text_partition) {
            dmg_gfx_hardware_wait_idle(hardware);

            // Alloc new buffer with double size
            uint64_t old_bytes = dmg_gfx_buffer_query_bytes(shared->glyph_buffer);
            dmg_gfx_buffer* new_buffer = create_glyph_ssbo(hardware, old_bytes * 2);

            // Failed to alloc new buffer
            if (!new_buffer) continue;

            // Rewrite contents
            dmg_gfx_command_list* rewrite_list = dmg_gfx_create_command_list(hardware, &(dmg_gfx_command_list_create_info){
                .domain = dmg_gfx_command_domain_transfer,
                .aindex = transfer_work_group_index,
                .record = glyphs_rewrite_record,
                .params = &(glyphs_rewrite_params){
                    .old_buffer = shared->glyph_buffer,
                    .new_buffer = new_buffer
                }
            });

            // Submit
            dmg_gfx_command_list_submit(1, &rewrite_list, &(dmg_gfx_submit_info){.domain_work_group = 0});
            dmg_gfx_hardware_wait_idle(hardware); dmg_gfx_free_command_list(rewrite_list);

            // Since rewrited, pick new buffer
            dmg_gfx_free_buffer(shared->glyph_buffer);
            shared->glyph_buffer = new_buffer;

            // Resize partitioner
            shared->glyph_buffer_partitioner = dmg_par_create_partitioner(&(dmg_par_partitioner_create_info){
                .memory_bytes    = old_bytes * 2,
                .old_partitioner = shared->glyph_buffer_partitioner
            });

            // Try again
            goto _try_partition;
        }

        // Assign partition to text node
        *req.text_pointer_out = text_partition;
    }

    // Generate draw regions for texts
    for (size_t i = 0; i < access.text_alloc_count; i++) {
        arb_text_alloc_request  req  = access.text_alloc_requests[i];
        dmg_par_partition*  prt  = *req.text_pointer_out;
        if (!prt) continue; // Text empty, nothing to upload

        dmg_seg_segmenter_upload(segmenter, (dmg_seg_upload_request){
            .target = (uint64_t)shared->glyph_buffer,
            .offset = dmg_par_partition_query_offset(prt),
            .source = req.glyphs,
            .bytes  = req.glyphs_count * sizeof(gpu_glyph)
        });
    }

    // Prepare draw items, draw instances, draw clipboxes for upload

    uint32_t        items_count = 0; 
    uint64_t        items_bytes = 0;
    gpu_draw_item*  items = NULL;

    uint32_t        instances_count = 0;
    uint64_t        instances_bytes = 0;
    gpu_instance*   instances = NULL;

    uint32_t        clipboxes_count = 0; 
    uint64_t        clipboxes_bytes = 0;
    gpu_clipbox*    clipboxes = NULL;
    
    // Generate GPU Items, findout instances count
    items_count = access.draws_count;
    items_bytes = access.draws_count * sizeof(gpu_draw_item);
    items = malloc(items_bytes); if (!items) goto _cleanup;
    for (uint32_t i = 0; i < items_count; i++) {
        arb_draw_request req = access.draws_requests[i];

        if (req.is_box_not_text) {
            int texture_index = 0; dmg_gfx_texture* texture; dmg_gfx_uv_2d uv;
            if (req.box.data.image && cmg_abr_injection_query_image(req.box.data.image, &texture, &uv)) {
                texture_index = dmg_gfx_shader_resource_bind(
                    hardware, dmg_gfx_resource_type_sampled_texture, texture, &success
                );
                texture_index++; // offset so idx 0 is no texture in shader
            }

            items[i] = (gpu_draw_item){
                .transform      = req.transform,
                .atlas_position = uv,
                .texture_index  = texture_index,
                .clipbox_index  = req.clip_index,
                .shader_index   = req.box.data.shader,
                .rounding_pixel = req.box.data.rounding,
                .r              = (float)req.box.data.tint.r / 255.0f,
                .g              = (float)req.box.data.tint.g / 255.0f,
                .b              = (float)req.box.data.tint.b / 255.0f,
                .a              = (float)req.box.data.tint.a / 255.0f
            };

            instances_count += 1;  // single box
        }
        else {
            dmg_par_partition* part     = *req.text.pointer;
            arb_text_data text_data =  req.text.data;
            if (!part) continue;

            cmg_fnt_font* font_tex; if (!cmg_abr_injection_query_font(text_data.font, &font_tex)) continue;
            uint32_t texture_index = dmg_gfx_shader_resource_bind(
                hardware, dmg_gfx_resource_type_sampled_texture, cmg_fnt_get_texture(font_tex), &success
            );

            int signed_texture_index = -(int)texture_index; // is font
            signed_texture_index--; // offset so idx 0 is no texture in shader

            items[i] = (gpu_draw_item){
                .transform      = req.transform,
                .atlas_position = (dmg_gfx_uv_2d){0, 0, 1, 1},
                .texture_index  = signed_texture_index,
                .clipbox_index  = req.clip_index,
                .shader_index   = text_data.shader,
                .r              = (float)text_data.tint.r / 255.0f,
                .g              = (float)text_data.tint.g / 255.0f,
                .b              = (float)text_data.tint.b / 255.0f,
                .a              = (float)text_data.tint.a / 255.0f,
            };

            instances_count += dmg_par_partition_query_size(part) / sizeof(gpu_glyph);
        }
    }

    // Generate GPU Instances
    instances_bytes = instances_count * sizeof(gpu_instance);
    instances = malloc(instances_bytes); if (!instances) goto _cleanup;
    uint32_t instance_idx = 0;
    for (int i = 0; i < access.draws_count; i++) {
        arb_draw_request req = access.draws_requests[i];
        if (req.is_box_not_text) {
            instances[instance_idx++] = (gpu_instance){
                .item   = i,
                .glyph  = -1
            };
        }
        else {
            dmg_par_partition* part = *req.text.pointer;
            if (!part) continue;
            
            size_t first  = dmg_par_partition_query_offset(part) / sizeof(gpu_glyph);
            size_t glyphs = dmg_par_partition_query_size(part) / sizeof(gpu_glyph);
            for (size_t g = 0; g < glyphs; g++) {
                instances[instance_idx++] = (gpu_instance){
                    .item   = i,
                    .glyph  = first + g
                };
            }
        }
    }

    // Generate GPU Clipboxes
    clipboxes_count = access.clipboxes_count;
    clipboxes_bytes = access.clipboxes_count * sizeof(gpu_clipbox);
    clipboxes       = malloc(clipboxes_bytes); if (!clipboxes) goto _cleanup;
    for (uint32_t i = 0; i < clipboxes_count; i++) {
        arb_clipbox_request req = access.clipboxes_requests[i];
        clipboxes[i] = (gpu_clipbox){
            .transform = req.transform
        };
    }

    // Items buffer
    if (dmg_gfx_buffer_query_bytes(frame->draw_items_buffer) < items_bytes) {
        dmg_gfx_buffer* new_buffer = create_ssbo(hardware, items_bytes);
        if (!new_buffer) {success = 0; goto _cleanup;}
        dmg_gfx_free_buffer(frame->draw_items_buffer);
        frame->draw_items_buffer = new_buffer;
    }

    // Instanced buffer
    if (dmg_gfx_buffer_query_bytes(frame->instances_buffer) < instances_bytes) {
        dmg_gfx_buffer* new_buffer = create_ssbo(hardware, instances_bytes);
        if (!new_buffer) {success = 0; goto _cleanup;}
        dmg_gfx_free_buffer(frame->instances_buffer);
        frame->instances_buffer = new_buffer;
    }

    // Clipboxes buffer
    if (dmg_gfx_buffer_query_bytes(frame->clipboxes_buffer) < clipboxes_bytes) {
        dmg_gfx_buffer* new_buffer = create_ssbo(hardware, clipboxes_bytes);
        if (!new_buffer) {success = 0; goto _cleanup;}
        dmg_gfx_free_buffer(frame->clipboxes_buffer);
        frame->clipboxes_buffer = new_buffer;
    }

    // Uploads requests
    dmg_seg_segmenter_upload(segmenter, (dmg_seg_upload_request){
        .target = (uint64_t)frame->draw_items_buffer,
        .offset = 0,
        .source = items,
        .bytes  = items_count * sizeof(gpu_draw_item)
    });

    dmg_seg_segmenter_upload(segmenter, (dmg_seg_upload_request){
        .target = (uint64_t)frame->clipboxes_buffer,
        .offset = 0,
        .source = clipboxes,
        .bytes  = clipboxes_count * sizeof(gpu_clipbox)
    });

    dmg_seg_segmenter_upload(segmenter, (dmg_seg_upload_request){
        .target = (uint64_t)frame->instances_buffer,
        .offset = 0,
        .source = instances,
        .bytes  = instances_count * sizeof(gpu_instance)
    });

    // Set render parameters since buffer are ready
    frame->vertex_constants = (gpu_vertex_constants){
        .resolution_width        = access.resolution_x,
        .resolution_height       = access.resolution_y,
        .instances_buffer_index  = dmg_gfx_shader_resource_bind(hardware, dmg_gfx_resource_type_storage_buffer, frame->instances_buffer, &success),
        .draw_items_buffer_index = dmg_gfx_shader_resource_bind(hardware, dmg_gfx_resource_type_storage_buffer, frame->draw_items_buffer, &success),
        .glyphs_buffer_index     = dmg_gfx_shader_resource_bind(hardware, dmg_gfx_resource_type_storage_buffer, shared->glyph_buffer, &success),
    };
    frame->pixel_constants = (gpu_pixel_constants){
        .resolution_width   = access.resolution_x,
        .resolution_height  = access.resolution_y,
        .clips_buffer_index = dmg_gfx_shader_resource_bind(hardware, dmg_gfx_resource_type_storage_buffer, frame->clipboxes_buffer, &success),
        .sampler_index      = dmg_gfx_shader_resource_bind(hardware, dmg_gfx_resource_type_sampler, shared->sampler, &success),
    };

    // Perform uploads
    dmg_gfx_timeline* internal = NULL;
    uint64_t internal_itr = 0;
    
    while (!dmg_seg_segmenter_query_empty(segmenter)) {
        if (internal) dmg_gfx_timeline_wait(internal, internal_itr);
        
        uint64_t count; dmg_seg_upload_request* requests;
        dmg_seg_segmenter_continue(segmenter, &count, &requests);

        int last_upload = dmg_seg_segmenter_query_empty(segmenter);
        if (!last_upload && !internal) {
            internal = dmg_gfx_create_timeline(hardware, &(dmg_gfx_timeline_create_info){
                .initial_value = 0
            });
        }

        // copy to staging memory
        char* mapped = dmg_gfx_staging_memory_map(staging_memory, staging_memory_region_offset, staging_memory_region_size);
        uint64_t offset = 0;
        for (uint64_t i = 0; i < count; i++) {
            dmg_seg_upload_request req = requests[i];
            memcpy(mapped + offset, req.source, req.bytes);
            offset += req.bytes;
        }
        dmg_gfx_staging_memory_unmap(staging_memory);

        // record rewrite list
        frame->upload_list = dmg_gfx_create_command_list(hardware, &(dmg_gfx_command_list_create_info){
            .domain = dmg_gfx_command_domain_transfer,
            .aindex = command_list_allocator_index,
            .parent = frame->upload_list,
            .record = ui_upload_record,
            .params = &(ui_upload_params){
                .count    = count,
                .requests = requests,
                .staging  = staging_memory,
                .offset   = staging_memory_region_offset
            }
        });

        // Submit gpu work
        dmg_gfx_timeline* timeline = last_upload ? signal_timeline : internal;
        dmg_gfx_command_list_submit(1, &frame->upload_list, &(dmg_gfx_submit_info){
            .domain_work_group  = transfer_work_group_index,
            .signal_count       = timeline ? 1 : 0,
            .signal_timelines   = &timeline,
            .signal_values      = last_upload ? &signal_value : (uint64_t[]){++internal_itr}
        });
    }

    if (internal) dmg_gfx_free_timeline(internal);

    // Mark to render
    frame->instances_to_render = instances_count;

_cleanup: 
    dmg_seg_free_segmenter(segmenter);                  // Free segmenter
    free(items); free(clipboxes); free(instances);  // Free allocated memory
    return success;
}

void cmg_abr_gcmd_render(
    cmg_abr_frames* frames,
    uint32_t    frame_idx
) {
    single_frame* frame = &frames->frames[frame_idx % frames->count];
    if (frame->instances_to_render) {
        dmg_gfx_gcmd_bind_graphics_pipeline(frames->owning_shared->pipeline);

        dmg_gfx_gcmd_write_constants(
            frames->owning_shared->pipeline, dmg_gfx_shader_stage_vertex, 0, sizeof(gpu_vertex_constants), &frame->vertex_constants
        );

        dmg_gfx_gcmd_write_constants(
            frames->owning_shared->pipeline, dmg_gfx_shader_stage_pixel, 0, sizeof(gpu_pixel_constants), &frame->pixel_constants
        );

        dmg_gfx_gcmd_draw(0, 4, 0, frame->instances_to_render);
    }
}

#endif // COMPAGES_ARBOR_RENDERING_IMPL
