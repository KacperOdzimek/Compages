#ifndef COMPAGES_RESOURCE_REGISTRY_H
#define COMPAGES_RESOURCE_REGISTRY_H

#include "fundatio/platform/filesystem.h"

typedef void*(*cmg_rsr_load_func)(
    cmg_rsr_registry*   registry,
    fnd_vfs_filesystem* filesystem,
    const char*         path
);

typedef struct cmg_rsr_registry_create_info {
    fnd_vfs_filesystem* filesystem;
} cmg_rsr_registry_create_info;

typedef struct cmg_rsr_registry cmg_rsr_registry;
typedef struct cmg_rsr_handle cmg_rsr_handle;

cmg_rsr_handle* cmg_rsr_registry_get(const char* path);
cmg_rsr_handle* cmg_rsr_registry_add(const char* path, void* resource);

void* cmg_rsr_handle_get(cmg_rsr_handle*);

#endif // COMPAGES_RESOURCE_REGISTRY_H

#ifdef COMPAGES_RESOURCE_REGISTRY_IMPL

#endif COMPAGES_RESOURCE_REGISTRY_IMPL
