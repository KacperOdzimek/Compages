#ifndef COMPAGES_SYNCHRONISED_WINDOW_H
#define COMPAGES_SYNCHRONISED_WINDOW_H

#include "fundatio/platform/graphics.h"

typedef struct cmg_sww_window cmg_sww_window;

// Returns non-zero if shall break
typedef int(*cmg_sww_window_frame_func)(
    cmg_sww_window*     window,
    fnd_gfx_timeline*   can_render,
    uint64_t            can_render_signal,
    uint32_t            frame_in_flight_index,
    uint32_t            attachment_index
);

typedef struct cmg_sww_window_create_info {
    fnd_gfx_window_create_info  gfx_create_info;
    cmg_sww_window_frame_func   frame_func;
} cmg_sww_window_create_info;

cmg_sww_window* cmg_sww_create_window(fnd_gfx_hardware*, const cmg_sww_window_create_info* info);
void cmg_sww_free_window(cmg_sww_window*);

// Begin new frame
// If lock wait for old to finish
// If not lock, if old frame pending, return
void cmg_sww_window_enter(cmg_sww_window*, int lock);

// Add timeline to pending frame, which must be waited on before presenting
// Shall be called from frame func
void cmg_sww_window_add_wait(cmg_sww_window*, fnd_gfx_timeline* timeline, uint64_t value);

// Gets underlying gfx window from sww window
fnd_gfx_window* cmg_sww_window_get_gfx_window(cmg_sww_window*);

#endif // COMPAGES_SYNCHRONISED_WINDOW_H

#ifdef COMPAGES_SYNCHRONISED_WINDOW_IMPL

#include <stdlib.h>

struct cmg_sww_window {
    cmg_sww_window_frame_func frame_func;

    fnd_gfx_hardware*   owning_hardware;
    fnd_gfx_window*     window;
    
    uint32_t            in_flight;
    uint32_t            in_flight_itr;
    fnd_gfx_timeline*   timeline;
    uint64_t*           rendered;  // Array per frame in flight

    uint32_t            wait_count;
    uint32_t            wait_capacity;
    fnd_gfx_timeline**  wait_timelines;
    uint64_t*           wait_values;
    int                 wait_hardware;
};

cmg_sww_window* cmg_sww_create_window(fnd_gfx_hardware* hardware, const cmg_sww_window_create_info* info) {
    cmg_sww_window* window = malloc(sizeof(cmg_sww_window));
    *window = (cmg_sww_window){
        .owning_hardware = hardware,
        .frame_func      = info->frame_func,
        .in_flight       = info->gfx_create_info.attachments
    };

    window->window = fnd_gfx_create_window(hardware, &info->gfx_create_info);
    if (!window->window) goto _fail;

    window->timeline = fnd_gfx_create_timeline(hardware, &(fnd_gfx_timeline_create_info){
        .initial_value = 0
    }); if (!window->timeline) goto _fail;

    window->rendered = calloc(window->in_flight, sizeof(uint64_t));
    if (!window->rendered) goto _fail;
    
    return window;
_fail: cmg_sww_free_window(window); return NULL;
}

void cmg_sww_free_window(cmg_sww_window* window) {
    if (!window) return;
    fnd_gfx_free_window(window->window);
    fnd_gfx_free_timeline(window->timeline);
    free(window->wait_timelines);
    free(window->wait_values);
    free(window->rendered);
    free(window);
}

void cmg_sww_window_enter(cmg_sww_window* window, int lock) {
    uint32_t frame_in_flight = window->in_flight_itr;                                               // This frame in flight
    uint32_t previ_in_flight = frame_in_flight == 0 ? window->in_flight - 1 : frame_in_flight - 1;  // Previous frame in flight

    if (lock) { // Wait for previous cycle to complete
        fnd_gfx_timeline_wait(window->timeline, window->rendered[frame_in_flight]);
    } else {    // Check if completed, else opt-out
        if (!fnd_gfx_timeline_is_after(window->timeline, window->rendered[frame_in_flight])) return;
    }

    // Current timeline value
    uint64_t iterator = window->rendered[previ_in_flight];

    // Acquire next window attachment index
    uint32_t attachment; if (!fnd_gfx_window_acquire(
        window->window, window->timeline, ++iterator, &attachment)
    ) return;

    // Reset waits after previous frame
    window->wait_count = 0;

    // Always wait window acquired before presenting
    cmg_sww_window_add_wait(window, window->timeline, iterator);

    // Do frame logic
    window->frame_func(window, window->timeline, iterator, frame_in_flight, attachment);

    // Fallback path: failed to push wait, wait hardware for safety
    if (window->wait_hardware) {
        fnd_gfx_hardware_wait_idle(window->owning_hardware);
        fnd_gfx_window_present(window->window, attachment, 0, NULL, NULL, NULL, 0);
        window->wait_hardware = 0; return;
    }

    // Present
    fnd_gfx_window_present(
        window->window, attachment, window->wait_count, window->wait_timelines, window->wait_values, window->timeline, ++iterator
    ); window->rendered[frame_in_flight] = iterator;

    // Advance frame in flight
    window->in_flight_itr = (window->in_flight_itr + 1) % window->in_flight;
}

void cmg_sww_window_add_wait(cmg_sww_window* window, fnd_gfx_timeline* timeline, uint64_t value) {
    if (window->wait_count == window->wait_capacity) {
        uint32_t            new_cap             = window->wait_capacity ? window->wait_capacity * 2 : 8;
        fnd_gfx_timeline**  new_wait_timelines  = realloc(window->wait_timelines, new_cap * sizeof(fnd_gfx_timeline*));
        uint64_t*           new_wait_values     = realloc(window->wait_values, new_cap * sizeof(uint64_t));
        if (new_wait_timelines) window->wait_timelines = new_wait_timelines;
        if (new_wait_values)    window->wait_values    = new_wait_values;
        if (new_wait_timelines && new_wait_values) {
            window->wait_capacity = new_cap;
        }
        else {
            window->wait_hardware = 1; // Fallback, failed to push
            return;
        }
    }

    window->wait_timelines[window->wait_count] = timeline;
    window->wait_values[window->wait_count] = value;
    window->wait_count++;    
}

fnd_gfx_window* cmg_sww_window_get_gfx_window(cmg_sww_window* window) {
    return window->window;
}

#endif // COMPAGES_SYNCHRONISED_WINDOW_IMPL
