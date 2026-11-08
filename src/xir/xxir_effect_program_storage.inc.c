/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_program_storage.inc.c - Physical bounds for owned Program metadata
 *
 * KEY CONCEPT:
 *   Readable canonical storage is a precondition, never semantic authority.
 */

#define PROGRAM_STORAGE(pointer, count, type) do { \
    XrXirStatus range_status = effect_program_range(work, pointer, count, sizeof(type), _Alignof(type)); \
    if (range_status != XR_XIR_OK) return range_status; \
} while (0)

/* The complete owner chain is bounded before the existing shape reader can
 * follow it. These checks cover metadata and signatures, not the Final graph. */

/* Current body correspondence reads these payloads after metadata preparation.
 * Capacity is checked against the genuine payload without reading its tail. */

#undef PROGRAM_STORAGE
