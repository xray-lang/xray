/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xglobal_indices.h - Predefined global variable indices (shared by compiler and VM)
 *
 * KEY CONCEPT:
 *   Reserved indices for builtin global objects (Array, etc.).
 *   User-defined globals start at XR_USER_GLOBALS_START.
 */

#ifndef XGLOBAL_INDICES_H
#define XGLOBAL_INDICES_H

#define XR_GLOBAL_VAR_RESERVED0 0
#define XR_GLOBAL_VAR_ARRAY 1
#define XR_GLOBAL_VAR_SET 2
#define XR_GLOBAL_VAR_MAP 3
#define XR_GLOBAL_VAR_STRING 4
#define XR_GLOBAL_VAR_PROCESS 5
#define XR_GLOBAL_VAR_FILE 6
#define XR_GLOBAL_VAR_DIR 7
#define XR_GLOBAL_VAR_ARRAY_BYTE_NEW 8
#define XR_GLOBAL_VAR_ARRAYBUFFER 9
#define XR_GLOBAL_VAR_INT8ARRAY 10
#define XR_GLOBAL_VAR_UINT8ARRAY 11
#define XR_GLOBAL_VAR_INT16ARRAY 12
#define XR_GLOBAL_VAR_UINT16ARRAY 13
#define XR_GLOBAL_VAR_INT32ARRAY 14
#define XR_GLOBAL_VAR_UINT32ARRAY 15
#define XR_GLOBAL_VAR_FLOAT32ARRAY 16
#define XR_GLOBAL_VAR_FLOAT64ARRAY 17
#define XR_GLOBAL_VAR_JSON 18
#define XR_GLOBAL_VAR_PANIC_INFO 19
#define XR_GLOBAL_VAR_RANGE 20
#define XR_GLOBAL_VAR_ATOMIC 21
#define XR_GLOBAL_VAR_ORDERING 22
#define XR_GLOBAL_VAR_ENDIAN 23
#define XR_GLOBAL_VAR_RECV 24
#define XR_GLOBAL_VAR_SEND_RESULT 25
#define XR_GLOBAL_VAR_TASK_RESULT 26
#define XR_GLOBAL_VAR_TASK_STATUS 27
#define XR_GLOBAL_VAR_WORKQUEUE 28
#define XR_GLOBAL_VAR_RESULTGROUP 29
#define XR_GLOBAL_VAR_RESERVED30 30
#define XR_GLOBAL_VAR_COUNTDOWNLATCH 31
#define XR_GLOBAL_VAR_SEMAPHORE 32
#define XR_GLOBAL_VAR_EVENTCOUNT 33
#define XR_GLOBAL_VAR_UTF8_ERROR 34
#define XR_GLOBAL_VAR_STRING_SLICE_ERROR 35
#define XR_GLOBAL_VAR_COMPRESSION_ERROR 36
#define XR_GLOBAL_VAR_CRYPTO_ERROR 37

#define XR_USER_GLOBALS_START 38

#endif  // XGLOBAL_INDICES_H
