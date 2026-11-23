/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_native_opaque.h - Nominals the host owns and source can only name
 *
 * KEY CONCEPT:
 *   An opaque native nominal has no source AST. The source layer may name the
 *   type and hand it back to the native query that produced it, and nothing
 *   else: no fields, no methods, no interfaces, no construction, no hashing.
 *   The generated storage registry supplies the private class identities.
 */
#ifndef XXIR_NATIVE_OPAQUE_H
#define XXIR_NATIVE_OPAQUE_H
#include "../shared/xnative_declaration.h"

/* The fieldless storage classes: they name a host body and carry no shape. */
static inline bool xir_native_opaque_storage(uint32_t id) {
    return id == XR_NATIVE_DECLARATION_NET_CONN_STORAGE ||
        id == XR_NATIVE_DECLARATION_NET_LISTENER_STORAGE ||
        id == XR_NATIVE_DECLARATION_TLS_CONTEXT_STORAGE ||
        id == XR_NATIVE_DECLARATION_FILE_STORAGE;
}

static inline bool xir_native_opaque_nominal(uint32_t id) {
    return id == XR_NATIVE_DECLARATION_FILE_STAT || xir_native_opaque_storage(id);
}
#endif // XXIR_NATIVE_OPAQUE_H
