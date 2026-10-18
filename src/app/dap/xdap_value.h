/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_value.h - Data-only typed child observations
 */
#ifndef XDAP_VALUE_H
#define XDAP_VALUE_H
#include "../../xir/xxir_vm_debug.h"
#include "../../xir/xxir_value_place.h"
typedef struct XdapValueShape { uint64_t children;bool indexed; } XdapValueShape;
/* A selector describes the observed typed child, including its real nominal
 * field ordinal after visibility filtering. It grants no write authority. */
typedef struct XdapValueChildInfo {
    const char *name;
    size_t name_length;
    XrXirValuePathStep selector;
    bool selected;
} XdapValueChildInfo;
typedef struct XdapValueView {
    const XrXirValue *value;
    XrXirDomain *domain;
    const XrXirVmDebugSnapshot *snapshot;
    uint32_t frame;
} XdapValueView;
enum { XDAP_TYPE_TEXT_BYTES = 1024 };
typedef struct XdapTypeText { char bytes[XDAP_TYPE_TEXT_BYTES];size_t length; } XdapTypeText;
/* No executable function admission or mutable root is constructed. Each
 * returned child owns its value; names borrow only the parent's type arena. */
XR_FUNC XrXirValueStatus xdap_value_shape(const XdapValueView *,XdapValueShape *);
XR_FUNC XrXirValueStatus xdap_value_child(const XdapValueView *,uint64_t,
    XrXirValue *,XdapValueChildInfo *);
/* Named nominal fields require the authentic frame member permission.
 * Missing and hidden members publish no value or write selector. */
XR_FUNC XrXirValueStatus xdap_value_member(const XdapValueView *,const char *,size_t,XrXirValue *);
/* Static display from retained type metadata; failure preserves output. Hidden
 * nominal names are redacted with the authentic frame's TYPE permission. */
XR_FUNC XrXirValueStatus xdap_value_type_text(const XdapValueView *,XdapTypeText *);
#endif // XDAP_VALUE_H
