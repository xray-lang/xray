/*
 * xray - Lightweight typed scripting with native concurrency
 * Public attribute registry shared by language tooling.
 */

#include "xattribute_registry.h"
#include <string.h>

static const XrPublicAttributeInfo g_public_attributes[] = {
#define XR_PUBLIC_ATTRIBUTE(id, spelling, targets, arguments, phase, production, impact,           \
                            stability)                                                             \
    {ATTR_##id, spelling, targets, arguments, phase, production, impact, stability},
#include "xattribute_registry.def"
#undef XR_PUBLIC_ATTRIBUTE
};

size_t xr_public_attribute_count(void) {
    return sizeof(g_public_attributes) / sizeof(g_public_attributes[0]);
}

const XrPublicAttributeInfo *xr_public_attribute_at(size_t index) {
    return index < xr_public_attribute_count() ? &g_public_attributes[index] : NULL;
}

const XrPublicAttributeInfo *xr_public_attribute_by_kind(AttributeKind kind) {
    /* Parameterized @test forms share the base registry entry. */
    if (kind == ATTR_TEST_SKIP || kind == ATTR_TEST_TIMEOUT)
        kind = ATTR_TEST;
    for (size_t i = 0; i < xr_public_attribute_count(); i++) {
        if (g_public_attributes[i].kind == kind)
            return &g_public_attributes[i];
    }
    return NULL;
}

const XrPublicAttributeInfo *xr_public_attribute_by_name(const char *name, size_t length) {
    if (!name)
        return NULL;
    for (size_t i = 0; i < xr_public_attribute_count(); i++) {
        const char *spelling = g_public_attributes[i].spelling;
        if (strlen(spelling) == length && memcmp(spelling, name, length) == 0)
            return &g_public_attributes[i];
    }
    return NULL;
}
