/*
----------------------------------------------------------------
Contents:
This file implements waiter object, checking whether uploader already have uploaded object at given timepoint.

----------------------------------------------------------------
Code info:
- cmg_stw prefix
- COMPAGES_STAGING_WAITER_IMPL macro to build
- compages/utility/staging_uploader.h dependent

----------------------------------------------------------------
Usage:
- Create waiter linked to uploader object
- After pushing an upload with uploader, user can push a callback at timepoint to a waiter
- In case of multi-threaded program uploader and waiter must be mutexed together, to ensure
    timepoint increasing order at waiter (this is required not to look all waits, only first ones).

----------------------------------------------------------------
Notes:
- Push order is assumed non-decreasing in timepoint. Enter relies on this to stop
    scanning at the first not-yet-completed entry rather than walking the whole queue.
*/

#ifndef COMPAGES_STAGING_WAITER_H
#define COMPAGES_STAGING_WAITER_H

#include "compages/staging/staging_uploader.h"
#include <stdint.h>

typedef void(cmg_stw_waiter_func_signature)(void* payload);
typedef cmg_stw_waiter_func_signature* cmg_stw_waiter_func;

typedef struct cmg_stw_waiter_create_info {
    cmg_stu_uploader* uploader;
} cmg_stw_waiter_create_info;

typedef struct cmg_stw_waiter cmg_stw_waiter;

cmg_stw_waiter* cmg_stw_create_waiter(const cmg_stw_waiter_create_info*);
void cmg_stw_free_waiter(cmg_stw_waiter*);

// Pushes a callback, fired once uploader reaches timepoint
// Caller must push in non-decreasing timepoint order
void cmg_stw_waiter_push(cmg_stw_waiter*, cmg_stw_waiter_func func, void* payload, uint64_t timepoint);

// Advances through queue, firing every callback whose timepoint is now reached
// Stops at first not-yet-reached entry (queue is ordered)
void cmg_stw_waiter_enter(cmg_stw_waiter*);

#endif // COMPAGES_STAGING_WAITER_H

#ifdef COMPAGES_STAGING_WAITER_IMPL

#include <stdlib.h>
#include <string.h>

// ===========================
// Typedef

typedef struct cmg_stw_wait_entry {
    cmg_stw_waiter_func func;
    void*               payload;
    uint64_t            timepoint;
} cmg_stw_wait_entry;

struct cmg_stw_waiter {
    cmg_stu_uploader*   uploader;
    cmg_stw_wait_entry* queue;      // Ring buffer, capacity always a power of 2 for cheap masking
    uint64_t            head;       // Index (unmasked) of oldest pending entry
    uint64_t            count;      // Number of pending entries
    uint64_t            capacity;
};

// ===========================
// Waiter

cmg_stw_waiter* cmg_stw_create_waiter(const cmg_stw_waiter_create_info* info) {
    if (!info->uploader) return NULL;

    cmg_stw_waiter* waiter = malloc(sizeof(cmg_stw_waiter));
    if (!waiter) return NULL;

    *waiter = (cmg_stw_waiter){
        .uploader = info->uploader,
        .capacity = 16,
    };

    waiter->queue = malloc(sizeof(cmg_stw_wait_entry) * waiter->capacity);
    if (!waiter->queue) { cmg_stw_free_waiter(waiter); return NULL; }

    return waiter;
}

void cmg_stw_free_waiter(cmg_stw_waiter* waiter) {
    if (!waiter) return;
    free(waiter->queue);
    free(waiter);
}

static int grow_queue(cmg_stw_waiter* waiter) {
    uint64_t            new_cap   = waiter->capacity * 2;
    cmg_stw_wait_entry* new_queue = malloc(sizeof(cmg_stw_wait_entry) * new_cap);
    if (!new_queue) return 0;

    for (uint64_t i = 0; i < waiter->count; ++i) {
        new_queue[i] = waiter->queue[(waiter->head + i) & (waiter->capacity - 1)];
    }

    free(waiter->queue);
    waiter->queue    = new_queue;
    waiter->head     = 0;
    waiter->capacity = new_cap;
    return 1;
}

void cmg_stw_waiter_push(cmg_stw_waiter* waiter, cmg_stw_waiter_func func, void* payload, uint64_t timepoint) {
    if (waiter->count == waiter->capacity) {
        if (!grow_queue(waiter)) return; // Out of memory, drop the push
    }

    uint64_t slot = (waiter->head + waiter->count) & (waiter->capacity - 1);
    waiter->queue[slot] = (cmg_stw_wait_entry){
        .func      = func,
        .payload   = payload,
        .timepoint = timepoint,
    };
    waiter->count++;
}

void cmg_stw_waiter_enter(cmg_stw_waiter* waiter) {
    uint64_t completed = cmg_stu_uploader_timepoint(waiter->uploader);
    uint64_t mask       = waiter->capacity - 1;

    uint64_t fired = 0; while (fired < waiter->count && waiter->queue[(waiter->head + fired) & mask].timepoint <= completed) {
        fired++;
    }

    for (uint64_t i = 0; i < fired; ++i) {
        cmg_stw_wait_entry* entry = &waiter->queue[(waiter->head + i) & mask];
        entry->func(entry->payload);
    }

    waiter->head   = (waiter->head + fired) & mask;
    waiter->count -= fired;
}

#endif // COMPAGES_STAGING_WAITER_IMPL
