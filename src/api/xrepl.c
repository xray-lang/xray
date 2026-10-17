/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrepl.c - Remaining Xi symbol storage keys
 *
 * KEY CONCEPT:
 *   Xi lowering still consumes symbol-bound storage keys. Source REPL execution
 *   and inspection are owned by the public Source submission path.
 */

#include "xrepl.h"
#include <inttypes.h>
#include <stdio.h>

XR_FUNC bool xr_repl_binding_key(uint32_t symbol_id, char *buffer, size_t capacity) {
    if (!symbol_id || !buffer || !capacity)
        return false;
    int written = snprintf(buffer, capacity, "__xray_repl_binding$%" PRIu32, symbol_id);
    return written > 0 && (size_t) written < capacity;
}
