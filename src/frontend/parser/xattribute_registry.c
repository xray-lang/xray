/*
 * xray - Lightweight typed scripting with native concurrency
 * Public attribute registry shared by language tooling.
 */

#include "xattribute_registry.h"
#include <string.h>

static const XrPublicAttributeInfo g_public_attributes[] = {
#define XR_PUBLIC_ATTRIBUTE(id, spelling, targets, arguments, category, phase, production, impact, \
                            stability)                                                             \
    {ATTR_##id, spelling, targets, arguments, category, phase, production, impact, stability},
#include "xattribute_registry.def"
#undef XR_PUBLIC_ATTRIBUTE
};

size_t xr_public_attribute_count(void) {
    return sizeof(g_public_attributes) / sizeof(g_public_attributes[0]);
}

const XrPublicAttributeInfo *xr_public_attribute_at(size_t index) {
    return index < xr_public_attribute_count() ? &g_public_attributes[index] : NULL;
}

typedef bool (*AttributeWork)(void *, uint64_t);
static bool attribute_system_work(void *context, uint64_t units) {
    (void) context; (void) units; return true;
}
static bool attribute_compile_work(void *context, uint64_t units) {
    return xr_compile_state_work(context, units) == XR_COMPILE_RESOURCE_OK;
}

static const XrPublicAttributeInfo *attribute_by_kind(AttributeKind kind, void *context, AttributeWork work) {
    if (!work(context, 0)) return NULL;
    /* Parameterized @test forms share the base registry entry. */
    if (kind == ATTR_TEST_SKIP || kind == ATTR_TEST_TIMEOUT)
        kind = ATTR_TEST;
    for (size_t i = 0; i < xr_public_attribute_count(); i++) {
        if (!work(context, 1)) return NULL;
        if (g_public_attributes[i].kind == kind)
            return &g_public_attributes[i];
    }
    return NULL;
}

static const XrPublicAttributeInfo *attribute_by_name(const char *name, size_t length, void *context, AttributeWork work) {
    if (!work(context, 0)) return NULL;
    if (!name)
        return NULL;
    for (size_t i = 0; i < xr_public_attribute_count(); i++) {
        if (!work(context, 1)) return NULL;
        const char *spelling = g_public_attributes[i].spelling;
        size_t matched = 0;
        for (;;) {
            if (!work(context, 1)) return NULL;
            char byte = spelling[matched];
            if (matched == length) {
                if (!byte) return &g_public_attributes[i];
                break;
            }
            if (!byte) break;
            if (!work(context, 1)) return NULL;
            if (byte != name[matched]) break;
            ++matched;
        }
    }
    return NULL;
}

const XrPublicAttributeInfo *xr_public_attribute_by_kind(AttributeKind kind) {
    return attribute_by_kind(kind, NULL, attribute_system_work);
}
const XrPublicAttributeInfo *xr_public_attribute_by_name(const char *name, size_t length) {
    return attribute_by_name(name, length, NULL, attribute_system_work);
}
const XrPublicAttributeInfo *xr_compile_public_attribute_by_kind(XrCompileState *state, AttributeKind kind) {
    return attribute_by_kind(kind, state, attribute_compile_work);
}
const XrPublicAttributeInfo *xr_compile_public_attribute_by_name(XrCompileState *state, const char *name, size_t length) {
    return attribute_by_name(name, length, state, attribute_compile_work);
}
