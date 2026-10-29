/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcompile_resources.c - Physical compiler memory ownership and admission
 *
 * KEY CONCEPT:
 *   Live blocks retain their owner, including all allocation metadata.
 *   Finite admission bounds held bytes, never cumulative allocation turnover.
 */
#include "xcompile_resources.h"
#include "xchecks.h"
#include "xmalloc.h"
#include <stdatomic.h>
#include <string.h>

typedef union CompileAllocation CompileAllocation;

struct XrCompileResources {
    XrCompileResourceLimits limits;
    XrCompileResourceStats stats;
    uint64_t references;
    CompileAllocation *allocations;
    atomic_bool locked;
};

/* Every caller already owns a reference while waiting for this lock. */
static void compile_lock(const XrCompileResources *resources) {
    atomic_bool *locked = &((XrCompileResources *) resources)->locked;
    for (;;) {
        if (!atomic_exchange_explicit(locked, true, memory_order_acquire)) return;
        while (atomic_load_explicit(locked, memory_order_relaxed)) XR_CPU_PAUSE();
    }
}

static void compile_unlock(XrCompileResources *resources) {
    atomic_store_explicit(&resources->locked, false, memory_order_release);
}

static bool compile_release_locked(XrCompileResources *resources) {
    XR_CHECK(resources->references, "A live compiler memory owner must have a reference");
    if (--resources->references) return false;
    XR_CHECK(!resources->allocations, "The last memory reference has no live payload registry");
    XR_CHECK(resources->stats.held_bytes == sizeof(*resources), "Outstanding compiler blocks retain their owner");
    return true;
}

/* Microsoft's C headers omit max_align_t; these scalar types cover its
 * fundamental alignments without depending on an OS allocation API. */
#if defined(XR_COMPILER_MSVC)
typedef union CompileMaxAlignment {
    long double floating;
    uint64_t integer;
    void *pointer;
} CompileMaxAlignment;
#else
typedef max_align_t CompileMaxAlignment;
#endif

union CompileAllocation {
    struct {
        XrCompileResources *resources;
        size_t bytes;
        CompileAllocation *left, *right;
        unsigned height;
    } info;
    CompileMaxAlignment alignment;
};

_Static_assert(sizeof(size_t) <= sizeof(uint64_t), "Allocation sizes must fit memory accounting");
_Static_assert(sizeof(CompileAllocation) % _Alignof(CompileMaxAlignment) == 0,
    "Allocation payload must preserve fundamental alignment");

static uint64_t compile_saturated_add(uint64_t value, uint64_t added) {
    return added > UINT64_MAX - value ? UINT64_MAX : value + added;
}

XR_FUNC XrCompileResourceStatus xr_compile_resources_new(
    const XrCompileResourceLimits *limits, XrCompileResources **output) {
    if (!limits || !output || *output ||
        (limits->memory_mode != XR_COMPILE_MEMORY_ORDINARY && limits->memory_mode != XR_COMPILE_MEMORY_FINITE) ||
        (limits->memory_mode == XR_COMPILE_MEMORY_ORDINARY && limits->memory_limit))
        return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    if (limits->memory_mode == XR_COMPILE_MEMORY_FINITE && sizeof(XrCompileResources) > limits->memory_limit)
        return XR_COMPILE_RESOURCE_MEMORY_LIMIT;
    XrCompileResources *resources = xr_malloc(sizeof(*resources));
    if (!resources) return XR_COMPILE_RESOURCE_OUT_OF_MEMORY;
    resources->limits = *limits;
    resources->stats = (XrCompileResourceStats) {sizeof(*resources), sizeof(*resources),
        limits->profile_allocations ? 1 : 0, limits->profile_allocations ? sizeof(*resources) : 0};
    resources->references = 1;
    resources->allocations = NULL;
    atomic_init(&resources->locked, false);
    *output = resources;
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC XrCompileResourceStatus xr_compile_resources_retain(XrCompileResources *resources) {
    if (!resources) return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    compile_lock(resources);
    XrCompileResourceStatus status = XR_COMPILE_RESOURCE_OVERFLOW;
    if (resources->references != UINT64_MAX) {
        ++resources->references;
        status = XR_COMPILE_RESOURCE_OK;
    }
    compile_unlock(resources);
    return status;
}

XR_FUNC void xr_compile_resources_release(XrCompileResources *resources) {
    if (!resources) return;
    compile_lock(resources);
    bool last = compile_release_locked(resources);
    compile_unlock(resources);
    /* No legal caller can still be waiting without a retained reference. */
    if (last) xr_free(resources);
}

XR_FUNC XrCompileResourceStatus xr_compile_resources_stats(
    const XrCompileResources *resources, XrCompileResourceStats *output) {
    if (!resources || !output) return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    compile_lock(resources);
    *output = resources->stats;
    compile_unlock((XrCompileResources *) resources);
    return XR_COMPILE_RESOURCE_OK;
}

/* The intrusive AVL is the sole live registry. Its links are charged in each
 * allocation header, so index insertion and deletion cannot fail or allocate.
 * Balanced height bounds recursion by the number of addressable live blocks. */
static unsigned compile_index_height(const CompileAllocation *node) {
    return node ? node->info.height : 0;
}
static void compile_index_height_update(CompileAllocation *node) {
    unsigned left=compile_index_height(node->info.left),right=compile_index_height(node->info.right);
    node->info.height=1+(left>right?left:right);
}
static CompileAllocation *compile_index_left(CompileAllocation *node) {
    CompileAllocation *root=node->info.right;
    node->info.right=root->info.left;root->info.left=node;
    compile_index_height_update(node);compile_index_height_update(root);return root;
}
static CompileAllocation *compile_index_right(CompileAllocation *node) {
    CompileAllocation *root=node->info.left;
    node->info.left=root->info.right;root->info.right=node;
    compile_index_height_update(node);compile_index_height_update(root);return root;
}
static CompileAllocation *compile_index_balance(CompileAllocation *node) {
    compile_index_height_update(node);
    unsigned left=compile_index_height(node->info.left),right=compile_index_height(node->info.right);
    if(left>right+1) {
        if(compile_index_height(node->info.left->info.right)>compile_index_height(node->info.left->info.left))
            node->info.left=compile_index_left(node->info.left);
        return compile_index_right(node);
    }
    if(right>left+1) {
        if(compile_index_height(node->info.right->info.left)>compile_index_height(node->info.right->info.right))
            node->info.right=compile_index_right(node->info.right);
        return compile_index_left(node);
    }
    return node;
}
static CompileAllocation *compile_index_insert(CompileAllocation *root,CompileAllocation *node) {
    if(!root)return node;
    uintptr_t address=(uintptr_t)(node+1),begin=(uintptr_t)(root+1);
    XR_CHECK(address!=begin,"Live allocation starts are distinct");
    if(address<begin)root->info.left=compile_index_insert(root->info.left,node);
    else root->info.right=compile_index_insert(root->info.right,node);
    return compile_index_balance(root);
}
static CompileAllocation *compile_index_detach_min(CompileAllocation *root,CompileAllocation **minimum) {
    if(!root->info.left){*minimum=root;return root->info.right;}
    root->info.left=compile_index_detach_min(root->info.left,minimum);
    return compile_index_balance(root);
}
static CompileAllocation *compile_index_remove(CompileAllocation *root,CompileAllocation *node) {
    XR_CHECK(root,"A released live block belongs to its owner's index");
    if(root==node) {
        if(!root->info.left)return root->info.right;
        if(!root->info.right)return root->info.left;
        CompileAllocation *successor=NULL;
        CompileAllocation *right=compile_index_detach_min(root->info.right,&successor);
        /* Reconnect the actual successor allocation. Payloads, byte counts and
         * owners never move between headers when a two-child node is erased. */
        successor->info.left=root->info.left;successor->info.right=right;
        return compile_index_balance(successor);
    }
    if((uintptr_t)(node+1)<(uintptr_t)(root+1))root->info.left=compile_index_remove(root->info.left,node);
    else root->info.right=compile_index_remove(root->info.right,node);
    return compile_index_balance(root);
}
/* Only trusted live headers are traversed. Proposed addresses are numeric
 * keys, never pointer probes. Disjoint payloads need only a predecessor search. */
static CompileAllocation *compile_find_locked(XrCompileResources *resources,
    const void *memory, size_t bytes, bool exact) {
    uintptr_t address = (uintptr_t)memory;
    CompileAllocation *node=resources->allocations,*hit=NULL;
    while(node) {
        uintptr_t begin=(uintptr_t)(node+1);
        if(address<begin)node=node->info.left;
        else {
            hit=node;if(address==begin)break;node=node->info.right;
        }
    }
    if(hit) {
        uintptr_t begin=(uintptr_t)(hit+1),offset=address-begin;
        if((!exact||address==begin)&&offset<hit->info.bytes&&
            bytes<=hit->info.bytes-(size_t)offset)return hit;
    }
    return NULL;
}

XR_FUNC XrCompileResourceStatus xr_compile_resources_allocation(XrCompileResources *resources,
    const void *memory, size_t bytes) {
    if (!resources || !memory || !bytes) return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    compile_lock(resources);
    bool found = compile_find_locked(resources, memory, bytes, true) != NULL;
    compile_unlock(resources);
    return found ? XR_COMPILE_RESOURCE_OK : XR_COMPILE_RESOURCE_BAD_ARGUMENT;
}

XR_FUNC XrCompileResourceStatus xr_compile_resources_scan_delimiter(XrCompileResources *resources,
    const char *source, size_t length, unsigned char delimiter, size_t *advanced, bool *found) {
    if (!resources || (!source && length) || length > 64 || !advanced || !found)
        return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    size_t position = 0;
    while (position < length && (unsigned char)source[position] != delimiter) ++position;
    *advanced = position;
    *found = position < length;
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC XrCompileResourceStatus xr_compile_resources_extent(XrCompileResources *resources,
    const void *memory, size_t bytes) {
    if (!resources || !memory || !bytes) return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    compile_lock(resources);
    CompileAllocation *hit = compile_find_locked(resources, memory, bytes, false);
    compile_unlock(resources);
    return hit ? XR_COMPILE_RESOURCE_OK : XR_COMPILE_RESOURCE_BAD_ARGUMENT;
}

static XrCompileResourceStatus compile_allocation_admit_locked(XrCompileResources *resources, size_t bytes) {
    if (bytes > SIZE_MAX - sizeof(CompileAllocation)) return XR_COMPILE_RESOURCE_OVERFLOW;
    size_t charged = sizeof(CompileAllocation) + bytes;
    if (charged > UINT64_MAX - resources->stats.held_bytes || resources->references == UINT64_MAX)
        return XR_COMPILE_RESOURCE_OVERFLOW;
    if (resources->limits.memory_mode == XR_COMPILE_MEMORY_FINITE &&
        charged > resources->limits.memory_limit - resources->stats.held_bytes)
        return XR_COMPILE_RESOURCE_MEMORY_LIMIT;
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC XrCompileResourceStatus xr_compile_resources_admit(XrCompileResources *resources, size_t bytes) {
    if (!resources || !bytes) return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    compile_lock(resources);
    XrCompileResourceStatus status = compile_allocation_admit_locked(resources, bytes);
    compile_unlock(resources);
    return status;
}

static XrCompileResourceStatus compile_allocate_locked(XrCompileResources *resources, size_t bytes, void **output) {
    XrCompileResourceStatus status = compile_allocation_admit_locked(resources, bytes);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    size_t charged = sizeof(CompileAllocation) + bytes;
    CompileAllocation *allocation = xr_malloc(charged);
    if (!allocation) return XR_COMPILE_RESOURCE_OUT_OF_MEMORY;
    allocation->info.resources = resources;
    allocation->info.bytes = bytes;
    allocation->info.left = allocation->info.right = NULL;
    allocation->info.height = 1;
    resources->allocations = compile_index_insert(resources->allocations,allocation);
    ++resources->references;
    if (resources->limits.profile_allocations) {
        resources->stats.allocation_count = compile_saturated_add(resources->stats.allocation_count, 1);
        resources->stats.allocated_bytes = compile_saturated_add(resources->stats.allocated_bytes, charged);
    }
    resources->stats.held_bytes += charged;
    if (resources->stats.held_bytes > resources->stats.peak_bytes)
        resources->stats.peak_bytes = resources->stats.held_bytes;
    *output = allocation + 1;
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC XrCompileResourceStatus xr_compile_resources_alloc(
    XrCompileResources *resources, size_t bytes, void **output) {
    if (!resources || !bytes || !output || *output) return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    compile_lock(resources);
    XrCompileResourceStatus status = compile_allocate_locked(resources, bytes, output);
    compile_unlock(resources);
    return status;
}

XR_FUNC XrCompileResourceStatus xr_compile_resources_calloc(
    XrCompileResources *resources, size_t count, size_t size, void **output) {
    if (!resources || !count || !size || !output || *output) return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    if (count > SIZE_MAX / size) return XR_COMPILE_RESOURCE_OVERFLOW;
    size_t bytes = count * size;
    void *memory = NULL;
    compile_lock(resources);
    XrCompileResourceStatus status = compile_allocate_locked(resources, bytes, &memory);
    if (status == XR_COMPILE_RESOURCE_OK) {
        memset(memory, 0, bytes);
        *output = memory;
    }
    compile_unlock(resources);
    return status;
}

static bool compile_free_locked(XrCompileResources *resources, CompileAllocation *allocation) {
    size_t charged = sizeof(*allocation) + allocation->info.bytes;
    XR_CHECK(resources->stats.held_bytes >= charged, "Held compiler bytes cover the released block");
    resources->allocations = compile_index_remove(resources->allocations,allocation);
    xr_free(allocation);
    resources->stats.held_bytes -= charged;
    return compile_release_locked(resources);
}

XR_FUNC void xr_compile_resources_free(void *memory) {
    if (!memory) return;
    CompileAllocation *allocation = (CompileAllocation *)memory - 1;
    XrCompileResources *resources = allocation->info.resources;
    compile_lock(resources);
    bool last = compile_free_locked(resources, allocation);
    compile_unlock(resources);
    if (last) xr_free(resources);
}

XR_FUNC XrCompileResourceStatus xr_compile_resources_resize(
    XrCompileResources *resources, void **memory, size_t bytes) {
    if (!resources || !memory) return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    if (!*memory) return bytes ? xr_compile_resources_alloc(resources, bytes, memory) : XR_COMPILE_RESOURCE_OK;
    compile_lock(resources);
    CompileAllocation *allocation = compile_find_locked(resources, *memory, 1, true);
    if (!allocation) {
        compile_unlock(resources);
        return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    }
    if (!bytes) {
        bool last = compile_free_locked(resources, allocation);
        *memory = NULL;
        compile_unlock(resources);
        if (last) xr_free(resources);
        return XR_COMPILE_RESOURCE_OK;
    }
    size_t old_bytes = allocation->info.bytes;
    if (old_bytes == bytes) {
        compile_unlock(resources);
        return XR_COMPILE_RESOURCE_OK;
    }
    size_t copied = old_bytes < bytes ? old_bytes : bytes;
    void *replacement = NULL;
    XrCompileResourceStatus status = compile_allocate_locked(resources, bytes, &replacement);
    if (status == XR_COMPILE_RESOURCE_OK) {
        memcpy(replacement, *memory, copied);
        bool last = compile_free_locked(resources, allocation);
        XR_CHECK(!last, "The replacement block retains its compiler memory owner");
        *memory = replacement;
    }
    compile_unlock(resources);
    return status;
}
