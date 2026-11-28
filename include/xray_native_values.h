/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_native_values.h - Fixed values under the active execution boundary
 *
 * KEY CONCEPT:
 *   Generated code borrows the execution's stable root. Nonlocal failure never
 *   depends on owners in an abandoned C activation.
 */
#ifndef XRAY_NATIVE_VALUES_H
#define XRAY_NATIVE_VALUES_H
#include "xray_native_managed_root.h"
#include "xray_native_string_value.h"
#include "xray_native_function.h"
#include "xray_native_value_image.h"
#include "xray_native_map.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Requires an active synchronous execution and a source id from its source map.
 * The borrowed root lives until that execution exits. Initialization is lazy;
 * scalar and POD-only code does not call this function or allocate a root.
 * The execution owns all frames before entering fallible generated code. */
XRAY_API XrtManagedRoot *xrt_native_managed_root(uint32_t source_id);
/* Borrow the root of the active, explicit execution. Failure preserves the
 * null receiver and returns the original preparation status to its caller. */
XRAY_API XrtManagedStatus xrt_native_managed_root_checked(XrtManagedRoot **empty);
/* Call only after the failing kernel operation has returned and released its
 * exclusive guard. Status must be a non-OK managed status, never a diagnostic
 * enum cast. Recovery aborts pending allocations before committed owners.
 * Allocator, lease and payload-cleanup callbacks must return normally instead. */
#ifdef __cplusplus
[[noreturn]] XRAY_API void xrt_native_managed_failure(XrtManagedStatus, uint32_t source_id);
#else
_Noreturn XRAY_API void xrt_native_managed_failure(XrtManagedStatus, uint32_t source_id);
#endif
/* Generated callers supply an exact type id from their own receiving image.
 * The constructor row is complete and borrowed until the call returns. A
 * field read is a synchronous borrow under the caller's live class owner. */
XRAY_API void xrt_native_class_create(XrtManagedFrame *, size_t slot,
    const XrtValueImage *, uint32_t type, const void *row, uint32_t source_id);
XRAY_API const void *xrt_native_class_field(const XrtManagedHeader *, const XrtValueImage *,
    uint32_t type, uint32_t field, uint32_t source_id);
/* Tests one live class handle against the nominal identity named by `type` and
 * its Class ancestors. Identity is the header's immutable per-class Ops, not a
 * compiler type id or a stored tag byte. */
XRAY_API bool xrt_native_class_is(const XrtManagedHeader *, const XrtValueImage *,
    uint32_t type, uint32_t source_id);
typedef struct XrtNativeClassField {
    XrtManagedHeader *instance;
    const XrtValueImage *image;
    uint32_t type, field;
} XrtNativeClassField;
XRAY_API void xrt_native_class_scalar_field_write(const XrtNativeClassField *,
    const void *bytes, size_t size, uint32_t source_id);
/* Copies an independently owned field snapshot into a slot in the active
 * frame root. SIZE_MAX selects String. Otherwise nullable_offset names the
 * start of the canonical Nullable<String> containing that slot's owner. */
XRAY_API void xrt_native_class_string_field_copy(const XrtNativeClassField *,
    XrtManagedFrame *, size_t destination, size_t nullable_offset, uint32_t source_id);
/* String uses present=1 and nullable=false. Nullable<String> supplies its
 * original byte tag and matching owner. The incoming owner is borrowed. */
XRAY_API void xrt_native_class_string_field_replace(const XrtNativeClassField *,
    const XrtManagedHeader *incoming, uint8_t present, bool nullable, uint32_t source_id);
XRAY_API void xrt_native_class_array_field_copy(const XrtNativeClassField *,
    XrtManagedFrame *, size_t destination, uint32_t source_id);
XRAY_API void xrt_native_class_array_field_replace(const XrtNativeClassField *,
    const XrtManagedHeader *, uint32_t source_id);
XRAY_API void xrt_native_class_array_field_push(const XrtNativeClassField *,
    XrtManagedFrame *, size_t workspace, int64_t item, uint32_t source_id);
XRAY_API void xrt_native_class_array_field_push_string(const XrtNativeClassField *,
    XrtManagedFrame *, size_t workspace, const XrtStringBacking *item, uint32_t source_id);
/* Executes a numbered synchronous path operation. The String is borrowed for
 * this call only. Host refusals return false, while execution faults retire
 * the active execution after the OS kernel releases its temporary state. */
XRAY_API bool xrt_native_host_path_bool(uint32_t callable, const XrtStringBacking *, uint32_t source_id);
typedef struct XrtNativeFileResult {
    XrtManagedFrame *frame;
    size_t destination, temporary, offset;
} XrtNativeFileResult;
typedef struct XrtNativeFileOpen {
    XrtNativeFileResult output;
    const XrtValueImage *image;
    uint32_t type;
    const XrtStringBacking *path;
    bool append;
} XrtNativeFileOpen;
/* The verified receiver supplies two distinct owner slots and its Nullable
 * frame offset. The temporary must be truly empty. */
XRAY_API void xrt_native_host_file_open_write(uint32_t callable, const XrtNativeFileOpen *, uint32_t source_id);
typedef struct XrtNativeFileOpenRead {
    XrtNativeFileResult output;
    size_t temporary_owner_offset;
    const XrtValueImage *image;
    uint32_t type, result_type;
    const XrtManagedHeader *path;
} XrtNativeFileOpenRead;
typedef struct XrtNativeFileRead {
    XrtNativeFileResult output;
    size_t temporary_owner_offset;
    const XrtValueImage *image;
    uint32_t type, result_type;
    const XrtManagedHeader *file;
    int64_t max_bytes;
} XrtNativeFileRead;
/* Types are the exact file class and Nullable result ids in the same image.
 * The temporary owner offset is relative to the frame payload. Inputs are
 * borrowed until return. A host refusal publishes None; EOF publishes an
 * owned empty byte Array. Typed faults retire the active execution. */
XRAY_API void xrt_native_host_file_open_read(uint32_t callable, const XrtNativeFileOpenRead *, uint32_t source_id);
XRAY_API void xrt_native_host_file_read(uint32_t callable, const XrtNativeFileRead *, uint32_t source_id);
XRAY_API bool xrt_native_host_file_close(uint32_t callable, const XrtValueImage *, uint32_t type,
    const XrtManagedHeader *, uint32_t source_id);
typedef struct XrtNativeFileWrite {
    const XrtValueImage *image;
    uint32_t type;
    const XrtManagedHeader *file, *data;
    int64_t offset;
} XrtNativeFileWrite;
/* Borrows data through one synchronous host transfer. A refusal returns -1,
 * and a short transfer returns its actual byte count. */
XRAY_API int64_t xrt_native_host_file_write(uint32_t callable, const XrtNativeFileWrite *, uint32_t source_id);
typedef struct XrtNativeEnvironmentGet {
    XrtManagedFrame *frame;
    size_t destination, temporary, offset, temporary_owner_offset;
    const XrtValueImage *image;
    uint32_t result_type;
    const XrtManagedHeader *name;
} XrtNativeEnvironmentGet;
/* A lookup publishes an owned String, including an empty value, or None. */
XRAY_API void xrt_native_host_environment_get(uint32_t callable, const XrtNativeEnvironmentGet *, uint32_t source_id);
typedef struct XrtNativeHostTextResult {
    XrtManagedFrame *frame;
    size_t destination, temporary, offset, temporary_owner_offset;
    const XrtValueImage *image;
    uint32_t result_type;
    const XrtManagedHeader *operand;
} XrtNativeHostTextResult;
/* A textual host leaf publishes the owned text it produced, or None when the
 * host refuses. A missing receiving binding or a transport allocation failure
 * ends the execution instead, because neither can promise the empty optional.
 * The callable decides which primitive runs; the operand is the borrowed text
 * argument that primitive consumes. */
XRAY_API void xrt_native_host_text_result(uint32_t callable, const XrtNativeHostTextResult *, uint32_t source_id);
typedef struct XrtNativeDirectoryNext {
    XrtManagedFrame *frame;
    size_t destination, temporary, offset, temporary_owner_offset;
    const XrtValueImage *image;
    uint32_t result_type;
    int64_t token;
} XrtNativeDirectoryNext;
/* The directory token belongs to the current execution root. Only actual
 * end-of-stream publishes the empty optional. */
XRAY_API int64_t xrt_native_host_directory_open(const XrtManagedHeader *path, uint32_t source_id);
XRAY_API void xrt_native_host_directory_next(const XrtNativeDirectoryNext *, uint32_t source_id);
XRAY_API bool xrt_native_host_directory_close(int64_t token, uint32_t source_id);
XRAY_API void xrt_native_host_directory_cleanup_close(int64_t token, uint32_t source_id);
XRAY_API void xrt_native_host_directory_cleanup_poll(uint32_t source_id);
/* Generated cleanup paths transport these statuses without inspecting them.
 * Zero means success. Checked next requires a frame entered before cleanup
 * registration. Only success changes the destination owner and presence. */
typedef uint64_t XrtNativeDirectoryStatus;
XRAY_API XrtNativeDirectoryStatus xrt_native_host_directory_next_checked(const XrtNativeDirectoryNext *,
    uint32_t source_id);
XRAY_API XrtNativeDirectoryStatus xrt_native_host_directory_cleanup_close_checked(int64_t token, uint32_t source_id);
XRAY_API XrtNativeDirectoryStatus xrt_native_host_directory_cleanup_poll_checked(uint32_t source_id);
/* Close is secondary only when next was already failing. The caller has
 * retired any unreturned name before publishing a close or poll failure. */
#ifdef __cplusplus
[[noreturn]] XRAY_API void xrt_native_host_directory_cleanup_fail(XrtNativeDirectoryStatus primary,
    XrtNativeDirectoryStatus secondary);
#else
_Noreturn XRAY_API void xrt_native_host_directory_cleanup_fail(XrtNativeDirectoryStatus primary,
    XrtNativeDirectoryStatus secondary);
#endif
#ifdef __cplusplus
[[noreturn]] XRAY_API void xrt_native_managed_failure_with_directory(XrtManagedStatus, uint32_t source_id,
    XrtNativeDirectoryStatus close);
#else
_Noreturn XRAY_API void xrt_native_managed_failure_with_directory(XrtManagedStatus, uint32_t source_id,
    XrtNativeDirectoryStatus close);
#endif
/* The frame is an active stable frame in this execution. Success publishes one
 * complete String owner into its empty slot; failure ends this execution with
 * the original status and source id. Bytes are borrowed until return only. */
XRAY_API void xrt_native_string_create(XrtManagedFrame *, size_t slot,
    const char *bytes, size_t length, uint32_t source_id);
/* Format the admitted scalar with the shared integer text kernel and publish
 * one complete String in the true-empty stable destination slot. */
XRAY_API void xrt_native_string_i64_create(XrtManagedFrame *, size_t slot,
    int64_t value, uint32_t source_id);
/* Format the exact binary64 payload with the shared integer-only decimal
 * kernel. No floating-point arithmetic or host formatting is performed. */
XRAY_API void xrt_native_string_f64_create(XrtManagedFrame *, size_t slot,
    uint64_t bits, uint32_t source_id);
/* Parse the complete UTF-8 byte span of a live admitted String borrow.
 * Success writes the scalar. Invalid syntax or range leaves output unchanged. */
XRAY_API bool xrt_native_string_try_parse_i64(const XrtStringBacking *, int64_t *output);
XRAY_API bool xrt_native_string_try_parse_f64(const XrtStringBacking *, double *output);
XRAY_API bool xrt_native_string_try_parse_bool(const XrtStringBacking *, bool *output);
/* Search live String borrows from a rune ordinal and return a rune ordinal or -1. */
XRAY_API int64_t xrt_native_string_index_of(const XrtStringBacking *, const XrtStringBacking *,
    int64_t start, uint32_t source_id);
/* Lexicographic order of two live String borrows: -1, 0 or 1. */
XRAY_API int32_t xrt_native_string_order(const XrtStringBacking *, const XrtStringBacking *,
    uint32_t source_id);
/* Last occurrence of one live String borrow inside another, or -1. */
XRAY_API int64_t xrt_native_string_last_index_of(const XrtStringBacking *, const XrtStringBacking *,
    uint32_t source_id);
/* Test one live String borrow against another. The selector is the runtime test
 * ordinal: 0 contains, 1 starts with, 2 ends with. */
XRAY_API bool xrt_native_string_test(const XrtStringBacking *, const XrtStringBacking *,
    uint32_t test, uint32_t source_id);
/* Inputs are live admitted String borrows, including aliases. The destination
 * slot is empty. Publication preserves both input objects and their owners. */
XRAY_API void xrt_native_string_concat_create(XrtManagedFrame *, size_t slot,
    const XrtManagedHeader *left, const XrtManagedHeader *right, uint32_t source_id);
/* Replaces occurrences of one live String borrow inside another into a new
 * String backing; an empty search or no match copies the receiver. */
XRAY_API void xrt_native_string_replace_create(XrtManagedFrame *, size_t slot,
    const XrtStringBacking *, const XrtStringBacking *, const XrtStringBacking *, bool all,
    uint32_t source_id);
/* Copies the admitted half-open rune range into its own String backing. */
XRAY_API void xrt_native_string_slice_create(XrtManagedFrame *, size_t slot,
    const XrtManagedHeader *receiver, int64_t start, int64_t end, uint32_t source_id);
/* The generated image and all its vectors are immutable static storage. The
 * exact admitted Tuple bytes and their backing owners are live until return. */
XRAY_API void xrt_native_tuple_display_create(XrtManagedFrame *, size_t slot,
    const XrtValueImage *, uint32_t type, const void *bytes, uint32_t source_id);
/* Consume the admitted MOVE input into the result with no extra retain. All
 * rejecting work precedes publication; callbacks observe complete owner fields
 * and cannot retire the participating Frame while this transaction is active. */
XRAY_API void xrt_native_string_append_owned(XrtManagedFrame *,
    const XrtStringAppendOwned *, uint32_t source_id);
/* One admitted closed loop keeps its current String and invariant suffix in
 * this Frame. Empty slots are the positions visited by its ordinary edge and
 * append moves. Consume only available capacity. Zero leaves all state alone
 * and the caller executes its original iteration. */
XRAY_API size_t xrt_native_string_append_window(XrtManagedFrame *, size_t owner,
    size_t suffix, size_t count, const size_t *empty_slots, size_t empty_count);
/* Cold preparation only: initialize this execution's existing backing setup
 * before taking any checked String binding or suffix projection. This grants
 * no access permission and creates no Cell or managed Frame. */
XRAY_API void xrt_native_string_binding_prepare(uint32_t source_id);
/* Requires the preceding cold preparation. This receiver never performs lazy
 * setup; the actual binding is not consumed and workspace is a distinct empty
 * candidate slot. Failure leaves its transaction before nonlocal recovery. */
XRAY_API void xrt_native_string_append_place(XrtPlace, XrtManagedFrame *, size_t workspace,
    const XrtStringBacking *suffix, uint32_t source_id);

typedef struct XrtModuleState XrtModuleState;
typedef struct XrtNativeEnvironmentInput {
    const XrtValueImage *image;
    XrtModuleState *state;
    XrtManagedLease code;
    const void *entry, *captures;
    uint32_t type;
} XrtNativeEnvironmentInput;
/* Entry is the admitted immutable backend entry object for this exact Env row,
 * not a function-pointer cast. Code pins that object and the raw image through
 * this execution; an empty Code lease selects permanent immutable entry/image
 * storage. With
 * State, type belongs to its bound image and no second image is bound here.
 * The caller holds Code or State and every input owner throughout this whole
 * synchronous call, including callbacks. Captures are complete admitted values
 * borrowed until return. Success puts
 * one Env owner in the true-empty frame slot and returns a full Function borrow
 * of that owner. Only the slot owns it; copying the result does not retain.
 * Frame environments do not call this factory. Cold setup is execution-owned,
 * while every produced Env independently keeps its Plan and code alive. */
XRAY_API XrtFunction xrt_native_environment_create(XrtManagedFrame *, size_t slot,
    const XrtNativeEnvironmentInput *, uint32_t source_id);

typedef struct XrtNativeCellInput {
    const XrtValueImage *image;
    XrtModuleState *state;
    XrtManagedLease code;
    uint32_t type;
    union { int64_t i64; const XrtManagedHeader *map; } initial;
} XrtNativeCellInput;
/* Type is this image's admitted Cell<I64> or Cell<Map<String,I64>> row. The
 * caller keeps Code or State and every input owner alive throughout this call,
 * including callbacks. An empty Code lease requires permanent immutable image
 * storage. State reuses its bound plan; the output is one real
 * owner in the caller's true-empty temporary slot. */
XRAY_API void xrt_native_cell_create(XrtManagedFrame *, size_t,
    const XrtNativeCellInput *, uint32_t source_id);
/* These operations consume already admitted exact Cell kinds and borrowing or
 * exclusive-write facts. The caller holds a real Cell/Env owner through the
 * entire synchronous operation, including callbacks; RC and the Header guard
 * confer no language authority. Shared-band Cells reject writes. */
XRAY_API int64_t xrt_native_cell_i64_read(const XrtManagedHeader *, uint32_t source_id);
XRAY_API void xrt_native_cell_i64_write(XrtManagedHeader *, int64_t, uint32_t source_id);
XRAY_API void xrt_native_cell_map_read_copy(XrtManagedFrame *, size_t,
    const XrtManagedHeader *, uint32_t source_id);
XRAY_API void xrt_native_cell_map_write(XrtManagedHeader *, const XrtManagedHeader *, uint32_t source_id);
XRAY_API void xrt_native_cell_map_update(XrtManagedHeader *, XrtManagedFrame *, size_t workspace,
    const XrtMapUpdate *, bool *removed, uint32_t source_id);
/* The returned binding borrows its actual Cell guard and child field. Keep the
 * real Cell/Env owner alive for the complete descriptor lifetime. */
XRAY_API XrtExternalOwner xrt_native_cell_map_binding(XrtManagedHeader *, uint32_t source_id);

#ifdef __cplusplus
}
#endif
#endif // XRAY_NATIVE_VALUES_H
