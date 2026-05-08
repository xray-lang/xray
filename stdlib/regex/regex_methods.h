/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * regex_methods.h - Regex instance method dispatch table.
 *
 * KEY POINTS:
 *   - Owning the Regex methods inside stdlib/regex/ keeps them
 *     colocated with the engine so src/vm never needs to
 *     reverse-include stdlib/regex/*.
 *   - The method bodies are static inside regex_methods.c.
 *   - Dispatch is via native_type_classes[XR_TREGEX], registered
 *     during isolate init by xr_regex_register_native_type().
 */

#ifndef XRAY_REGEX_METHODS_H
#define XRAY_REGEX_METHODS_H

/* Regex method dispatch is handled via native_type_classes.
 * Method bodies are static inside regex_methods.c. */

#endif  // XRAY_REGEX_METHODS_H
