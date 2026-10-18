/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_hit.h - Bounded hit-count syntax without expression execution
 */
#ifndef XDAP_HIT_H
#define XDAP_HIT_H
#include "xdap_json.h"
#include "xdap_current.h"
/* Empty input means every encounter. A bare unsigned count means >=N; explicit
 * ==, =, >, >=, <, <= and % select the corresponding count predicate. Failure
 * preserves output; modulo zero and unsigned overflow are rejected. */
XR_FUNC XrXirStatus xdap_hit_parse(XrCompileResources *,const char *,size_t,XrXirVmDebugRule *);
#endif // XDAP_HIT_H
