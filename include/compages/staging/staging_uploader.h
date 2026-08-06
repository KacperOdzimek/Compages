#ifndef COMPAGES_STAGING_UPLOADER_H
#define COMPAGES_STAGING_UPLOADER_H

#include "fundatio/platform/graphics.h"
#include <stdint.h>

typedef void(*cmg_stu_memory_free_func)(void* memory);

typedef struct cmg_stu_uploader_create_info {
    uint64_t staging_size_bytes;
    uint8_t  transfer_group_index;
    uint8_t  commands_allocator_index;
} cmg_stu_uploader_create_info;

typedef struct cmg_stu_uploader cmg_stu_uploader;

cmg_stu_uploader* cmg_stu_create_uploader(fnd_gfx_hardware*, const cmg_stu_uploader_create_info*);
void cmg_stu_free_uploader(cmg_stu_uploader*);

// Schedules buffer upload
// Returns non-zero at success
// cmg_stu_uploader_timepoint is higher or equal to out_timepoint
// buffer is uploaded
int cmg_stu_uploader_buffer_upload(
    cmg_stu_uploader*, uint64_t bytes, void* memory, cmg_stu_memory_free_func free_func,
    fnd_gfx_buffer* dest_buffer, uint64_t dest_offset, uint64_t* out_timepoint
);

// Schedules texture upload
// Returns non-zero at success
// cmg_stu_uploader_timepoint is higher or equal to out_timepoint
// texture is uploaded
int cmg_stu_uploader_texture_upload(
    cmg_stu_uploader*, uint64_t bytes, void* memory, cmg_stu_memory_free_func free_func,
    fnd_gfx_texture* dest_texture, fnd_gfx_texture_dimensions dest_offset, fnd_gfx_texture_dimensions dest_dim, uint64_t* out_timepoint
);

// Returns current timepoint
uint64_t cmg_stu_uploader_timepoint(cmg_stu_uploader*);

// Enters uploader routine
// If lock will wait for staging to be unused, and then begin copying
// If not lock and staging is in use, returns instantly
void cmg_stu_uploader_enter(cmg_stu_uploader*, int lock);

#endif // COMPAGES_UPLOADER_H

#ifdef COMPAGES_STAGING_UPLOADER_IMPL

#include <stdlib.h>
#include <string.h>

typedef struct upload_request {
    uint64_t                            bytes;
    void*                               memory;
    cmg_stu_memory_free_func            free_func;
    int                                 is_buffer;
    uint64_t                            current_offset;
    uint64_t                            timepoint;
    union {
        struct {
            fnd_gfx_buffer*             dest_buffer;
            uint64_t                    dest_offset;
        } buffer;
        struct {
            fnd_gfx_texture*            dest_texture;
            fnd_gfx_texture_dimensions  dest_offset;
            fnd_gfx_texture_dimensions  dest_dim;
        } texture;
    };
} upload_request;

struct cmg_stu_uploader {
    fnd_gfx_hardware*   hardware;
    fnd_gfx_staging*    staging;
    fnd_gfx_commands*   commands;
    uint64_t            bandwidth;
    fnd_gfx_timeline*   staging_tl;
    uint64_t            staging_it;
    fnd_gfx_timeline*   uploads_tl;
    uint64_t            uploads_it;
    uint8_t             allocator;
    uint8_t             transfer;

    uint64_t            capacity;
    uint64_t            count;
    uint64_t            position;
    upload_request*     requests;
};

static inline uint64_t min_u64(uint64_t a, uint64_t b) {
    return a < b ? a : b;
}

cmg_stu_uploader* cmg_stu_create_uploader(fnd_gfx_hardware* hardware, const cmg_stu_uploader_create_info* info) {
    if (info->staging_size_bytes == 0) return NULL;

    cmg_stu_uploader* uploader = malloc(sizeof(cmg_stu_uploader));
    if (!uploader) goto _fail;

    *uploader = (cmg_stu_uploader){
        .hardware  = hardware,
        .transfer  = info->transfer_group_index,
        .allocator = info->commands_allocator_index,
        .bandwidth = info->staging_size_bytes,
        .capacity  = 16
    };

    uploader->requests = malloc(sizeof(upload_request) * uploader->capacity);

    uploader->staging = fnd_gfx_create_staging(hardware, &(fnd_gfx_staging_create_info){
        .bytes = uploader->bandwidth
    });

    uploader->staging_tl = fnd_gfx_create_timeline(hardware, &(fnd_gfx_timeline_create_info){ .initial_value = 0 });
    uploader->uploads_tl = fnd_gfx_create_timeline(hardware, &(fnd_gfx_timeline_create_info){ .initial_value = 0 });

    if (!uploader->requests || !uploader->staging || !uploader->staging_tl || !uploader->uploads_tl) {
        goto _fail;
    }

    return uploader;

_fail:
    cmg_stu_free_uploader(uploader);
    return NULL;
}

void cmg_stu_free_uploader(cmg_stu_uploader* uploader) {
    if (!uploader) return;

    if (uploader->hardware)   fnd_gfx_hardware_wait_idle(uploader->hardware);
    if (uploader->commands)   fnd_gfx_free_commands(uploader->commands);
    if (uploader->staging_tl) fnd_gfx_free_timeline(uploader->staging_tl);
    if (uploader->uploads_tl) fnd_gfx_free_timeline(uploader->uploads_tl);
    if (uploader->staging)    fnd_gfx_free_staging(uploader->staging);

    if (uploader->requests) {
        for (uint64_t i = 0; i < uploader->count; ++i) {
            upload_request* req = &uploader->requests[(uploader->position + i) % uploader->capacity];
            if (req->free_func && req->memory) req->free_func(req->memory);
        }
        free(uploader->requests);
    }

    free(uploader);
}

static int enqueue_request(cmg_stu_uploader* uploader, upload_request req) {
    if (uploader->count == uploader->capacity) {
        uint64_t new_cap = uploader->capacity * 2;
        upload_request* new_reqs = realloc(uploader->requests, sizeof(upload_request) * new_cap);
        if (!new_reqs) return 0;

        // Fix wrap-around alignment during realloc
        for (uint64_t i = 0; i < uploader->count; ++i) {
            new_reqs[i] = uploader->requests[(uploader->position + i) % uploader->capacity];
        }
        uploader->requests = new_reqs;
        uploader->position = 0;
        uploader->capacity = new_cap;
    }

    uploader->requests[(uploader->position + uploader->count) % uploader->capacity] = req;
    uploader->count++;
    return 1;
}

int cmg_stu_uploader_buffer_upload(
    cmg_stu_uploader* uploader, uint64_t bytes, void* memory, cmg_stu_memory_free_func free_func,
    fnd_gfx_buffer* dest_buffer, uint64_t dest_offset, uint64_t* out_timepoint
) {
    upload_request req = {
        .bytes      = bytes,
        .memory     = memory,
        .free_func  = free_func,
        .is_buffer  = 1,
        .timepoint  = ++uploader->uploads_it,
        .buffer     = { .dest_buffer = dest_buffer, .dest_offset = dest_offset }
    };

    if (!enqueue_request(uploader, req)) return 0;
    if (out_timepoint) *out_timepoint = req.timepoint;
    return 1;
}

int cmg_stu_uploader_texture_upload(
    cmg_stu_uploader* uploader, uint64_t bytes, void* memory, cmg_stu_memory_free_func free_func,
    fnd_gfx_texture* dest_texture, fnd_gfx_texture_dimensions dest_offset, fnd_gfx_texture_dimensions dest_dim, uint64_t* out_timepoint
) {
    upload_request req = {
        .bytes      = bytes,
        .memory     = memory,
        .free_func  = free_func,
        .is_buffer  = 0,
        .timepoint  = ++uploader->uploads_it,
        .texture    = { .dest_texture = dest_texture, .dest_offset = dest_offset, .dest_dim = dest_dim }
    };

    if (!enqueue_request(uploader, req)) return 0;
    if (out_timepoint) *out_timepoint = req.timepoint;
    return 1;
}

uint64_t cmg_stu_uploader_timepoint(cmg_stu_uploader* uploader) {
    return fnd_gfx_timeline_get_value(uploader->uploads_tl);
}

typedef struct upload_record_params {
    cmg_stu_uploader* uploader;
    uint64_t          items_to_remove;
    uint64_t          highest_completed_timepoint;
    uint64_t          bytes_written;
} upload_record_params;

static void upload_commands_record(void* raw_params) {
    upload_record_params* params = raw_params;
    cmg_stu_uploader* uploader = params->uploader;

    char* staging_map = (char*)fnd_gfx_staging_map(uploader->staging, 0, uploader->bandwidth);
    uint64_t offset = 0;

    while (offset < uploader->bandwidth && params->items_to_remove < uploader->count) {
        uint64_t left_bytes = uploader->bandwidth - offset;
        upload_request* req = &uploader->requests[(uploader->position + params->items_to_remove) % uploader->capacity];
        uint64_t taken_bytes;

        if (req->is_buffer) {
            taken_bytes = min_u64(req->bytes - req->current_offset, left_bytes);
            memcpy(staging_map + offset, (char*)req->memory + req->current_offset, taken_bytes);
            fnd_gfx_tcmd_copy_staging_to_buffer(
                uploader->staging, req->buffer.dest_buffer, offset,
                req->buffer.dest_offset + req->current_offset, taken_bytes
            );
        } 
        else {
            uint64_t pixel_bytes = fnd_gfx_texture_format_get_pixel_bytes(fnd_gfx_texture_query_format(req->texture.dest_texture));
            uint64_t left_pixels = left_bytes / pixel_bytes;
            if (left_pixels == 0) break; // Staging chunk too small for even a single pixel

            // Exact 3D coordinates based on pixels transferred so far
            uint64_t current_pixel_idx = req->current_offset / pixel_bytes;
            uint32_t current_x = current_pixel_idx % req->texture.dest_dim.width;
            uint32_t current_y = (current_pixel_idx / req->texture.dest_dim.width) % req->texture.dest_dim.height;
            uint32_t current_z = current_pixel_idx / (req->texture.dest_dim.width * req->texture.dest_dim.height);

            fnd_gfx_texture_dimensions copy_dim;
            uint64_t taken_pixels;

            if (current_x > 0) {
                // Mid-row: copy up to the end of the current row
                taken_pixels = min_u64(left_pixels, req->texture.dest_dim.width - current_x);
                copy_dim = (fnd_gfx_texture_dimensions){ (uint32_t)taken_pixels, 1, 1 };
            } 
            else {
                uint64_t row_pixels = req->texture.dest_dim.width;
                if (left_pixels < row_pixels) {
                    // Sub-row write (less than one full row fits in remaining staging space)
                    taken_pixels = left_pixels;
                    copy_dim = (fnd_gfx_texture_dimensions){ (uint32_t)taken_pixels, 1, 1 };
                } 
                else {
                    uint64_t fit_rows = left_pixels / row_pixels;
                    if (current_y > 0) {
                        // Mid-slice: copy up to the end of the current slice
                        uint64_t taken_rows = min_u64(fit_rows, req->texture.dest_dim.height - current_y);
                        taken_pixels = taken_rows * row_pixels;
                        copy_dim = (fnd_gfx_texture_dimensions){ req->texture.dest_dim.width, (uint32_t)taken_rows, 1 };
                    } 
                    else {
                        uint64_t fit_slices = fit_rows / req->texture.dest_dim.height;
                        uint64_t taken_slices = min_u64(fit_slices, req->texture.dest_dim.depth - current_z);
                        if (taken_slices > 0) {
                            // Start of a slice, fits one or more full slices
                            taken_pixels = taken_slices * req->texture.dest_dim.height * row_pixels;
                            copy_dim = (fnd_gfx_texture_dimensions){ req->texture.dest_dim.width, req->texture.dest_dim.height, (uint32_t)taken_slices };
                        } 
                        else {
                            // Less than a full slice
                            taken_pixels = fit_rows * row_pixels;
                            copy_dim = (fnd_gfx_texture_dimensions){ req->texture.dest_dim.width, (uint32_t)fit_rows, 1 };
                        }
                    }
                }
            }

            taken_bytes = taken_pixels * pixel_bytes;
            memcpy(staging_map + offset, (char*)req->memory + req->current_offset, taken_bytes);

            fnd_gfx_tcmd_copy_staging_to_texture(
                uploader->staging, req->texture.dest_texture, offset,
                (fnd_gfx_texture_dimensions){
                    req->texture.dest_offset.width  + current_x,
                    req->texture.dest_offset.height + current_y,
                    req->texture.dest_offset.depth  + current_z
                },
                copy_dim
            );
        }

        offset += taken_bytes;
        req->current_offset += taken_bytes;

        if (req->current_offset == req->bytes) {
            if (req->free_func) req->free_func(req->memory);
            params->highest_completed_timepoint = req->timepoint;
            params->items_to_remove++;
        }
    }

    fnd_gfx_staging_unmap(uploader->staging);
    params->bytes_written = offset;
}

void cmg_stu_uploader_enter(cmg_stu_uploader* uploader, int lock) {
    if (lock) fnd_gfx_timeline_wait(uploader->staging_tl, uploader->staging_it);
    else if (!fnd_gfx_timeline_is_after(uploader->staging_tl, uploader->staging_it)) return;
    if (uploader->count == 0) return;

    upload_record_params params = { .uploader = uploader };
    fnd_gfx_commands* commands = fnd_gfx_create_commands(uploader->hardware, &(fnd_gfx_commands_create_info){
        .domain = fnd_gfx_command_domain_transfer,
        .aindex = uploader->allocator,
        .parent = uploader->commands,
        .record = upload_commands_record,
        .params = &params
    });

    if (params.bytes_written == 0) {
        fnd_gfx_free_commands(commands); return;
    }

    uploader->commands = commands;
    fnd_gfx_commands_submit(1, &uploader->commands, &(fnd_gfx_submit_info){
        .domain_work_group  = uploader->transfer,
        .wait_count         = 0,
        .signal_count       = (params.items_to_remove > 0) ? 2 : 1,
        .signal_timelines   = (fnd_gfx_timeline*[]){uploader->staging_tl, uploader->uploads_tl},
        .signal_values      = (uint64_t[]){++uploader->staging_it, params.highest_completed_timepoint},
    });

    uploader->count   -= params.items_to_remove;
    uploader->position = (uploader->position + params.items_to_remove) % uploader->capacity;
}

#endif // COMPAGES_STAGING_UPLOADER_IMPL
