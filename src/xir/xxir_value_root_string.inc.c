/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_value_root_string.inc.c - Single-block Strings owned by a native root
 *
 * KEY CONCEPT:
 *   A root String lives in a root domain that is never closed. Until another
 *   holder exists, its root thread allocates, recycles and accounts for it
 *   without the domain lock, an atomic or a graph operation. Escape moves it
 *   to the shared accounting and pins the domain before any second holder.
 */
/* A root String stores its first body in the header block; capacity covers
 * that body and the separately allocated body that replaces it on growth.
 * Until it escapes, the root's single reference is its only one: it pins no
 * domain reference (the root outlives it) and releases without an atomic.
 * Escape is marked on the root thread before any other holder exists. */
typedef struct XirRootString {
    XirString string;
    size_t block_bytes;
    bool escaped;
} XirRootString;
static bool string_body_inline(const XirString *string) {
    return string->object.root_private && string->bytes == (const char *)((const XirRootString *)string + 1);
}
#define ROOT_BLOCK_MINIMUM ((sizeof(XirRootString) + ROOT_BLOCK_GRANULE) & \
    ~(size_t)(ROOT_BLOCK_GRANULE - 1))
static size_t root_block_class(size_t bytes) {
    return (bytes - ROOT_BLOCK_MINIMUM) / ROOT_BLOCK_GRANULE;
}
/* A root domain is unbound, so its only admission is the live-byte limit.
 * An unbounded root admits on its own ledger; a bounded one also reads the
 * shared live bytes under the domain lock. */
static bool root_admit(XirRootDomain *root, uint64_t private_bytes, size_t bytes) {
    if (root->domain.limit == UINT64_MAX) return bytes <= UINT64_MAX - private_bytes;
    domain_lock(&root->domain);
    bool admitted = domain_fits(&root->domain, bytes);
    domain_unlock(&root->domain);
    return admitted;
}
static XrXirValueStatus root_string_allocate(XrXirDomain *domain, size_t length, XirString **output) {
    if (!xr_xir_domain_current(domain) || !domain->root || !output || *output) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (length > SIZE_MAX - sizeof(XirRootString) - ROOT_BLOCK_GRANULE) return XR_XIR_VALUE_LIMIT;
    size_t bytes = (sizeof(XirRootString) + length + ROOT_BLOCK_GRANULE) & ~(size_t)(ROOT_BLOCK_GRANULE - 1);
    XirRootDomain *ledger = (XirRootDomain *)domain;
    uint64_t private_bytes = atomic_load_explicit(&ledger->private_bytes, memory_order_relaxed);
    if (!root_admit(ledger, private_bytes, bytes)) return XR_XIR_VALUE_LIMIT;
    size_t index = root_block_class(bytes);
    XirRootString *root = NULL;
    if (index < ROOT_CACHE_CLASSES && ledger->cache[index]) {
        XirRootBlock *block = ledger->cache[index];
        ledger->cache[index] = block->next;
        --ledger->cached[index];
        root = (XirRootString *)(void *)block;
    } else if (!(root = xr_malloc(bytes))) return XR_XIR_VALUE_OOM;
    atomic_store_explicit(&ledger->private_bytes, private_bytes + bytes, memory_order_relaxed);
    XirString *string = &root->string;
    /* Every header field of a known leaf kind, as graph_object_initialize
     * would leave it, without its generic kind and domain checks. */
    xir_object_initialize(&string->object);
    string->object.domain = domain;
    string->object.arena = NULL;
    string->object.type = XR_XIR_STRING;
    string->object.kind = 0;
    string->object.release_next = NULL;
    string->object.graph_previous = string->object.graph_next = string->object.graph_work = NULL;
    string->object.graph_trial = 0;
    string->object.graph_registered = string->object.graph_live = string->object.graph_dead = false;
    string->object.root_private = true;
    root->block_bytes = bytes;
    root->escaped = false;
    string->bytes = (char *)(root + 1);
    string->length = length;
    string->runes = 0;
    string->capacity = bytes - sizeof(XirRootString);
    string->bytes[length] = 0;
    *output = string;
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_root_string_new(XrXirDomain *domain, const char *bytes,
    size_t length, XirString **output) {
    if (length && !bytes) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrUtf8ScanResult scan = xr_utf8_core_scan_strict((const uint8_t *) bytes, length);
    if (scan.error != XR_UTF8_OK) return XR_XIR_VALUE_BAD_UTF8;
    XrXirValueStatus status = root_string_allocate(domain, length, output);
    if (status != XR_XIR_VALUE_OK) return status;
    if (length) memcpy((*output)->bytes, bytes, length);
    (*output)->runes = scan.rune_count;
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_root_string_i64(XrXirDomain *domain, int64_t value, XirString **output) {
    char text[24];
    int length = xr_numeric_core_format_i64(text, sizeof(text), value);
    XR_CHECK(length > 0, "the canonical I64 formatter fits its fixed buffer");
    XrXirValueStatus status = root_string_allocate(domain, (size_t)length, output);
    if (status != XR_XIR_VALUE_OK) return status;
    memcpy((*output)->bytes, text, (size_t)length);
    (*output)->runes = (size_t)length; // Sign and decimal digits are ASCII.
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_root_string_concat(XrXirDomain *domain,
    const XirString *left, const XirString *right, XirString **output) {
    if (!left || !right) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (right->length >= SIZE_MAX - left->length) return XR_XIR_VALUE_LIMIT;
    XrXirValueStatus status = root_string_allocate(domain, left->length + right->length, output);
    if (status != XR_XIR_VALUE_OK) return status;
    if (left->length) memcpy((*output)->bytes, left->bytes, left->length);
    if (right->length) memcpy((*output)->bytes + left->length, right->bytes, right->length);
    (*output)->runes = left->runes + right->runes;
    return XR_XIR_VALUE_OK;
}
/* Escaped blocks moved to the shared accounting and pinned the domain. */
static void root_string_deallocate(XirString *string) {
    XirRootString *root = (XirRootString *)string;
    XrXirDomain *domain = string->object.domain;
    XR_CHECK(root->escaped, "only an escaped root String uses the shared accounting");
    if (!string_body_inline(string)) xr_xir_domain_deallocate(domain, string->bytes, string->capacity);
    xr_xir_domain_deallocate(domain, root, root->block_bytes);
}
/* Unescaped blocks never left the root thread's ledger or grew a body. */
static void root_string_recycle(XirString *string) {
    XirRootString *root = (XirRootString *)string;
    XirRootDomain *ledger = (XirRootDomain *)string->object.domain;
    XR_CHECK(!root->escaped && string_body_inline(string), "an unescaped root String keeps its single block");
    uint64_t private_bytes = atomic_load_explicit(&ledger->private_bytes, memory_order_relaxed);
    XR_CHECK(private_bytes >= root->block_bytes, "a root ledger covers every unescaped block");
    atomic_store_explicit(&ledger->private_bytes, private_bytes - root->block_bytes, memory_order_relaxed);
    size_t index = root_block_class(root->block_bytes);
    /* A class only grows while it is below the depth, so "not full" is "not
     * equal". Sanitized builds keep no cache: the depth is zero, and an
     * unsigned "below zero" test is a GCC type-limits error there. */
    if (index < ROOT_CACHE_CLASSES && ledger->cached[index] != (uint32_t)ROOT_CACHE_DEPTH) {
        XirRootBlock *block = (XirRootBlock *)(void *)root;
        block->next = ledger->cache[index];
        ledger->cache[index] = block;
        ++ledger->cached[index];
    } else xr_free(root);
}
/* Every additional real reference and every transfer out of the root marks
 * escape first, on the root thread, so a second holder always finds the
 * domain pinned and the block in the shared accounting. */
static bool root_string_escape(XirObject *object) {
    if (object->type != XR_XIR_STRING || !object->root_private) return true;
    XirRootString *root = (XirRootString *)object;
    if (root->escaped) return true;
    XrXirDomain *domain = object->domain;
    XirRootDomain *ledger = (XirRootDomain *)domain;
    XR_CHECK(string_body_inline(&root->string), "an unescaped root String keeps its single block");
    if (!xr_xir_domain_retain(domain)) return false;
    domain_lock(domain);
    if (domain->stats.allocations == UINT64_MAX) {
        domain_unlock(domain);
        xr_xir_domain_drop(domain);
        return false;
    }
    domain->stats.live_bytes += root->block_bytes;
    ++domain->stats.allocations;
    if (domain->stats.live_bytes > domain->stats.peak_bytes) domain->stats.peak_bytes = domain->stats.live_bytes;
    domain_unlock(domain);
    uint64_t private_bytes = atomic_load_explicit(&ledger->private_bytes, memory_order_relaxed);
    atomic_store_explicit(&ledger->private_bytes, private_bytes - root->block_bytes, memory_order_relaxed);
    root->escaped = true;
    return true;
}
XR_FUNC XrXirValueStatus xr_xir_root_string_escape(XirString *string) {
    XR_CHECK(string && string->object.root_private, "only a root String escapes its root");
    return root_string_escape(&string->object) ? XR_XIR_VALUE_OK : XR_XIR_VALUE_REFCOUNT_LIMIT;
}
XR_FUNC void xr_xir_root_domain_release(XrXirDomain *domain) {
    if (!domain) return;
    XR_CHECK(xr_xir_domain_current(domain) && domain->root, "only a root releases its root domain");
    XirRootDomain *ledger = (XirRootDomain *)domain;
    XR_CHECK(!atomic_load_explicit(&ledger->private_bytes, memory_order_relaxed),
        "a root ends only after releasing every unescaped String");
    for (size_t i = 0; i < ROOT_CACHE_CLASSES; ++i) {
        while (ledger->cache[i]) {
            XirRootBlock *block = ledger->cache[i];
            ledger->cache[i] = block->next;
            xr_free(block);
        }
        ledger->cached[i] = 0;
    }
    xr_xir_domain_drop(domain);
}
XR_FUNC void xr_xir_string_release(XirString *string) {
    XR_CHECK(string && string->object.type == XR_XIR_STRING && !string->object.kind,
        "String release consumes one live String reference");
    if (!string->object.root_private) {
        XrXirValue value = string_value(string);
        xr_xir_value_drop(&value);
        return;
    }
    /* Unpublished and unclosed: only the reference count is shared. */
    XR_CHECK(!string->object.graph_registered, "a root String never enters the residual graph");
    if (!((XirRootString *)string)->escaped) {
        XR_CHECK(xir_object_references(&string->object) == 1,
            "an unescaped root String has only its root's reference");
        root_string_recycle(string);
        return;
    }
    if (!xir_object_release(&string->object)) return;
    XrXirDomain *domain = string->object.domain;
    root_string_deallocate(string);
    xr_xir_domain_drop(domain);
}
