/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_stdlib_provider_keys_gen.h - Provider identity spellings
 *
 * Generated from explicit provider declarations. Do not edit.
 */

#ifndef XR_STDLIB_PROVIDER_KEYS_GEN_H
#define XR_STDLIB_PROVIDER_KEYS_GEN_H

#define XR_PROVIDER_BYTE_STORAGE_CONTRACT_KEY "xray.runtime.provider.v1/byte-storage"
#define XR_PROVIDER_CLOCK_CONTRACT_KEY "xray.runtime.provider.v1/clock"
#define XR_PROVIDER_IO_CONTRACT_KEY "xray.runtime.provider.v1/io"
#define XR_PROVIDER_PROCESS_CONTRACT_KEY "xray.runtime.provider.v1/process"

#define XR_PROVIDER_BYTE_STORAGE_ALLOCATE_OPERATION_KEY "xray.runtime.provider-operation.v1/byte-storage/allocate"
#define XR_PROVIDER_BYTE_STORAGE_ALLOCATE_ALIGNED_OPERATION_KEY "xray.runtime.provider-operation.v1/byte-storage/allocate-aligned"
#define XR_PROVIDER_BYTE_STORAGE_ALLOCATE_ZEROED_OPERATION_KEY "xray.runtime.provider-operation.v1/byte-storage/allocate-zeroed"
#define XR_PROVIDER_BYTE_STORAGE_LENGTH_OPERATION_KEY "xray.runtime.provider-operation.v1/byte-storage/length"
#define XR_PROVIDER_CLOCK_MONOTONIC_NANOS_OPERATION_KEY "xray.runtime.provider-operation.v1/clock/monotonic-nanos"
#define XR_PROVIDER_CLOCK_PROCESS_CPU_NANOS_OPERATION_KEY "xray.runtime.provider-operation.v1/clock/process-cpu-nanos"
#define XR_PROVIDER_CLOCK_REALTIME_NANOS_OPERATION_KEY "xray.runtime.provider-operation.v1/clock/realtime-nanos"
#define XR_PROVIDER_CLOCK_UTC_OFFSET_MINUTES_AT_OPERATION_KEY "xray.runtime.provider-operation.v1/clock/utc-offset-minutes-at"
#define XR_PROVIDER_IO_PIPE_CLOSE_OPERATION_KEY "xray.runtime.provider-operation.v1/io/pipe-close"
#define XR_PROVIDER_IO_PIPE_OPEN_OPERATION_KEY "xray.runtime.provider-operation.v1/io/pipe-open"
#define XR_PROVIDER_PROCESS_GETPID_OPERATION_KEY "xray.runtime.provider-operation.v1/process/getpid"
#define XR_PROVIDER_RESOURCE_MEM___BUFFERSTORAGE_ID { { 0x87, 0xe4, 0x1b, 0x1e, 0xee, 0x0d, 0xb8, 0x27, 0x88, 0xaf, 0x91, 0xbf, 0x9e, 0xd5, 0xc7, 0xf8 } }

#endif  // XR_STDLIB_PROVIDER_KEYS_GEN_H
