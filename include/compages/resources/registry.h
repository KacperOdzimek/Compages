/*
----------------------------------------------------------------
Contents:
This file implements a generic resource registry, an thread-safe asset manager.

----------------------------------------------------------------
Code info:
- cmg_rsr prefix
- COMPAGES_REGISTRY_IMPL macro to build
- fundation/platform/threads.h dependent

----------------------------------------------------------------
Usage:
- Create resource registry with valid load/unload callbacks
- In app, query for assets when needed with find/load
- Once called to load and asset via callback, do whatever load logic, and push resource with cmg_rsr_push
- Pushes and unloads are sync at cmg_rsr_enter (a sync phase, blocking concurrent access)

----------------------------------------------------------------
Notes:
- During cmg_rsr_enter phase, underlying handle pointers may be changed, and the current ones may be unloaded
    It is recommended to block work on any thread at that time
- Registry unlike all other object is internally thread safe for convenience
*/

#ifndef COMPAGES_REGISTRY_H
#define COMPAGES_REGISTRY_H

#include <stdint.h>

typedef struct cmg_rsr_handle   cmg_rsr_handle;
typedef struct cmg_rsr_registry cmg_rsr_registry;

// Resource state inside registry, get with cmg_rsr_stat
typedef enum cmg_rsr_state_result {
    cmg_rsr_state_loaded = 0,   // Returned when asset is loaded
    cmg_rsr_state_unloaded,     // Returned when asset is unloaded/invalid/pending
    cmg_rsr_state_unowned,      // Returned when given handle is not from this registry
} cmg_rsr_state_result;

// Load callback
// Note this function does not have to create a object!
// It may enqueue load for async load
// Actuall resource add with cmg_rsr_push
typedef void(cmg_rsr_load_func_signature)(cmg_rsr_registry*, const char* path);
typedef cmg_rsr_load_func_signature* cmg_rsr_load_func;

// Unload callback
// Shall get rid of asset
typedef void(cmg_rsr_unload_func_signature)(cmg_rsr_registry*, void* asset);
typedef cmg_rsr_unload_func_signature* cmg_rsr_unload_func;

typedef struct cmg_rsr_registry_create_info {
    uint16_t            gc_frequency;   // Frames interval between GC walks
    cmg_rsr_load_func   load_func;      // Resource missing callback
    cmg_rsr_unload_func unload_func;    // Resource unload callback
} cmg_rsr_registry_create_info;

cmg_rsr_registry* cmg_rsr_create_registry(const cmg_rsr_registry_create_info*);
void cmg_rsr_free_registry(cmg_rsr_registry*);

// Returns pointer to resource only if in registry
cmg_rsr_handle* cmg_rsr_find(cmg_rsr_registry*, const char* path);

// Always return a pointer to resource
// If unloaded, causes load of resource
cmg_rsr_handle* cmg_rsr_load(cmg_rsr_registry*, const char* path);

// Locks element from being unloaded (stacks on uint64 counter)
// This does not prevents swapping with cmg_rsr_push
void cmg_rsr_lock(cmg_rsr_registry*, cmg_rsr_handle*);

// Unlocks element from being unloaded
void cmg_rsr_unlock(cmg_rsr_registry*, cmg_rsr_handle*);

// Returns current state of asset
// Quering state prevents asset from being unloaded in this frame
cmg_rsr_state_result cmg_rsr_state(cmg_rsr_registry*, cmg_rsr_handle*);

// Gets underlying asset from handle
// Retuned pointer is only valid if cmg_rsr_stat returned cmg_rsr_state_loaded
const void* cmg_rsr_access(cmg_rsr_registry*, cmg_rsr_handle*);

// Causes registry to enqueue unload resource at handle
// Resolved at registry enter, before handles unchanged
void cmg_rsr_unload(cmg_rsr_registry*, cmg_rsr_handle*);

// Pushes asset to registry
// If unload, resource may be garbage collected, else it is only accessed through registry (externally managed)
// Push on a existing entry enqueue a unload of old resource, insertion of new, and does not invalidate handles.
// Resolved at registry enter, before handles unchanged
cmg_rsr_handle* cmg_rsr_push(cmg_rsr_registry*, const char* path, void* asset, int unload);

// Enters registry, shall happen every frame
void cmg_rsr_enter(cmg_rsr_registry*);

#endif // COMPAGES_REGISTRY_H

#ifdef COMPAGES_REGISTRY_IMPL

#include "fundatio/platform/threads.h"
#include <stdlib.h>
#include <string.h>

// ===========================
// Path trie

// Children of a node are kept as a sibling-linked list rather than a fixed
// 256-wide array, since paths are usually sparse over the byte range and
// this keeps a node at one cache line for the common case.
typedef struct trie_node {
    char                edge;       // Byte this node represents (unused on root)
    struct trie_node*   sibling;    // Next child of same parent
    struct trie_node*   child;      // First child
    cmg_rsr_handle*     handle;     // Non-NULL once this path has been find/load/push'd
} trie_node;

static trie_node* trie_find_child(trie_node* parent, char edge) {
    for (trie_node* node = parent->child; node; node = node->sibling) {
        if (node->edge == edge) return node;
    } return NULL;
}

// Read-only walk, caller must hold at least trie_lock (read)
static trie_node* trie_lookup(trie_node* root, const char* path) {
    trie_node* node = root;
    for (const char* c = path; *c && node; ++c) {
        node = trie_find_child(node, *c);
    } return node;
}

// Mutating walk, caller must hold trie_lock (write)
static trie_node* trie_insert(trie_node* root, const char* path) {
    trie_node* node = root;
    for (const char* c = path; *c; ++c) {
        trie_node* child = trie_find_child(node, *c);
        if (!child) {
            child = calloc(1, sizeof(trie_node));
            if (!child) return NULL;
            child->edge    = *c;
            child->sibling = node->child;
            node->child    = child;
        }
        node = child;
    }
    return node;
}

// ===========================
// Typedef

struct cmg_rsr_handle {
    cmg_rsr_state_result  state;        // Asset state
    void*                 asset;        // Asset pointer
    uint64_t              lock_count;   // Put locks count
    int                   owned;        // If non-zero registry calls unload_func on replace/GC/free
    int                   touched;      // If non-zero touched since last GC and shall not be GCed
};

typedef struct pending_request pending_request;
struct cmg_rsr_registry {
    cmg_rsr_load_func    load_func;
    cmg_rsr_unload_func  unload_func;
    uint16_t             gc_frequency;
    uint16_t             gc_counter;

    fnd_thr_mutex*       meta_lock;   // Protects per-handle asset/state/lock_count/touched

    trie_node*           root;
    fnd_thr_rwlock*      trie_lock;   // Protects trie structure (node insertion/lookup)

    fnd_thr_mutex*       queue_lock;  // Protects the pending queue below
    pending_request*     queue;
    uint64_t             queue_count;
    uint64_t             queue_capacity;
};

// ===========================
// Handle

static cmg_rsr_handle* create_handle(const char* path) {
    cmg_rsr_handle* handle = malloc(sizeof(cmg_rsr_handle));
    if (!handle) return NULL;
    *handle = (cmg_rsr_handle){
        .state = cmg_rsr_state_unloaded,
    }; return handle;
}

static void trie_free_tree_recursive(cmg_rsr_registry* registry, trie_node* node) {
    if (!node) return;

    trie_free_tree_recursive(registry, node->child);
    trie_free_tree_recursive(registry, node->sibling);

    if (node->handle) {
        cmg_rsr_handle* handle = node->handle;
        if (handle->owned && handle->state == cmg_rsr_state_loaded && handle->asset) {
            registry->unload_func(registry, handle->asset);
        }
        free(handle);
    }

    free(node);
}

// ===========================
// Pending push/unload queue

typedef struct pending_request {
    cmg_rsr_handle* handle;     // Target handle
    int             is_unload;  // Whether request is unload one
    void*           asset;      // Ignored when is_unload
    int             owned;      // Ignored when is_unload
} pending_request;

static int enqueue_pending(cmg_rsr_registry* registry, pending_request req) {
    fnd_thr_mutex_lock(registry->queue_lock);

    if (registry->queue_count == registry->queue_capacity) {
        uint64_t          new_cap   = registry->queue_capacity * 2;
        pending_request*  new_queue = realloc(registry->queue, sizeof(pending_request) * new_cap);
        if (!new_queue) { fnd_thr_mutex_unlock(registry->queue_lock); return 0; }
        registry->queue          = new_queue;
        registry->queue_capacity = new_cap;
    }
    registry->queue[registry->queue_count++] = req;

    fnd_thr_mutex_unlock(registry->queue_lock);
    return 1;
}

static void touch_handle(cmg_rsr_registry* registry, cmg_rsr_handle* handle) {
    fnd_thr_mutex_lock(registry->meta_lock);
    handle->touched = 1;
    fnd_thr_mutex_unlock(registry->meta_lock);
}

// Looks up path, inserting a fresh unloaded handle if not present.
// out_created is set non-zero only if this call is the one that created it
// (a racing thread landing in the write-lock section after us just adopts
// the handle we or it made, and does not create a second one).
static cmg_rsr_handle* find_or_insert_handle(cmg_rsr_registry* registry, const char* path, int* out_created) {
    *out_created = 0;

    fnd_thr_rwlock_lock_read(registry->trie_lock);
    trie_node*      node   = trie_lookup(registry->root, path);
    cmg_rsr_handle* handle = node ? node->handle : NULL;
    fnd_thr_rwlock_unlock_read(registry->trie_lock);

    if (handle) return handle;

    fnd_thr_rwlock_lock_write(registry->trie_lock);
    node = trie_insert(registry->root, path);
    if (node) {
        if (node->handle) {
            handle = node->handle; // Lost the race to another inserter, reuse theirs
        } 
        else {
            handle = create_handle(path);
            if (handle) {
                node->handle = handle;
                *out_created = 1;
            }
        }
    }
    fnd_thr_rwlock_unlock_write(registry->trie_lock);

    return handle;
}

// ===========================
// Registry

cmg_rsr_registry* cmg_rsr_create_registry(const cmg_rsr_registry_create_info* info) {
    if (!info->load_func || !info->unload_func) return NULL;

    cmg_rsr_registry* registry = malloc(sizeof(cmg_rsr_registry));
    if (!registry) goto _fail;

    *registry = (cmg_rsr_registry){
        .load_func      = info->load_func,
        .unload_func    = info->unload_func,
        .gc_frequency   = info->gc_frequency,
        .queue_capacity = 16,
    };

    registry->root       = calloc(1, sizeof(trie_node));
    registry->trie_lock  = fnd_thr_create_rwlock();
    registry->meta_lock  = fnd_thr_create_mutex(&(fnd_thr_mutex_create_info){ .recursive = 0 });
    registry->queue_lock = fnd_thr_create_mutex(&(fnd_thr_mutex_create_info){ .recursive = 0 });
    registry->queue      = malloc(sizeof(pending_request) * registry->queue_capacity);

    if (!registry->root || !registry->trie_lock || !registry->meta_lock ||
        !registry->queue_lock || !registry->queue) {
        goto _fail;
    }

    return registry;
_fail: cmg_rsr_free_registry(registry); return NULL;
}

void cmg_rsr_free_registry(cmg_rsr_registry* registry) {
    if (!registry) return;
    if (registry->root)  trie_free_tree_recursive(registry, registry->root);
    free(registry->queue);
    fnd_thr_free_rwlock(registry->trie_lock);
    fnd_thr_free_mutex(registry->meta_lock);
    fnd_thr_free_mutex(registry->queue_lock);
    free(registry);
}

cmg_rsr_handle* cmg_rsr_find(cmg_rsr_registry* registry, const char* path) {
    fnd_thr_rwlock_lock_read(registry->trie_lock);
    trie_node*      node   = trie_lookup(registry->root, path);
    cmg_rsr_handle* handle = node ? node->handle : NULL;
    fnd_thr_rwlock_unlock_read(registry->trie_lock);

    if (handle) touch_handle(registry, handle);
    return handle;
}

cmg_rsr_handle* cmg_rsr_load(cmg_rsr_registry* registry, const char* path) {
    int created = 0; cmg_rsr_handle* handle = find_or_insert_handle(registry, path, &created);
    if (!handle) return NULL;
    if (created) registry->load_func(registry, path);
    else touch_handle(registry, handle);
    return handle;
}

void cmg_rsr_lock(cmg_rsr_registry* registry, cmg_rsr_handle* handle) {
    if (!handle) return;
    fnd_thr_mutex_lock(registry->meta_lock);
    handle->lock_count++;
    fnd_thr_mutex_unlock(registry->meta_lock);
}

void cmg_rsr_unlock(cmg_rsr_registry* registry, cmg_rsr_handle* handle) {
    if (!handle) return;
    fnd_thr_mutex_lock(registry->meta_lock);
    if (handle->lock_count > 0) handle->lock_count--;
    fnd_thr_mutex_unlock(registry->meta_lock);
}

cmg_rsr_state_result cmg_rsr_state(cmg_rsr_registry* registry, cmg_rsr_handle* handle) {
    if (!handle) return cmg_rsr_state_unowned;

    fnd_thr_mutex_lock(registry->meta_lock);
    handle->touched = 1;
    cmg_rsr_state_result state = handle->state;
    fnd_thr_mutex_unlock(registry->meta_lock);

    return state;
}

const void* cmg_rsr_access(cmg_rsr_registry* registry, cmg_rsr_handle* handle) {
    if (!handle) return NULL;

    fnd_thr_mutex_lock(registry->meta_lock);
    const void* asset = (handle->state == cmg_rsr_state_loaded) ? handle->asset : NULL;
    fnd_thr_mutex_unlock(registry->meta_lock);

    return asset;
}

void cmg_rsr_unload(cmg_rsr_registry* registry, cmg_rsr_handle* handle) {
    if (!handle) return;
    enqueue_pending(registry, (pending_request){
        .handle    = handle,
        .is_unload = 1,
    });
}

cmg_rsr_handle* cmg_rsr_push(cmg_rsr_registry* registry, const char* path, void* asset, int unload) {
    int created = 0; // Unused here, push does not call load_func either way
    cmg_rsr_handle* handle = find_or_insert_handle(registry, path, &created);
    if (!handle) return NULL;

    if (!enqueue_pending(registry, (pending_request){
        .handle = handle,
        .asset  = asset,
        .owned  = unload,
    })) {
        return NULL;
    }

    return handle;
}

static void gc_walk(cmg_rsr_registry* registry, trie_node* node) {
    if (!node) return;
 
    gc_walk(registry, node->child);
    gc_walk(registry, node->sibling);
 
    cmg_rsr_handle* handle = node->handle;
    if (!handle) return;
 
    fnd_thr_mutex_lock(registry->meta_lock);
    int collectible = (
        handle->owned && handle->state == cmg_rsr_state_loaded &&
        handle->lock_count == 0 && !handle->touched
    ); handle->touched = 0;
    fnd_thr_mutex_unlock(registry->meta_lock);
 
    if (collectible) cmg_rsr_unload(registry, handle);
}

void cmg_rsr_enter(cmg_rsr_registry* registry) {
    fnd_thr_mutex_lock(registry->queue_lock);
 
    uint64_t index = 0;
    while (index < registry->queue_count) {
        pending_request req    = registry->queue[index];
        cmg_rsr_handle* handle = req.handle;
 
        void* old_asset = NULL;
        int   old_owned = 0;
 
        fnd_thr_mutex_lock(registry->meta_lock);
        if (handle->owned && handle->state == cmg_rsr_state_loaded) {
            old_asset = handle->asset;
            old_owned = 1;
        }
 
        if (req.is_unload) {
            handle->asset = NULL;
            handle->state = cmg_rsr_state_unloaded;
        } else {
            handle->asset = req.asset;
            handle->owned = req.owned;
            handle->state = cmg_rsr_state_loaded;
        }
        fnd_thr_mutex_unlock(registry->meta_lock);
 
        if (old_owned && old_asset) {
            registry->unload_func(registry, old_asset);
        }
 
        index++;
    }
 
    registry->queue_count = 0;
    fnd_thr_mutex_unlock(registry->queue_lock);
 
    if (registry->gc_frequency > 0) {
        registry->gc_counter++;
        if (registry->gc_counter >= registry->gc_frequency) {
            registry->gc_counter = 0;
            gc_walk(registry, registry->root);
        }
    }
}

#endif // COMPAGES_REGISTRY_IMPL
