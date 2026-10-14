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
#include "../../xir/xxir_value.h"
typedef struct XdapValueShape { uint64_t children;bool indexed; } XdapValueShape;
/* No executable function admission or mutable root is constructed. Each
 * returned child owns its value; names borrow only the parent's type arena. */
XR_FUNC XrXirValueStatus xdap_value_shape(const XrXirValue *,XrXirDomain *,XdapValueShape *);
XR_FUNC XrXirValueStatus xdap_value_child(const XrXirValue *,XrXirDomain *,uint64_t,
    XrXirValue *,const char **name,size_t *name_length);
#endif // XDAP_VALUE_H
