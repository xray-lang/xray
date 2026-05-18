/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * stdlib_cache.h - Per-isolate cache shared across stdlib modules
 *
 * KEY CONCEPT:
 *   Several stdlib bindings need to memoise values that are cheap to build
 *   but only valid inside a single XrayIsolate (e.g. dynamic-layout XrClass
 *   instances that reference symbol IDs drawn from the isolate's symbol
 *   table). Storing the cache on XrayIsolate itself would leak stdlib types
 *   into the core header; instead we attach an opaque pointer to the
 *   isolate and keep the concrete fields private to stdlib.
 *
 * WHY THIS DESIGN:
 *   Previously, io.c used a single process-global static which silently
 *   broke in multi-isolate embeddings (SymbolIds are per-isolate, so the
 *   class built for isolate A could not be used by isolate B without
 *   crashing). Per-isolate caching moves ownership to the isolate
 *   lifecycle and eliminates the cross-isolate aliasing hazard.
 *
 * USAGE:
 *     XrStdlibCache *c = xr_stdlib_cache_get(isolate);
 *     if (!c->io_stat_class) {
 *         c->io_stat_class = build_stat_class(isolate);
 *     }
 *
 *   Cache is created lazily on first access and released by
 *   xr_stdlib_cache_free() during isolate shutdown.
 */

#ifndef XR_STDLIB_CACHE_H
#define XR_STDLIB_CACHE_H

#include <stdbool.h>
#include <stddef.h>

#include "../src/base/xdefs.h"
#include "../src/runtime/value/xvalue.h"

struct XrClass;
struct XrString;

// XML node-shape keys interned once per isolate and reused on every
// stdlib/xml parse / stringify / builder call. Previously the xml
// module called xr_string_intern() for "type", "tag", "attrs", ...
// on each binding entry; caching the XrValue handles here eliminates
// the per-call lookup cost (interned strings are stable within an
// isolate so the XrValue remains valid for its entire lifetime).
typedef struct XrStdlibXmlKeys {
    bool ready;
    XrValue type;
    XrValue tag;
    XrValue attrs;
    XrValue children;
    XrValue text;

    // Optional extra keys used by the basic namespace surface exposed
    // on element nodes. `namespaces` holds a Map<prefix -> URI>
    // (empty-prefix key "" represents the default namespace). Scripts
    // can derive `prefix`/`localName` by splitting `tag` on ':'.
    XrValue namespaces;

    struct XrString *str_element;
    struct XrString *str_text;
    struct XrString *str_comment;
    struct XrString *str_cdata;
    struct XrString *str_document;
} XrStdlibXmlKeys;

// Common parser error-map keys shared by json/yaml/toml/xml/csv parsers.
// All error Maps in stdlib use a { type, line, row, column, message }
// schema; interning these four keys once per isolate lets add_error()
// paths skip the (cheap but non-zero) xr_string_intern() hash lookup on
// every error push. The XrValues are stable for the isolate's lifetime
// because interned strings are pinned.
typedef struct XrStdlibErrKeys {
    bool ready;
    XrValue type;
    XrValue line;
    XrValue row;
    XrValue column;
    XrValue message;
} XrStdlibErrKeys;

typedef struct XrStdlibCache {
    // io module: dynamic-layout class for stat() return value. Built on
    // first call to io.stat() within this isolate. Holds symbol IDs that
    // are only valid for the owning isolate.
    struct XrClass *io_stat_class;

    // xml module: per-isolate interned key / type-name cache.
    XrStdlibXmlKeys xml_keys;

    // Parser error-map key cache (see XrStdlibErrKeys above).
    XrStdlibErrKeys err_keys;

    // Per-isolate log state (default logger, mutex, async queue).
    // The concrete struct lives privately in log.c; the cache only holds
    // an opaque pointer and a destructor so stdlib_cache.h does not depend
    // on log.h.  Set by log_state_get() on first use.
    void *log_state;
    void (*log_state_cleanup)(void *);
} XrStdlibCache;

// Retrieve (and lazily allocate) the per-isolate stdlib cache.
// Never returns NULL in practice; on allocator OOM it returns NULL,
// matching the xmalloc OOM policy used elsewhere by stdlib.
XR_FUNC XrStdlibCache *xr_stdlib_cache_get(struct XrayIsolate *isolate);

// Release the cache and every lazily-populated object it owns. Safe to
// call with a NULL isolate or with the cache already freed.
XR_FUNC void xr_stdlib_cache_free(struct XrayIsolate *isolate);

#endif  // XR_STDLIB_CACHE_H
