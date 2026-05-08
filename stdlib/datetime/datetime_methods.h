/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * datetime_methods.h - DateTime instance method dispatch table.
 *
 * KEY POINTS:
 *   - Owning the DateTime methods inside stdlib/datetime/ keeps them
 *     colocated with the implementation so src/vm never needs to
 *     reverse-include stdlib/datetime/*.
 *   - Dispatch is via native_type_classes[XR_TDATETIME], registered
 *     during isolate init by xr_datetime_register_native_type().
 *   - The method bodies are `static` inside datetime_methods.c.
 */

#ifndef XRAY_DATETIME_METHODS_H
#define XRAY_DATETIME_METHODS_H

/* DateTime method dispatch is handled via native_type_classes.
 * Method bodies are static inside datetime_methods.c. */

#endif /* XRAY_DATETIME_METHODS_H */
