/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * math.h - Math standard library
 *
 * KEY CONCEPT:
 *   Comprehensive math library: basic (abs, floor, ceil, round, trunc),
 *   trigonometric (sin, cos, tan, asin, acos, atan, atan2),
 *   hyperbolic (sinh, cosh, tanh), logarithmic (log, log2, log10, log1p),
 *   exponential (exp, expm1, pow), comparison (min, max, clamp),
 *   utility (hypot, cbrt, fmod, lerp, degToRad, radToDeg, sign),
 *   random (random, randomInt), and constants (PI, E, TAU, etc.).
 *   All functions accept both int and float, preserving int type where possible.
 */

#ifndef XR_STDLIB_MATH_H
#define XR_STDLIB_MATH_H

#include "../../src/module/xmodule.h"
#include "../../src/vm/xvm.h"

XrModule *xr_load_module_math(XrayIsolate *isolate);

#endif
