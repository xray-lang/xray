/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_array_hold.h - Local Arrays held between their aliases
 */
#ifndef XLIR_ARRAY_HOLD_H
#define XLIR_ARRAY_HOLD_H
#include "xlir.h"

/* Rebuild a readable module with its local Arrays of fixed scalar elements
 * held. A chain is the owner a local Array passes through: block parameters,
 * edge arguments and the result of a private Cell round trip that mutates it.
 * A chain with an element read, a length or such a mutation is held: every
 * owner it produces is held at once, reads and lengths become held operations,
 * a round trip becomes one held set or push, and every other use is an alias
 * that sees the unheld owner and holds it again unless it retires it. A chain
 * that starts at an incoming parameter borrows and stays unheld. Function,
 * block and edge ordinals and the complete registry are preserved; value IDs
 * may change. The output is verified before publication. Output must be empty;
 * failure publishes nothing and preserves input and any occupied output. */
XR_FUNC XrLirStatus xr_lir_array_hold(const XrLirModule *input, XrLirModule **output);
#endif // XLIR_ARRAY_HOLD_H
