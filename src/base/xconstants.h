/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xconstants.h - Centralized global constants
 *
 * KEY CONCEPT:
 *   Single source of truth for all numeric limits and configuration
 *   constants. Avoids scattered #define across modules.
 */

#ifndef XCONSTANTS_H
#define XCONSTANTS_H

/* ========== VM Constants ========== */

// Stack size: 32K XrValues (256KB), supports 10000 levels of recursion
#define XR_STACK_MAX 32768

// Frame count: supports deep recursion (10000 levels)
#define XR_FRAMES_MAX 10000

// Max builtin globals (Reflect, Array, Map, etc.)
#define XR_GLOBALS_MAX 256

// Exception handler stack depth
#define XR_EXCEPTION_HANDLERS_MAX 64

// Constructor call stack depth (prevents circular construction)
#define XR_CTOR_CALL_STACK_MAX 32

// String/buffer size limits
#define XR_MAX_PROPERTY_NAME_LEN 256
#define XR_MAX_METHOD_NAME_LEN   256
#define XR_TOSTRING_BUFFER_SIZE  512

// Defer limits
#define XR_DEFER_ENTRIES_MAX 64
#define XR_DEFER_ARGS_MAX    16

/* ========== Coroutine Constants ========== */

// Per-coroutine stack size
#define XR_CORO_STACK_MAX 4096

// Per-coroutine frame count
#define XR_CORO_FRAMES_MAX 256

// Reduction quantum per scheduling round
#define XR_CORO_REDUCTIONS 4000

// LOW priority runs every N schedule rounds
#define XR_RESCHEDULE_LOW 8

// Max coroutines system-wide
#define XR_MAX_COROUTINES 10000000

/* ========== Scheduler Constants ========== */

// Max worker threads (P count)
#define XR_MAX_WORKERS 32

// Per-worker local run queue capacity
#define XR_LOCAL_QUEUE_SIZE 256

// Time slice in microseconds
#define XR_TIME_SLICE_US 1000

/* Time-aware work stealing: only steal tasks older than this (ms).
 * Fresh tasks stay local for cache locality.
 * Kotlin uses 100us; we use 2ms with ms-resolution clock. */
#define XR_STEAL_TIME_RESOLUTION_MS 2

// Number of run queues (priority levels mapped to queues)
#define XR_RUNQ_COUNT 2

/* ========== Pool Constants ========== */

// Blocked coroutine hash buckets per worker
#define XR_BLOCKED_BUCKET_SIZE 32

// Pending coroutine pool limit
#define XR_CORO_POOL_MAX_PENDING 256

/* ========== String Constants ========== */

// Short string optimization threshold (stack buffer for concat)
#define XR_SHORT_STRING_THRESHOLD 64

// DateTime default format pattern
#define XR_DATETIME_DEFAULT_FORMAT "YYYY-MM-DD HH:mm:ss"

// toFixed() maximum decimal places
#define XR_TOFIXED_MAX_DECIMALS 20

/* ========== VM Stack Growth ========== */

// Default extra slots when growing stack
#define XR_STACK_GROW_DEFAULT  128

// Minimum padding slots when growing stack
#define XR_STACK_GROW_PADDING   64

/* ========== Bit Width ========== */

// int64 bit width (for shift range checks)
#define XR_INT64_BITS 64

#endif // XCONSTANTS_H
