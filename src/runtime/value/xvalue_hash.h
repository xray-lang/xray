/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvalue_hash.h - XrValue-aware hash functions
 *
 * KEY CONCEPT:
 *   Hash functions for XrValue types, used by Map/Set.
 *   Built on top of xhash.h primitive hash functions.
 *
 * RELATED MODULES:
 *   - xhash.h: Pure hash functions (no XrValue dependency)
 *   - xmap.h: Runtime Map object (uses this)
 *   - xset.h: Runtime Set object (uses this)
 */

#ifndef XVALUE_HASH_H
#define XVALUE_HASH_H

#include "xvalue.h"
#include "../../base/xhash.h"

// Forward declaration
typedef struct XrString XrString;

// Null hash: non-zero to avoid collision with tombstone (0)
#define XR_HASH_NULL 6u

// Hash never returns 0 (0 is used for tombstone)
XR_FUNC uint32_t xr_hash_value(XrValue val);
XR_FUNC uint32_t xr_hash_string(XrString *str);

// Shallow value equality: primitives by value, strings by content, objects by pointer
XR_FUNC bool xr_value_eq(XrValue a, XrValue b);

/* Instance hash / equality hooks.
 *
 * A class that implements Hashable by hand — declaring `hash() -> int` and
 * `operator ==` — must key a Map or Set by value, not by the object's address.
 * Computing that means invoking the user's own methods, which only an execution
 * backend can do, so the VM installs these hooks at startup. `@derive(Hash)`
 * keeps its inline structural path and never reaches them.
 *
 * When no hook is installed (pure tooling, or the AOT runtime, which carries
 * its own xrt_* hash/eq), instance keys fall back to pointer identity exactly
 * as before. The hash hook returns true and writes *out_hash when it handled
 * the value; the eq hook returns 0 or 1 when it handled the pair, or -1 to
 * defer to the default. The non-moving heap keeps every borrowed pointer valid
 * across the reentrant call the hooks make. */
typedef bool (*XrValueInstanceHashHook)(XrValue key, uint32_t *out_hash);
typedef int (*XrValueInstanceEqHook)(XrValue a, XrValue b);
XR_FUNC void xr_value_set_instance_hooks(XrValueInstanceHashHook hash_hook,
                                         XrValueInstanceEqHook eq_hook);

#endif  // XVALUE_HASH_H
