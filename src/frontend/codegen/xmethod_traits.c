/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmethod_traits.c - Method mutability traits implementation
 *
 * KEY CONCEPT:
 *   Provides compile-time checking for const object method calls.
 *   Mutating methods modify object state, readonly methods do not.
 */

#include "xmethod_traits.h"
#include "../../base/xchecks.h"
#include "../../runtime/value/xtype_names.h"
#include <string.h>

typedef struct {
    const char *method_name;
    XrMethodTrait trait;
} MethodTraitEntry;

// Array methods
static const MethodTraitEntry array_methods[] = {
    // Mutating
    {"push", XR_METHOD_MUTATING},
    {"pop", XR_METHOD_MUTATING},
    {"shift", XR_METHOD_MUTATING},
    {"unshift", XR_METHOD_MUTATING},
    {"splice", XR_METHOD_MUTATING},
    {"sort", XR_METHOD_MUTATING},
    {"reverse", XR_METHOD_MUTATING},
    {"set", XR_METHOD_MUTATING},
    {"clear", XR_METHOD_MUTATING},
    {"fill", XR_METHOD_MUTATING},
    // Readonly
    {"length", XR_METHOD_READONLY},
    {"get", XR_METHOD_READONLY},
    {"indexOf", XR_METHOD_READONLY},
    {"lastIndexOf", XR_METHOD_READONLY},
    {"includes", XR_METHOD_READONLY},
    {"slice", XR_METHOD_READONLY},
    {"concat", XR_METHOD_READONLY},
    {"join", XR_METHOD_READONLY},
    {"map", XR_METHOD_READONLY},
    {"filter", XR_METHOD_READONLY},
    {"reduce", XR_METHOD_READONLY},
    {"find", XR_METHOD_READONLY},
    {"findIndex", XR_METHOD_READONLY},
    {"forEach", XR_METHOD_READONLY},
    {"every", XR_METHOD_READONLY},
    {"some", XR_METHOD_READONLY},
    {"isEmpty", XR_METHOD_READONLY},
    {"clone", XR_METHOD_READONLY},
    {"toString", XR_METHOD_READONLY},
    {NULL, 0}};

// Map methods
static const MethodTraitEntry map_methods[] = {
    // Mutating
    {"set", XR_METHOD_MUTATING},
    {"delete", XR_METHOD_MUTATING},
    {"clear", XR_METHOD_MUTATING},
    {"increment", XR_METHOD_MUTATING},
    // Readonly
    {"get", XR_METHOD_READONLY},
    {"has", XR_METHOD_READONLY},
    {"keys", XR_METHOD_READONLY},
    {"values", XR_METHOD_READONLY},
    {"entries", XR_METHOD_READONLY},
    {"size", XR_METHOD_READONLY},
    {"length", XR_METHOD_READONLY},
    {"isEmpty", XR_METHOD_READONLY},
    {"clone", XR_METHOD_READONLY},
    {"toString", XR_METHOD_READONLY},
    {NULL, 0}};

// Set methods
static const MethodTraitEntry set_methods[] = {
    // Mutating
    {"add", XR_METHOD_MUTATING},
    {"delete", XR_METHOD_MUTATING},
    {"clear", XR_METHOD_MUTATING},
    // Readonly
    {"has", XR_METHOD_READONLY},
    {"values", XR_METHOD_READONLY},
    {"entries", XR_METHOD_READONLY},
    {"size", XR_METHOD_READONLY},
    {"length", XR_METHOD_READONLY},
    {"isEmpty", XR_METHOD_READONLY},
    {"clone", XR_METHOD_READONLY},
    {"toString", XR_METHOD_READONLY},
    {NULL, 0}};

// StringBuilder methods
static const MethodTraitEntry stringbuilder_methods[] = {
    // Mutating
    {"append", XR_METHOD_MUTATING},
    {"clear", XR_METHOD_MUTATING},
    // Readonly
    {"toString", XR_METHOD_READONLY},
    {"length", XR_METHOD_READONLY},
    {NULL, 0}};

// Channel methods
static const MethodTraitEntry channel_methods[] = {
    // Mutating
    {"send", XR_METHOD_MUTATING},
    {"trySend", XR_METHOD_MUTATING},
    {"close", XR_METHOD_MUTATING},
    // Readonly
    {"recv", XR_METHOD_READONLY},
    {"tryRecv", XR_METHOD_READONLY},
    {"isClosed", XR_METHOD_READONLY},
    {"length", XR_METHOD_READONLY},
    {"capacity", XR_METHOD_READONLY},
    {NULL, 0}};

static XrMethodTrait lookup_in_table(const MethodTraitEntry *table, const char *method_name) {
    for (int i = 0; table[i].method_name != NULL; i++) {
        if (strcmp(table[i].method_name, method_name) == 0) {
            return table[i].trait;
        }
    }
    // Not found, default to readonly (conservative)
    return XR_METHOD_READONLY;
}

XrMethodTrait xr_method_get_trait(const char *type_name, const char *method_name) {
    if (!type_name || !method_name) {
        return XR_METHOD_READONLY;
    }

    if (strcmp(type_name, TYPE_NAME_ARRAY) == 0) {
        return lookup_in_table(array_methods, method_name);
    }
    if (strcmp(type_name, TYPE_NAME_MAP) == 0) {
        return lookup_in_table(map_methods, method_name);
    }
    if (strcmp(type_name, TYPE_NAME_SET) == 0) {
        return lookup_in_table(set_methods, method_name);
    }
    if (strcmp(type_name, TYPE_NAME_BYTES) == 0) {
        return lookup_in_table(array_methods, method_name);
    }
    if (strcmp(type_name, TYPE_NAME_STRINGBUILDER) == 0) {
        return lookup_in_table(stringbuilder_methods, method_name);
    }
    if (strcmp(type_name, TYPE_NAME_CHANNEL) == 0) {
        return lookup_in_table(channel_methods, method_name);
    }

    // String methods are all readonly (immutable)
    if (strcmp(type_name, TYPE_NAME_STRING) == 0) {
        return XR_METHOD_READONLY;
    }

    // Unknown type, default to readonly
    return XR_METHOD_READONLY;
}

bool xr_method_is_mutating(const char *type_name, const char *method_name) {
    return xr_method_get_trait(type_name, method_name) == XR_METHOD_MUTATING;
}
