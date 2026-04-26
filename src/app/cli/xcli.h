/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli.h - Xray CLI umbrella header
 *
 * KEY CONCEPT:
 *   Spec-driven CLI: declarative command/option specs, unified parser,
 *   auto-generated help, and typed handler dispatch.
 *   All command handlers use XrCliHandler(const XrCliInvocation *inv).
 */

#ifndef XCLI_H
#define XCLI_H

#include "../../base/xdefs.h"
#include "xcli_spec.h"
#include "xcli_parser.h"
#include "xcli_dispatch.h"
#include "xcli_help.h"
#include "xcli_diag.h"

#endif  // XCLI_H
