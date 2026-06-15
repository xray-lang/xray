/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_map_set_abi.h - Shared dense-entry Map/Set ABI.
 *
 * Dependency: the including header must define XrValue first. This keeps the
 * ABI usable from both the VM runtime and the standalone AOT runtime without
 * forcing either side to include the other's value header.
 */

#ifndef XR_MAP_SET_ABI_H
#define XR_MAP_SET_ABI_H

#include "xr_swiss_index.h"
#include <stdint.h>

/* ========== Map Entry (insertion-order dense slot) ========== */

typedef struct XrMapEntry {
    XrValue value;
    XrValue key;
    uint32_t hash;  /* Cached key hash (avoids recompute on resize/lookup) */
    uint8_t key_tt; /* Key type tag (+1); 0 = empty/tombstone slot */
    uint8_t _pad[3];
} XrMapEntry;

#define XR_MAP_ENTRY_NIL_KEY 0
#define XR_MAP_ENTRY_EMPTY(e) ((e)->key_tt == XR_MAP_ENTRY_NIL_KEY)

/* Debug sentinel for indices[] slots whose ctrl byte is not FULL. FULL slots
 * always store a direct entries[] index. */
#define XR_MAP_IX_EMPTY (-1)

#define XR_MAP_ABI_FIELDS                                                                          \
    uint32_t count;        /* Live entries (excludes tombstones) */                                \
    uint32_t nentries;     /* Used entries incl. tombstones (= next append index) */               \
    uint32_t entries_cap;  /* Allocated entries[] capacity */                                      \
    uint32_t indices_size; /* indices[] slot count (power of two, 0 = dummy) */                    \
    uint8_t *ctrl;         /* Swiss control bytes, indices_size + XR_SWISS_GROUP */                \
    int32_t *indices;      /* FULL ctrl slots -> entries index */                                  \
    XrMapEntry *entries;   /* Dense insertion-order array */                                       \
    uint8_t flags;                                                                                 \
    uint8_t key_tid;   /* XrTypeId / storage tag for reified generic keys (0=any) */               \
    uint8_t value_tid; /* XrTypeId / storage tag for reified generic values (0=any) */             \
    uint8_t _pad

typedef struct XrMapCore {
    XR_MAP_ABI_FIELDS;
} XrMapCore;

#define XR_MAP_FLAG_WEAK 0x01
#define XR_MAP_FLAG_DUMMY 0x02           /* Empty map: no ctrl/indices/entries allocation */
#define XR_MAP_FLAG_NODES_ON_GC 0x04     /* ctrl/indices/entries live on Region GC heap */
#define XR_MAP_FLAG_WEAK_REGISTERED 0x08 /* Registered in the runtime weak registry */
#define XR_MAP_FLAG_NODES_ON_STACK 0x10  /* AOT stack Map nodes; never free or resize */

#define xr_map_isdummy(m) ((m)->flags & XR_MAP_FLAG_DUMMY)

#define XR_MAP_MAXHBITS 30

/* ========== Set Entry (insertion-order dense slot) ========== */

typedef struct XrSetEntry {
    XrValue value;
    uint32_t hash;  /* Cached value hash (avoids recompute on resize/lookup) */
    uint8_t val_tt; /* Value type tag (+1); 0 = empty/tombstone slot */
    uint8_t _pad[3];
} XrSetEntry;

#define XR_SET_ENTRY_NIL 0
#define XR_SET_ENTRY_EMPTY(e) ((e)->val_tt == XR_SET_ENTRY_NIL)

/* Debug sentinel for indices[] slots whose ctrl byte is not FULL. FULL slots
 * always store a direct entries[] index. */
#define XR_SET_IX_EMPTY (-1)

#define XR_SET_ABI_FIELDS                                                                          \
    uint32_t count;        /* Live entries (excludes tombstones) */                                \
    uint32_t nentries;     /* Used entries incl. tombstones (= next append index) */               \
    uint32_t entries_cap;  /* Allocated entries[] capacity */                                      \
    uint32_t indices_size; /* indices[] slot count (power of two, 0 = dummy) */                    \
    uint8_t *ctrl;         /* Swiss control bytes, indices_size + XR_SWISS_GROUP */                \
    int32_t *indices;      /* FULL ctrl slots -> entries index */                                  \
    XrSetEntry *entries;   /* Dense insertion-order array */                                       \
    uint8_t flags;                                                                                 \
    uint8_t elem_tid; /* XrTypeId / storage tag for reified generic elements (0=any) */            \
    uint8_t _pad[2]

typedef struct XrSetCore {
    XR_SET_ABI_FIELDS;
} XrSetCore;

#define XR_SET_FLAG_WEAK 0x01
#define XR_SET_FLAG_DUMMY 0x02           /* Empty set: no ctrl/indices/entries allocation */
#define XR_SET_FLAG_NODES_ON_GC 0x04     /* ctrl/indices/entries live on Region GC heap */
#define XR_SET_FLAG_WEAK_REGISTERED 0x08 /* Registered in the runtime weak registry */
#define XR_SET_FLAG_NODES_ON_STACK 0x10  /* AOT stack Set nodes; never free or resize */

#define xr_set_isdummy(s) ((s)->flags & XR_SET_FLAG_DUMMY)

#define XR_SET_MAXHBITS 30

#endif  // XR_MAP_SET_ABI_H
