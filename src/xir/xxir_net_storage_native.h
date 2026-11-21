/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_net_storage_native.h - Opaque host storage classes of the network module
 *
 * KEY CONCEPT:
 *   A storage class names a host-owned body and carries no source fields. The
 *   source layer may name the type and pass it back to the native query that
 *   produced it; it can never read, write, construct or hash the body.
 */
#ifndef XXIR_NET_STORAGE_NATIVE_H
#define XXIR_NET_STORAGE_NATIVE_H
#include <string.h>
#include "xxir_types.h"
#include "xxir_native_opaque.h"
#include "../shared/xnative_declaration.h"

typedef struct XirNetStorageContract {
    uint32_t id;
    const char *name;
} XirNetStorageContract;

static const XirNetStorageContract xir_net_storage_contracts[] = {
    {XR_NATIVE_DECLARATION_NET_CONN_STORAGE, "__NetConnStorage"},
    {XR_NATIVE_DECLARATION_NET_LISTENER_STORAGE, "__NetListenerStorage"},
    {XR_NATIVE_DECLARATION_TLS_CONTEXT_STORAGE, "__TlsContextStorage"}
};

/* A storage class is exactly its canonical declaration: the storage kind, the
 * expected name, no fields and no members of any kind. */
static inline bool xir_net_storage_declaration(uint32_t id, const XrNativeTypeDeclaration **output) {
    if (!output || !xir_native_opaque_storage(id)) return false;
    const XirNetStorageContract *contract = NULL;
    for (uint32_t n = 0; n < sizeof(xir_net_storage_contracts) / sizeof(*xir_net_storage_contracts); ++n)
        if (xir_net_storage_contracts[n].id == id) { contract = xir_net_storage_contracts + n; break; }
    if (!contract) return false;
    const XrNativeTypeDeclaration *native = xr_native_declaration_by_id(id);
    if (!native || native->kind != XR_NATIVE_DECLARATION_STORAGE || native->members || native->member_count ||
        native->parameter_count || native->parameter_names || !native->name ||
        strcmp(native->name, contract->name) || !xr_native_declaration_validate(native)) return false;
    *output = native;
    return true;
}
#endif // XXIR_NET_STORAGE_NATIVE_H
