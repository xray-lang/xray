/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xglobal_summary.h - Whole-program summary/evidence data model
 */

#ifndef XGLOBAL_SUMMARY_H
#define XGLOBAL_SUMMARY_H

#include "../base/xdefs.h"
#include "../base/xentry_plan.h"
#include "../base/xstorage.h"
#include <stddef.h>
#include <stdint.h>

typedef uint32_t XgModuleId;
typedef uint32_t XgDeclId;
typedef uint32_t XgFuncId;
typedef uint32_t XgTypeId;
typedef uint32_t XgClassId;
typedef uint32_t XgInterfaceId;
typedef uint32_t XgMethodId;
typedef uint32_t XgInterfaceMethodId;
typedef uint32_t XgInterfaceObjectUseId;
typedef uint32_t XgFieldId;
typedef uint32_t XgCallsiteId;
typedef uint32_t XgParamStorageId;
typedef uint32_t XgLinkId;
typedef uint32_t XgGenericInstId;
typedef uint32_t XgGenericBodyUseId;
typedef uint32_t XgGenericStorageId;
typedef uint32_t XgGenericCodeSizeId;
typedef uint32_t XgSequenceAccessId;
typedef uint32_t XgCapacityOpId;
typedef uint32_t XgBulkOpId;
typedef uint32_t XgEncodingOpId;
typedef uint32_t XgDeriveId;
typedef uint32_t XgDerivedFieldId;
typedef uint32_t XgDerivedMethodId;
typedef uint32_t XgJsonShapeId;
typedef uint32_t XgJsonFieldId;
typedef uint32_t XgJsonAccessId;
typedef uint32_t XgJsonCodecId;
typedef uint32_t XgRecordShapeId;
typedef uint32_t XgRecordFieldId;
typedef uint32_t XgRecordAccessId;
typedef uint32_t XgRecordMergeId;
typedef uint32_t XgOptionsId;
typedef uint32_t XgMapShapeId;
typedef uint32_t XgMapEntryId;
typedef uint32_t XgKeyAccessId;
typedef uint32_t XgHashEqId;

#define XG_LINK_DEP_NAME_MAX 512

enum {
    XG_NO_ID = 0,
    XG_GLOBAL_EVIDENCE_SCHEMA_VERSION = 22,
};

typedef enum XgBuildProfile {
    XG_BUILD_CHECK = 0,
    XG_BUILD_DEV,
    XG_BUILD_NATIVE_RELEASE,
    XG_BUILD_FREESTANDING,
    XG_BUILD_DEBUG_TOOLING,
} XgBuildProfile;

typedef enum XgDeclKind {
    XG_DECL_FUNC = 1,
    XG_DECL_CLASS,
    XG_DECL_STRUCT,
    XG_DECL_UNION,
    XG_DECL_ENUM,
    XG_DECL_INTERFACE,
    XG_DECL_GLOBAL,
} XgDeclKind;

enum {
    XG_DECL_PUBLIC = 1u << 0,
    XG_DECL_EXPORT = 1u << 1,
    XG_DECL_NATIVE = 1u << 2,
    XG_DECL_EXTERN = 1u << 3,
    XG_DECL_C_EXPORT = 1u << 4,
    XG_DECL_DERIVE = 1u << 5,
    XG_DECL_FINAL = 1u << 6,
    XG_DECL_NAKED = 1u << 7,
    XG_DECL_INTERRUPT = 1u << 8,
};

enum {
    XG_CLASS_EXPLICIT_FINAL = 1u << 0,
    XG_CLASS_HAS_SUBCLASS = 1u << 1,
    XG_CLASS_INFERRED_FINAL = 1u << 2,
    XG_CLASS_NATIVE = 1u << 3,
    XG_CLASS_RUNTIME_ONLY = 1u << 4,
    XG_CLASS_GENERIC_SKELETON = 1u << 5,
    XG_CLASS_MONOMORPHIZED = 1u << 6,
};

enum {
    XG_CLASS_FIELD_STATIC = 1u << 0,
    XG_CLASS_FIELD_CONST = 1u << 1,
    XG_CLASS_FIELD_PRIVATE = 1u << 2,
    XG_CLASS_FIELD_PROTECTED = 1u << 3,
    XG_CLASS_FIELD_OWNED_REF = 1u << 4,
    XG_CLASS_FIELD_NULLABLE = 1u << 5,
};

typedef enum XgClassFieldTypeKind {
    XG_CLASS_FIELD_TYPE_I8 = 1,
    XG_CLASS_FIELD_TYPE_U8,
    XG_CLASS_FIELD_TYPE_I16,
    XG_CLASS_FIELD_TYPE_U16,
    XG_CLASS_FIELD_TYPE_I32,
    XG_CLASS_FIELD_TYPE_U32,
    XG_CLASS_FIELD_TYPE_I64,
    XG_CLASS_FIELD_TYPE_U64,
    XG_CLASS_FIELD_TYPE_ISIZE,
    XG_CLASS_FIELD_TYPE_USIZE,
    XG_CLASS_FIELD_TYPE_F32,
    XG_CLASS_FIELD_TYPE_F64,
    XG_CLASS_FIELD_TYPE_BOOL,
    XG_CLASS_FIELD_TYPE_RUNE,
    XG_CLASS_FIELD_TYPE_STRING,
    XG_CLASS_FIELD_TYPE_ARRAY,
    XG_CLASS_FIELD_TYPE_MAP,
    XG_CLASS_FIELD_TYPE_SET,
    XG_CLASS_FIELD_TYPE_CLASS,
    XG_CLASS_FIELD_TYPE_INTERFACE,
    XG_CLASS_FIELD_TYPE_ENUM,
    XG_CLASS_FIELD_TYPE_STRUCT,
    XG_CLASS_FIELD_TYPE_FIXED_UNION,
    XG_CLASS_FIELD_TYPE_FIXED_ARRAY,
    XG_CLASS_FIELD_TYPE_OPTIONAL,
    XG_CLASS_FIELD_TYPE_UNION,
    XG_CLASS_FIELD_TYPE_FUNCTION,
    XG_CLASS_FIELD_TYPE_TUPLE,
    XG_CLASS_FIELD_TYPE_OBJECT,
    XG_CLASS_FIELD_TYPE_TYPE_PARAM,
    XG_CLASS_FIELD_TYPE_UNIT,
    XG_CLASS_FIELD_TYPE_NULL,
    XG_CLASS_FIELD_TYPE_DYNAMIC,
} XgClassFieldTypeKind;

enum {
    XG_METHOD_STATIC = 1u << 0,
    XG_METHOD_CONSTRUCTOR = 1u << 1,
    XG_METHOD_DIRECT_ONLY = 1u << 2,
    XG_METHOD_OVERRIDDEN = 1u << 3,
    XG_METHOD_NATIVE = 1u << 4,
};

typedef enum XgCallsiteKind {
    XG_CALL_DIRECT_FUNC = 1,
    XG_CALL_METHOD,
    XG_CALL_INTERFACE,
    XG_CALL_CLOSURE,
    XG_CALL_NATIVE,
    XG_CALL_EXTERN,
} XgCallsiteKind;

typedef enum XgBodyKind {
    XG_BODY_MODULE_INIT = 1,
    XG_BODY_FUNCTION,
    XG_BODY_METHOD,
} XgBodyKind;

typedef enum XgLinkDependencyKind {
    XG_LINK_DEP_EXTERN_DYLIB = 1,
    XG_LINK_DEP_STDLIB_MODULE,
    XG_LINK_DEP_STDLIB_SYMBOL,
} XgLinkDependencyKind;

typedef enum XgGenericInstKind {
    XG_GENERIC_INST_FUNCTION = 1,
    XG_GENERIC_INST_METHOD,
    XG_GENERIC_INST_CLASS,
    XG_GENERIC_INST_CONTAINER,
} XgGenericInstKind;

typedef enum XgGenericStorageKind {
    XG_GENERIC_STORAGE_ARRAY = 1,
    XG_GENERIC_STORAGE_MAP,
    XG_GENERIC_STORAGE_SET,
    XG_GENERIC_STORAGE_CLASS,
    XG_GENERIC_STORAGE_STRUCT,
} XgGenericStorageKind;

typedef enum XgSequenceKind {
    XG_SEQ_ARRAY = 1,
    XG_SEQ_BYTES,
    XG_SEQ_STRING,
    XG_SEQ_SPAN,
    XG_SEQ_BYTE_SLICE,
    XG_SEQ_STRING_BUILDER,
} XgSequenceKind;

typedef enum XgSequenceAccessKind {
    XG_SEQ_ACCESS_INDEX_GET = 1,
    XG_SEQ_ACCESS_INDEX_SET,
    XG_SEQ_ACCESS_SLICE,
    XG_SEQ_ACCESS_ITER,
    XG_SEQ_ACCESS_LENGTH,
} XgSequenceAccessKind;

typedef enum XgCapacityOpKind {
    XG_CAPACITY_PUSH = 1,
    XG_CAPACITY_APPEND,
    XG_CAPACITY_EXTEND,
    XG_CAPACITY_RESERVE,
    XG_CAPACITY_CONCAT,
    XG_CAPACITY_TO_STRING,
    XG_CAPACITY_CLEAR,
} XgCapacityOpKind;

typedef enum XgBulkOpKind {
    XG_BULK_COPY = 1,
    XG_BULK_FILL,
    XG_BULK_COMPARE,
    XG_BULK_REPEAT,
    XG_BULK_COPY_WITHIN,
} XgBulkOpKind;

typedef enum XgEncodingOpKind {
    XG_ENCODING_STRING_TO_BYTES = 1,
    XG_ENCODING_BYTES_TO_STRING,
    XG_ENCODING_UTF8_VALIDATE,
    XG_ENCODING_UTF8_COUNT,
    XG_ENCODING_UTF16_ENCODE,
    XG_ENCODING_UTF16_DECODE,
} XgEncodingOpKind;

typedef enum XgDeriveKind {
    XG_DERIVE_JSON = 1,
    XG_DERIVE_INSPECT,
    XG_DERIVE_EQ,
    XG_DERIVE_HASH,
    XG_DERIVE_CLONE,
} XgDeriveKind;

typedef enum XgDerivedMethodKind {
    XG_DERIVED_METHOD_JSON_ENCODE = 1,
    XG_DERIVED_METHOD_INSPECT_FORMAT,
    XG_DERIVED_METHOD_EQ,
    XG_DERIVED_METHOD_HASH,
    XG_DERIVED_METHOD_CLONE,
} XgDerivedMethodKind;

typedef enum XgJsonShapeKind {
    XG_JSON_SHAPE_OPEN = 1,
    XG_JSON_SHAPE_SHAPED,
    XG_JSON_SHAPE_RECORD_BRIDGE,
} XgJsonShapeKind;

typedef enum XgJsonAccessKind {
    XG_JSON_ACCESS_FIELD_GET = 1,
    XG_JSON_ACCESS_FIELD_SET,
    XG_JSON_ACCESS_INDEX_GET,
    XG_JSON_ACCESS_INDEX_SET,
    XG_JSON_ACCESS_GET_DEFAULT,
} XgJsonAccessKind;

typedef enum XgJsonCodecKind {
    XG_JSON_CODEC_PARSE = 1,
    XG_JSON_CODEC_DECODE,
    XG_JSON_CODEC_ENCODE,
    XG_JSON_CODEC_STRINGIFY,
} XgJsonCodecKind;

typedef enum XgRecordShapeKind {
    XG_RECORD_SHAPE_LITERAL = 1,
    XG_RECORD_SHAPE_OPTIONS,
    XG_RECORD_SHAPE_SPREAD,
    XG_RECORD_SHAPE_STATIC,
    XG_RECORD_SHAPE_PATCH,
} XgRecordShapeKind;

typedef enum XgRecordAccessKind {
    XG_RECORD_ACCESS_FIELD_GET = 1,
    XG_RECORD_ACCESS_FIELD_SET,
    XG_RECORD_ACCESS_DESTRUCTURE,
} XgRecordAccessKind;

typedef enum XgOptionsAction {
    XG_OPTIONS_DEFAULT_ELIDED = 1,
    XG_OPTIONS_DEFAULT_FILL_TABLE,
    XG_OPTIONS_REQUIRED_CHECK,
    XG_OPTIONS_CALLSITE_SPECIALIZED,
    XG_OPTIONS_REJECT,
} XgOptionsAction;

typedef enum XgMapContainerKind {
    XG_MAP_CONTAINER_MAP = 1,
    XG_MAP_CONTAINER_SET,
} XgMapContainerKind;

typedef enum XgMapShapeSource {
    XG_MAP_SHAPE_SRC_LITERAL = 1,
    XG_MAP_SHAPE_SRC_CONSTRUCTOR,
    XG_MAP_SHAPE_SRC_FROM_ARRAY,
    XG_MAP_SHAPE_SRC_STATIC,
} XgMapShapeSource;

typedef enum XgKeyAccessOp {
    XG_KEY_ACCESS_GET = 1,
    XG_KEY_ACCESS_INDEX_GET,
    XG_KEY_ACCESS_SET,
    XG_KEY_ACCESS_HAS,
    XG_KEY_ACCESS_DELETE,
    XG_KEY_ACCESS_ADD,
    XG_KEY_ACCESS_CLEAR,
} XgKeyAccessOp;

typedef enum XgHashEqKind {
    XG_HASH_EQ_BUILTIN = 1,
    XG_HASH_EQ_ENUM_ORDINAL,
    XG_HASH_EQ_DERIVE,
    XG_HASH_EQ_USER_METHOD,
    XG_HASH_EQ_MISSING,
} XgHashEqKind;

enum {
    XG_CALL_MAY_THROW = 1u << 0,
    XG_CALL_MAY_SUSPEND = 1u << 1,
    XG_CALL_USES_DEFAULT_ARGS = 1u << 2,
};

enum {
    XG_CAP_COROUTINE = XR_CAP_COROUTINE,
    XG_CAP_CHANNEL = XR_CAP_CHANNEL,
    XG_CAP_EXCEPTION = XR_CAP_EXCEPTION,
    XG_CAP_NATIVE = XR_CAP_NATIVE,
    XG_CAP_EXTERN = XR_CAP_EXTERN,
    XG_CAP_OBJECTS = XR_CAP_OBJECTS,
    XG_CAP_DEEP_COPY = XR_CAP_DEEP_COPY,
    XG_CAP_INSTANCEOF = XR_CAP_INSTANCEOF,
    XG_CAP_SYS_THREAD = XR_CAP_SYS_THREAD,
    XG_CAP_SCOPE = XR_CAP_SCOPE,
    XG_CAP_TIMER = XR_CAP_TIMER,
    XG_CAP_NETPOLL = XR_CAP_NETPOLL,
    XG_CAP_TASK = XR_CAP_TASK,
    XG_CAP_ATOMIC = XR_CAP_ATOMIC,
    XG_CAP_WORK_QUEUE = XR_CAP_WORK_QUEUE,
    XG_CAP_RESULT_GROUP = XR_CAP_RESULT_GROUP,
    XG_CAP_COUNTDOWN_LATCH = XR_CAP_COUNTDOWN_LATCH,
    XG_CAP_SEMAPHORE = XR_CAP_SEMAPHORE,
    XG_CAP_EVENT_COUNT = XR_CAP_EVENT_COUNT,
    XG_CAP_GENERATOR = XR_CAP_GENERATOR,
    XG_CAP_STACKTRACE = XR_CAP_STACKTRACE,
    XG_CAP_PARALLEL = XR_CAP_PARALLEL,
};

enum {
    XG_METADATA_TYPENAME = 1u << 0,
    XG_METADATA_DERIVE = 1u << 1,
    XG_METADATA_DEBUG = 1u << 2,
    XG_METADATA_TOOLING = 1u << 3,
};

enum {
    XG_STATIC_DATA_COMPTIME_VALUE = 1u << 0,
    XG_STATIC_DATA_FIXED_LAYOUT = 1u << 1,
    XG_STATIC_DATA_RODATA = 1u << 2,
    XG_STATIC_DATA_FREESTANDING_SAFE = 1u << 3,
    XG_STATIC_DATA_RUNTIME_INIT = 1u << 4,
};

enum {
    XG_BODY_MAY_THROW = XR_EFFECT_MAY_THROW,
    XG_BODY_MAY_SUSPEND = XR_EFFECT_MAY_SUSPEND,
    XG_BODY_MAY_ALLOC = XR_EFFECT_MAY_ALLOC,
    XG_BODY_MAY_MUTATE = XR_EFFECT_MAY_MUTATE,
    XG_BODY_MAY_CALL_NATIVE = XR_EFFECT_MAY_CALL_NATIVE,
    XG_BODY_MAY_READ_MEM = XR_EFFECT_MAY_READ_MEM,
    XG_BODY_MAY_CALL = XR_EFFECT_MAY_CALL,
    XG_BODY_MAY_SPAWN = XR_EFFECT_MAY_SPAWN,
    XG_BODY_ACCESSES_MUTABLE_MODULE = XR_EFFECT_ACCESSES_MUTABLE_MODULE,
    XG_BODY_OBSERVES_TASK_ID = XR_EFFECT_OBSERVES_TASK_ID,
};

enum {
    XG_BODY_ESCAPE_RETURN = 1u << 0,
    XG_BODY_ESCAPE_FIELD = 1u << 1,
    XG_BODY_ESCAPE_CONTAINER = 1u << 2,
    XG_BODY_ESCAPE_CORO = 1u << 3,
    XG_BODY_ESCAPE_NATIVE = 1u << 4,
    XG_BODY_ESCAPE_EXTERN = 1u << 5,
    XG_BODY_ESCAPE_CAPTURE = 1u << 6,
};

enum {
    XG_GENERIC_INST_CONCRETE_TYPES = 1u << 0,
    XG_GENERIC_INST_INTERFACE_CONSTRAINT = 1u << 1,
    XG_GENERIC_INST_SPECIALIZED_BODY = 1u << 2,
    XG_GENERIC_INST_SPECIALIZED_ABI = 1u << 3,
    XG_GENERIC_INST_CONCRETE_STORAGE = 1u << 4,
};

enum {
    XG_GENERIC_BODY_EXPLICIT_ROOT = 1u << 0,
    XG_GENERIC_BODY_IMPLICIT_ROOT = 1u << 1,
    XG_GENERIC_BODY_EXPORTED = 1u << 2,
    XG_GENERIC_BODY_DYNAMIC_BOUNDARY = 1u << 3,
};

enum {
    XG_GENERIC_STORAGE_TYPED_INLINE = 1u << 0,
    XG_GENERIC_STORAGE_REF_LANE = 1u << 1,
    XG_GENERIC_STORAGE_BOXED = 1u << 2,
    XG_GENERIC_STORAGE_POD = 1u << 3,
    XG_GENERIC_STORAGE_MANAGED_REF = 1u << 4,
};

enum {
    XG_GENERIC_CODESIZE_ALLOW_CLONE = 1u << 0,
    XG_GENERIC_CODESIZE_SHARE_CANONICAL_BODY = 1u << 1,
    XG_GENERIC_CODESIZE_FORCE_CLONE = 1u << 2,
    XG_GENERIC_CODESIZE_PROFILE_IGNORED = 1u << 3,
};

enum {
    XG_INTERFACE_OBJECT_USE_VALUE = 1u << 0,
    XG_INTERFACE_OBJECT_USE_ARRAY = 1u << 1,
    XG_INTERFACE_OBJECT_USE_FIELD = 1u << 2,
    XG_INTERFACE_OBJECT_USE_RETURN = 1u << 3,
    XG_INTERFACE_OBJECT_USE_CAPTURE = 1u << 4,
    XG_INTERFACE_OBJECT_USE_PARAM = 1u << 5,
};

enum {
    XG_SEQ_ACCESS_MUTATING = 1u << 0,
    XG_SEQ_ACCESS_NEGATIVE_INDEX = 1u << 1,
    XG_SEQ_ACCESS_SLICE_NORMALIZED = 1u << 2,
    XG_SEQ_ACCESS_FROM_SPAN = 1u << 3,
    XG_SEQ_ACCESS_CONST_INDEX = 1u << 4,
};

enum {
    XG_CAPACITY_MAY_GROW = 1u << 0,
    XG_CAPACITY_EXACT_COUNT = 1u << 1,
    XG_CAPACITY_LOOP_APPEND = 1u << 2,
    XG_CAPACITY_BUILDER_FINAL = 1u << 3,
};

enum {
    XG_BULK_POD = 1u << 0,
    XG_BULK_OVERLAP_POSSIBLE = 1u << 1,
    XG_BULK_READONLY_SRC = 1u << 2,
    XG_BULK_WRITE_BARRIER = 1u << 3,
};

enum {
    XG_ENCODING_KNOWN_UTF8 = 1u << 0,
    XG_ENCODING_VALIDATED_ONCE = 1u << 1,
    XG_ENCODING_SCALAR_BOUNDARY = 1u << 2,
    XG_ENCODING_STATIC_LITERAL = 1u << 3,
};

enum {
    XG_DERIVE_OPT_IN = 1u << 0,
    XG_DERIVE_REACHABLE = 1u << 1,
    XG_DERIVE_GENERATED = 1u << 2,
    XG_DERIVE_METADATA_ONLY = 1u << 3,
};

enum {
    XG_DERIVED_FIELD_PUBLIC = 1u << 0,
    XG_DERIVED_FIELD_PRIVATE = 1u << 1,
    XG_DERIVED_FIELD_PROTECTED = 1u << 2,
    XG_DERIVED_FIELD_STATIC = 1u << 3,
    XG_DERIVED_FIELD_READONLY = 1u << 4,
};

enum {
    XG_DERIVED_METHOD_INLINEABLE = 1u << 0,
    XG_DERIVED_METHOD_NO_ALLOC = 1u << 1,
    XG_DERIVED_METHOD_NO_THROW = 1u << 2,
    XG_DERIVED_METHOD_PURE = 1u << 3,
    XG_DERIVED_METHOD_DEEP_COPY = 1u << 4,
};

enum {
    XG_JSON_SHAPE_STATIC_KEYS = 1u << 0,
    XG_JSON_SHAPE_HAS_COMPUTED_KEYS = 1u << 1,
    XG_JSON_SHAPE_MUTABLE = 1u << 2,
    XG_JSON_SHAPE_RECORD_BRIDGEABLE = 1u << 3,
};

enum {
    XG_JSON_ACCESS_STATIC_KEY = 1u << 0,
    XG_JSON_ACCESS_COMPUTED_KEY = 1u << 1,
    XG_JSON_ACCESS_RECEIVER_SHAPE_PROVEN = 1u << 2,
    XG_JSON_ACCESS_MUTATING = 1u << 3,
};

enum {
    XG_JSON_CODEC_HAS_INPUT_SHAPE = 1u << 0,
    XG_JSON_CODEC_HAS_OUTPUT_SHAPE = 1u << 1,
    XG_JSON_CODEC_HAS_TARGET_TYPE = 1u << 2,
    XG_JSON_CODEC_USES_DERIVE = 1u << 3,
    XG_JSON_CODEC_STATIC_TEXT = 1u << 4,
};

enum {
    XG_JSON_FIELD_STATIC_KEY = 1u << 0,
    XG_JSON_FIELD_TYPED = 1u << 1,
    XG_JSON_FIELD_RECORD_BRIDGE = 1u << 2,
};

enum {
    XG_RECORD_SHAPE_SEALED = 1u << 0,
    XG_RECORD_SHAPE_STATIC_KEYS = 1u << 1,
    XG_RECORD_SHAPE_HAS_SPREAD = 1u << 2,
    XG_RECORD_SHAPE_HAS_OPTIONS = 1u << 3,
    XG_RECORD_SHAPE_JSON_BRIDGEABLE = 1u << 4,
};

enum {
    XG_RECORD_ACCESS_STATIC_FIELD = 1u << 0,
    XG_RECORD_ACCESS_RECEIVER_SHAPE_PROVEN = 1u << 1,
    XG_RECORD_ACCESS_MUTATING = 1u << 2,
};

enum {
    XG_RECORD_FIELD_REQUIRED = 1u << 0,
    XG_RECORD_FIELD_OPTIONAL = 1u << 1,
    XG_RECORD_FIELD_DEFAULTED = 1u << 2,
    XG_RECORD_FIELD_READONLY = 1u << 3,
    XG_RECORD_FIELD_STATIC_KEY = 1u << 4,
};

enum {
    XG_RECORD_MERGE_BASE_SHAPE_PROVEN = 1u << 0,
    XG_RECORD_MERGE_PATCH_SHAPE_PROVEN = 1u << 1,
    XG_RECORD_MERGE_RESULT_SHAPE_PROVEN = 1u << 2,
    XG_RECORD_MERGE_OVERWRITES = 1u << 3,
    XG_RECORD_MERGE_JSON_BRIDGE = 1u << 4,
};

enum {
    XG_OPTIONS_ALL_SUPPLIED = 1u << 0,
    XG_OPTIONS_NEEDS_DEFAULTS = 1u << 1,
    XG_OPTIONS_MISSING_REQUIRED = 1u << 2,
    XG_OPTIONS_CALLSITE_PROVEN = 1u << 3,
};

enum {
    XG_MAP_SHAPE_LITERAL = 1u << 0,
    XG_MAP_SHAPE_SMALL = 1u << 1,
    XG_MAP_SHAPE_DENSE_ENUM = 1u << 2,
    XG_MAP_SHAPE_DENSE_INT = 1u << 3,
    XG_MAP_SHAPE_READONLY = 1u << 4,
    XG_MAP_SHAPE_STATIC = 1u << 5,
    XG_MAP_SHAPE_BOOL_DIRECT = 1u << 6,
};

enum {
    XG_MAP_ENTRY_CONST_KEY = 1u << 0,
    XG_MAP_ENTRY_CONST_VALUE = 1u << 1,
    XG_MAP_ENTRY_DUPLICATE_KEY = 1u << 2,
    XG_MAP_ENTRY_INT_KEY = 1u << 3,
    XG_MAP_ENTRY_BOOL_KEY = 1u << 4,
    XG_MAP_ENTRY_ENUM_KEY = 1u << 5,
};

enum {
    XG_KEY_ACCESS_MUTATING = 1u << 0,
    XG_KEY_ACCESS_CONST_KEY = 1u << 1,
    XG_KEY_ACCESS_MISSING_PANICS = 1u << 2,
    XG_KEY_ACCESS_DEFAULT_PATH = 1u << 3,
};

enum {
    XG_HASH_EQ_NO_ALLOC = 1u << 0,
    XG_HASH_EQ_NO_THROW = 1u << 1,
    XG_HASH_EQ_PURE = 1u << 2,
    XG_HASH_EQ_FINAL = 1u << 3,
};

typedef struct XgBuildKey {
    uint64_t source_hash;
    uint64_t compiler_semver_hash;
    uint64_t profile_hash;
    uint64_t imported_summary_hash;
    XgModuleId module_id;
    uint32_t profile;
} XgBuildKey;

typedef enum XgEvidenceCachePhase {
    XG_EVIDENCE_CACHE_DECLARATIONS = 1,
    XG_EVIDENCE_CACHE_SEMANTIC_GRAPH,
    XG_EVIDENCE_CACHE_BODY_SUMMARY,
    XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE,
} XgEvidenceCachePhase;

typedef struct XgEvidenceCacheRequestKey {
    uint32_t schema_version;
    uint32_t phase;
    XgModuleId module_id;
    uint32_t profile;
    uint64_t source_hash;
    uint64_t compiler_semver_hash;
    uint64_t profile_hash;
    uint64_t imported_summary_hash;
} XgEvidenceCacheRequestKey;

typedef struct XgEvidenceCacheKey {
    uint32_t schema_version;
    uint32_t phase;
    XgModuleId module_id;
    uint32_t profile;
    uint64_t compiler_semver_hash;
    uint64_t profile_hash;
    uint64_t imported_summary_hash;
    uint64_t content_hash;
} XgEvidenceCacheKey;

enum {
    XG_EVIDENCE_CACHE_PHASE_COUNT = 4,
};

typedef struct XgEvidenceCacheManifest {
    XgEvidenceCacheKey keys[XG_EVIDENCE_CACHE_PHASE_COUNT];
    uint32_t phase_mask;
} XgEvidenceCacheManifest;

typedef struct XgEvidenceCachePayloadInfo {
    XgEvidenceCacheRequestKey request_key;
    XgEvidenceCacheKey key;
    const char *body; /* points into the parsed payload text */
    size_t body_len;
    size_t payload_bytes;
    uint64_t request_hash;
    uint64_t key_hash;
    uint64_t payload_hash;
    uint32_t phase;
} XgEvidenceCachePayloadInfo;

typedef struct XgEvidencePackageImportReport {
    uint64_t package_hash;
    uint32_t modules_remapped;
    uint32_t modules_added;
    uint32_t rows_imported;
    uint32_t payloads_imported;
} XgEvidencePackageImportReport;

typedef enum XgModuleFlags {
    XG_MODULE_EMBEDDED_SOURCE = 1u << 0,
} XgModuleFlags;

typedef struct XgModuleSummary {
    XgModuleId module_id;
    uint32_t name_id;
    uint64_t canonical_hash;
    uint64_t source_hash;
    uint8_t kind;
    uint32_t flags;
} XgModuleSummary;

XR_FUNC bool xg_module_summary_identity_complete(const XgModuleSummary *module);
XR_FUNC bool xg_module_summary_identity_matches(const XgModuleSummary *a, const XgModuleSummary *b);

typedef struct XgDeclSummary {
    XgModuleId module_id;
    uint32_t source_node_id;
    XgDeclId decl_id;
    uint8_t kind;
    uint32_t flags;
    uint32_t name_id;
    uint32_t type_key;
    uint32_t signature_key;
    uint32_t source_span_id;
    uint32_t derive_flags;
    uint32_t storage_flags;
    uint8_t storage_owner;
    uint8_t storage_mutability;
    uint8_t address_identity;
    uint8_t materialization_kind;
} XgDeclSummary;

typedef struct XgClassSummary {
    XgModuleId module_id;
    XgDeclId decl_id;
    XgClassId class_id;
    XgClassId parent_class_id;
    uint32_t name_id;
    uint32_t flags;
    uint32_t field_start;
    uint32_t field_count;
    uint32_t method_start;
    uint32_t method_count;
    uint32_t interface_start;
    uint32_t interface_count;
    XgClassId generic_origin_class_id;
    uint32_t generic_origin_name_id;
    uint32_t generic_type_key;
    uint32_t generic_type_arg_key_start;
    uint16_t generic_type_arg_count;
    uint8_t decl_kind;
} XgClassSummary;

typedef struct XgClassFieldSummary {
    XgFieldId field_id;
    XgModuleId module_id;
    uint32_t source_node_id;
    XgClassId owner_class_id;
    uint32_t name_id;
    uint32_t type_key;
    uint32_t target_name_id;
    XgClassId target_class_id;
    XgInterfaceId target_interface_id;
    uint32_t element_type_key;
    uint32_t key_type_key;
    uint32_t value_type_key;
    uint32_t fixed_length;
    uint32_t decl_ordinal;
    uint32_t instance_slot;
    uint32_t flags;
    uint8_t semantic_kind;
    uint8_t native_width;
} XgClassFieldSummary;

typedef struct XgMethodSummary {
    XgMethodId method_id;
    XgClassId owner_class_id;
    uint32_t source_node_id;
    uint32_t name_id;
    uint32_t signature_key;
    XgMethodId override_of;
    XgMethodId root_method_id;
    uint32_t override_depth;
    uint32_t default_arg_contract_id;
    uint32_t flags;
} XgMethodSummary;

typedef struct XgInterfaceImplSummary {
    XgClassId implementor_class_id;
    XgInterfaceId interface_id;
    uint32_t name_id;
    uint32_t type_key;
    uint32_t source_span_id;
    uint32_t flags;
} XgInterfaceImplSummary;

typedef struct XgInterfaceExtendsSummary {
    XgInterfaceId child_interface_id;
    XgInterfaceId parent_interface_id;
    uint32_t name_id;
    uint32_t type_key;
    uint32_t source_span_id;
    uint32_t flags;
} XgInterfaceExtendsSummary;

typedef struct XgInterfaceMethodSummary {
    XgInterfaceMethodId interface_method_id;
    XgInterfaceId owner_interface_id;
    uint32_t name_id;
    uint32_t signature_key;
    uint32_t ordinal;
    uint32_t source_span_id;
    uint32_t flags;
} XgInterfaceMethodSummary;

typedef struct XgInterfaceObjectUseSummary {
    XgInterfaceObjectUseId use_id;
    XgInterfaceId interface_id;
    XgFuncId owner_func_id;
    uint32_t source_span_id;
    uint32_t body_ordinal;
    uint32_t type_key;
    uint32_t reason;
    uint32_t flags;
} XgInterfaceObjectUseSummary;

typedef struct XgBodySummary {
    XgFuncId func_id;
    XgModuleId module_id;
    uint32_t source_node_id;
    XgDeclId owner_decl_id;
    XgClassId owner_class_id;
    XgMethodId owner_method_id;
    uint32_t name_id;
    uint32_t signature_key;
    uint32_t source_span_id;
    uint8_t kind;
    uint64_t body_hash;
    uint32_t effect_bits;
    uint32_t escape_bits;
    uint32_t capability_bits;
    uint32_t param_storage_key;
    uint32_t param_storage_start;
    uint32_t param_storage_count;
    uint32_t callsite_start;
    uint32_t callsite_count;
    uint32_t metadata_use_bits;
    uint32_t static_data_use_bits;
} XgBodySummary;

typedef struct XgParamStorageSummary {
    XgParamStorageId requirement_id;
    XgFuncId owner_func_id;
    uint32_t param_index;
    uint8_t storage_owner;
    uint32_t flags;
} XgParamStorageSummary;

typedef struct XgCallsiteSummary {
    XgCallsiteId callsite_id;
    XgFuncId owner_func_id;
    uint32_t source_node_id;
    uint32_t source_span_id;
    uint32_t body_ordinal;
    uint8_t kind;
    XgFuncId static_target_func_id;
    XgClassId receiver_static_class_id;
    XgInterfaceId receiver_static_interface_id;
    XgMethodId method_id;
    uint32_t method_name_id;
    uint32_t method_signature_key;
    uint32_t arg_type_key_start;
    uint16_t arg_count;
    uint32_t flags;
} XgCallsiteSummary;

typedef struct XgLinkDependencySummary {
    XgLinkId link_id;
    XgModuleId module_id;
    XgDeclId decl_id;
    uint32_t source_span_id;
    uint32_t name_id;
    uint8_t kind;
    uint32_t flags;
    char name[XG_LINK_DEP_NAME_MAX];
} XgLinkDependencySummary;

typedef struct XgGenericInstSummary {
    XgGenericInstId generic_inst_id;
    XgModuleId module_id;
    XgDeclId origin_decl_id;
    XgFuncId origin_func_id;
    XgMethodId origin_method_id;
    XgClassId origin_class_id;
    XgFuncId specialized_func_id;
    XgClassId specialized_class_id;
    XgCallsiteId root_callsite_id;
    XgInterfaceId constraint_interface_id;
    uint32_t name_id;
    uint32_t type_key;
    uint32_t type_arg_key_start;
    uint16_t type_arg_count;
    uint32_t source_span_id;
    uint8_t kind;
    uint32_t flags;
} XgGenericInstSummary;

typedef struct XgGenericBodyUseSummary {
    XgGenericBodyUseId use_id;
    XgGenericInstId generic_inst_id;
    XgModuleId module_id;
    XgFuncId owner_func_id;
    XgFuncId origin_body_func_id;
    XgFuncId specialized_body_func_id;
    XgCallsiteId root_callsite_id;
    uint32_t type_key;
    uint32_t type_arg_key_start;
    uint16_t type_arg_count;
    uint32_t estimated_body_size;
    uint32_t flags;
    uint64_t body_use_hash;
} XgGenericBodyUseSummary;

typedef struct XgGenericStorageSummary {
    XgGenericStorageId storage_id;
    XgGenericInstId generic_inst_id;
    XgModuleId module_id;
    uint8_t storage_kind;
    uint32_t origin_type_key;
    uint32_t specialized_type_key;
    uint32_t elem_type_key;
    uint32_t key_type_key;
    uint32_t value_type_key;
    uint32_t container_plan_id;
    uint32_t flags;
    uint64_t storage_hash;
} XgGenericStorageSummary;

typedef struct XgGenericCodeSizeSummary {
    XgGenericCodeSizeId code_size_id;
    XgGenericInstId generic_inst_id;
    XgModuleId module_id;
    XgGenericBodyUseId body_use_id;
    uint32_t origin_body_size_estimate;
    uint32_t specialized_body_size_estimate;
    uint32_t instantiation_count;
    uint32_t threshold;
    uint32_t flags;
} XgGenericCodeSizeSummary;

typedef struct XgSequenceAccessSummary {
    XgSequenceAccessId access_id;
    XgFuncId owner_func_id;
    uint32_t source_span_id;
    uint32_t body_ordinal;
    uint8_t sequence_kind;
    uint8_t access_kind;
    uint32_t receiver_type_key;
    uint32_t elem_type_key;
    uint32_t index_expr_id;
    uint32_t length_expr_id;
    uint32_t flags;
} XgSequenceAccessSummary;

typedef struct XgCapacityOpSummary {
    XgCapacityOpId op_id;
    XgFuncId owner_func_id;
    uint32_t source_span_id;
    uint32_t body_ordinal;
    uint8_t sequence_kind;
    uint8_t op_kind;
    uint32_t receiver_type_key;
    uint32_t elem_type_key;
    uint32_t count_expr_id;
    uint32_t loop_id;
    uint32_t flags;
} XgCapacityOpSummary;

typedef struct XgBulkOpSummary {
    XgBulkOpId op_id;
    XgFuncId owner_func_id;
    uint32_t source_span_id;
    uint32_t body_ordinal;
    uint8_t op_kind;
    uint32_t elem_type_key;
    uint32_t src_type_key;
    uint32_t dst_type_key;
    uint32_t length_expr_id;
    uint32_t flags;
} XgBulkOpSummary;

typedef struct XgEncodingOpSummary {
    XgEncodingOpId op_id;
    XgFuncId owner_func_id;
    uint32_t source_span_id;
    uint32_t body_ordinal;
    uint8_t op_kind;
    uint32_t input_type_key;
    uint32_t output_type_key;
    uint32_t flags;
} XgEncodingOpSummary;

typedef struct XgDeriveSummary {
    XgDeriveId derive_id;
    XgModuleId module_id;
    XgDeclId owner_decl_id;
    uint32_t source_span_id;
    uint32_t type_key;
    uint8_t derive_kind;
    uint32_t field_start;
    uint16_t field_count;
    uint32_t method_start;
    uint16_t method_count;
    uint32_t flags;
    uint64_t derive_hash;
} XgDeriveSummary;

typedef struct XgDerivedFieldSummary {
    XgDerivedFieldId field_id;
    XgDeriveId derive_id;
    uint16_t field_ordinal;
    uint32_t name_id;
    uint32_t type_key;
    uint32_t source_field_id;
    uint32_t flags;
} XgDerivedFieldSummary;

typedef struct XgDerivedMethodSummary {
    XgDerivedMethodId method_id;
    XgDeriveId derive_id;
    uint8_t method_kind;
    XgFuncId generated_body_func_id;
    uint32_t signature_key;
    uint32_t flags;
} XgDerivedMethodSummary;

typedef struct XgJsonShapeSummary {
    XgJsonShapeId json_shape_id;
    XgModuleId module_id;
    XgFuncId owner_func_id;
    uint32_t source_span_id;
    uint32_t type_key;
    uint32_t field_name_start;
    uint16_t field_count;
    uint8_t shape_kind;
    uint32_t flags;
    uint64_t shape_hash;
} XgJsonShapeSummary;

typedef struct XgJsonFieldSummary {
    XgJsonFieldId field_id;
    XgJsonShapeId shape_id;
    uint16_t field_ordinal;
    uint32_t name_id;
    uint32_t type_key;
    uint32_t flags;
} XgJsonFieldSummary;

typedef struct XgJsonAccessSummary {
    XgJsonAccessId json_access_id;
    XgModuleId module_id;
    XgFuncId owner_func_id;
    XgJsonShapeId receiver_shape_id;
    uint32_t source_span_id;
    uint32_t key_name_id;
    uint32_t result_type_key;
    uint16_t field_ordinal;
    uint8_t access_kind;
    uint32_t flags;
} XgJsonAccessSummary;

typedef struct XgJsonCodecSummary {
    XgJsonCodecId codec_id;
    XgModuleId module_id;
    XgFuncId owner_func_id;
    uint32_t source_span_id;
    uint8_t codec_kind;
    uint32_t input_type_key;
    uint32_t target_type_key;
    XgJsonShapeId input_shape_id;
    XgJsonShapeId output_shape_id;
    uint16_t field_count;
    uint32_t flags;
} XgJsonCodecSummary;

typedef struct XgRecordShapeSummary {
    XgRecordShapeId record_shape_id;
    XgModuleId module_id;
    XgFuncId owner_func_id;
    uint32_t source_span_id;
    uint32_t type_key;
    uint32_t field_name_start;
    uint16_t field_count;
    uint8_t shape_kind;
    uint32_t flags;
    uint64_t shape_hash;
} XgRecordShapeSummary;

typedef struct XgRecordFieldSummary {
    XgRecordFieldId field_id;
    XgRecordShapeId shape_id;
    uint16_t field_ordinal;
    uint32_t name_id;
    uint32_t type_key;
    uint32_t default_value_id;
    uint32_t flags;
} XgRecordFieldSummary;

typedef struct XgRecordAccessSummary {
    XgRecordAccessId record_access_id;
    XgModuleId module_id;
    XgFuncId owner_func_id;
    XgRecordShapeId receiver_shape_id;
    uint32_t source_span_id;
    uint32_t field_name_id;
    uint32_t result_type_key;
    uint16_t field_ordinal;
    uint8_t access_kind;
    uint32_t flags;
} XgRecordAccessSummary;

typedef struct XgRecordMergeSummary {
    XgRecordMergeId merge_id;
    XgModuleId module_id;
    XgFuncId owner_func_id;
    uint32_t source_span_id;
    XgRecordShapeId base_shape_id;
    XgRecordShapeId patch_shape_id;
    XgRecordShapeId result_shape_id;
    uint16_t base_field_count;
    uint16_t patch_field_count;
    uint16_t result_field_count;
    uint16_t overwrite_count;
    uint32_t copy_table_id;
    uint32_t flags;
    uint64_t merge_hash;
} XgRecordMergeSummary;

typedef struct XgOptionsBagSummary {
    XgOptionsId options_id;
    XgModuleId module_id;
    XgFuncId owner_func_id;
    XgCallsiteId callsite_id;
    XgRecordShapeId param_shape_id;
    XgRecordShapeId supplied_shape_id;
    uint32_t source_span_id;
    uint32_t supplied_field_mask_id;
    uint32_t default_field_mask_id;
    uint32_t required_field_mask_id;
    uint16_t supplied_count;
    uint16_t default_count;
    uint16_t required_count;
    uint8_t action;
    uint32_t flags;
} XgOptionsBagSummary;

typedef struct XgMapShapeSummary {
    XgMapShapeId shape_id;
    XgModuleId module_id;
    XgFuncId owner_func_id;
    uint32_t source_span_id;
    uint8_t container_kind;
    uint8_t source;
    uint32_t key_type_key;
    uint32_t value_type_key;
    uint32_t entry_start;
    uint16_t entry_count;
    uint32_t literal_count;
    uint32_t flags;
    uint64_t shape_hash;
} XgMapShapeSummary;

typedef struct XgMapEntrySummary {
    XgMapEntryId entry_id;
    XgMapShapeId shape_id;
    uint32_t entry_ordinal;
    uint32_t key_const_id;
    uint32_t value_const_id;
    int64_t key_i64;
    uint64_t prehash;
    uint32_t flags;
} XgMapEntrySummary;

typedef struct XgKeyAccessSummary {
    XgKeyAccessId access_id;
    XgFuncId owner_func_id;
    uint32_t source_span_id;
    uint32_t body_ordinal;
    uint8_t container_kind;
    uint8_t op;
    XgMapShapeId receiver_shape_id;
    uint32_t receiver_type_key;
    uint32_t key_type_key;
    uint32_t value_type_key;
    uint32_t key_const_id;
    uint64_t key_prehash;
    uint32_t flags;
} XgKeyAccessSummary;

typedef struct XgHashEqSummary {
    XgHashEqId hash_eq_id;
    uint32_t type_key;
    uint8_t kind;
    XgDeriveId eq_derive_id;
    XgDeriveId hash_derive_id;
    XgFuncId eq_func_id;
    XgFuncId hash_func_id;
    uint32_t flags;
} XgHashEqSummary;

typedef struct XgGlobalEvidence {
    XgBuildKey key;

    XgModuleSummary *modules;
    XgDeclSummary *decls;
    XgClassSummary *classes;
    XgClassFieldSummary *class_fields;
    XgMethodSummary *methods;
    XgInterfaceImplSummary *interface_impls;
    XgInterfaceExtendsSummary *interface_extends;
    XgInterfaceMethodSummary *interface_methods;
    XgInterfaceObjectUseSummary *interface_object_uses;
    XgBodySummary *bodies;
    XgParamStorageSummary *param_storages;
    XgCallsiteSummary *callsites;
    XgLinkDependencySummary *link_deps;
    XgGenericInstSummary *generic_insts;
    XgGenericBodyUseSummary *generic_body_uses;
    XgGenericStorageSummary *generic_storages;
    XgGenericCodeSizeSummary *generic_code_sizes;
    XgSequenceAccessSummary *sequence_accesses;
    XgCapacityOpSummary *capacity_ops;
    XgBulkOpSummary *bulk_ops;
    XgEncodingOpSummary *encoding_ops;
    XgDeriveSummary *derives;
    XgDerivedFieldSummary *derived_fields;
    XgDerivedMethodSummary *derived_methods;
    XgJsonShapeSummary *json_shapes;
    XgJsonFieldSummary *json_fields;
    XgJsonAccessSummary *json_accesses;
    XgJsonCodecSummary *json_codecs;
    XgRecordShapeSummary *record_shapes;
    XgRecordFieldSummary *record_fields;
    XgRecordAccessSummary *record_accesses;
    XgRecordMergeSummary *record_merges;
    XgOptionsBagSummary *options_bags;
    XgMapShapeSummary *map_shapes;
    XgMapEntrySummary *map_entries;
    XgKeyAccessSummary *key_accesses;
    XgHashEqSummary *hash_eqs;

    uint32_t nmodules;
    uint32_t ndecls;
    uint32_t nclasses;
    uint32_t nclass_fields;
    uint32_t nmethods;
    uint32_t ninterface_impls;
    uint32_t ninterface_extends;
    uint32_t ninterface_methods;
    uint32_t ninterface_object_uses;
    uint32_t nbodies;
    uint32_t nparam_storages;
    uint32_t ncallsites;
    uint32_t nlink_deps;
    uint32_t ngeneric_insts;
    uint32_t ngeneric_body_uses;
    uint32_t ngeneric_storages;
    uint32_t ngeneric_code_sizes;
    uint32_t nsequence_accesses;
    uint32_t ncapacity_ops;
    uint32_t nbulk_ops;
    uint32_t nencoding_ops;
    uint32_t nderives;
    uint32_t nderived_fields;
    uint32_t nderived_methods;
    uint32_t njson_shapes;
    uint32_t njson_fields;
    uint32_t njson_accesses;
    uint32_t njson_codecs;
    uint32_t nrecord_shapes;
    uint32_t nrecord_fields;
    uint32_t nrecord_accesses;
    uint32_t nrecord_merges;
    uint32_t noptions_bags;
    uint32_t nmap_shapes;
    uint32_t nmap_entries;
    uint32_t nkey_accesses;
    uint32_t nhash_eqs;

    uint32_t module_cap;
    uint32_t decl_cap;
    uint32_t class_cap;
    uint32_t class_field_cap;
    uint32_t method_cap;
    uint32_t interface_impl_cap;
    uint32_t interface_extend_cap;
    uint32_t interface_method_cap;
    uint32_t interface_object_use_cap;
    uint32_t body_cap;
    uint32_t param_storage_cap;
    uint32_t callsite_cap;
    uint32_t link_dep_cap;
    uint32_t generic_inst_cap;
    uint32_t generic_body_use_cap;
    uint32_t generic_storage_cap;
    uint32_t generic_code_size_cap;
    uint32_t sequence_access_cap;
    uint32_t capacity_op_cap;
    uint32_t bulk_op_cap;
    uint32_t encoding_op_cap;
    uint32_t derive_cap;
    uint32_t derived_field_cap;
    uint32_t derived_method_cap;
    uint32_t json_shape_cap;
    uint32_t json_field_cap;
    uint32_t json_access_cap;
    uint32_t json_codec_cap;
    uint32_t record_shape_cap;
    uint32_t record_field_cap;
    uint32_t record_access_cap;
    uint32_t record_merge_cap;
    uint32_t options_bag_cap;
    uint32_t map_shape_cap;
    uint32_t map_entry_cap;
    uint32_t key_access_cap;
    uint32_t hash_eq_cap;
} XgGlobalEvidence;

XR_FUNC uint32_t xg_name_id(const char *name);
XR_FUNC uint32_t xg_stable_source_node_id(XgModuleId module_id, uint32_t ast_kind, uint32_t line,
                                          uint32_t column);
XR_FUNC uint32_t xg_synthetic_type_key(uint8_t tref_kind);
XR_FUNC uint32_t xg_synthetic_width_type_key(uint8_t tref_kind, uint8_t native_width);
XR_FUNC uint64_t xg_json_shape_hash_begin(uint32_t field_count);
XR_FUNC uint64_t xg_json_shape_hash_add_field(uint64_t hash, uint8_t shape_kind, uint32_t name_id,
                                              uint32_t type_key);
XR_FUNC const char *xg_build_profile_name(uint32_t profile);
XR_FUNC const char *xg_decl_kind_name(uint8_t kind);
XR_FUNC const char *xg_callsite_kind_name(uint8_t kind);
XR_FUNC const char *xg_link_dependency_kind_name(uint8_t kind);
XR_FUNC const char *xg_generic_inst_kind_name(uint8_t kind);
XR_FUNC const char *xg_generic_storage_kind_name(uint8_t kind);
XR_FUNC const char *xg_sequence_kind_name(uint8_t kind);
XR_FUNC const char *xg_sequence_access_kind_name(uint8_t kind);
XR_FUNC const char *xg_capacity_op_kind_name(uint8_t kind);
XR_FUNC const char *xg_bulk_op_kind_name(uint8_t kind);
XR_FUNC const char *xg_encoding_op_kind_name(uint8_t kind);
XR_FUNC const char *xg_derive_kind_name(uint8_t kind);
XR_FUNC const char *xg_derived_method_kind_name(uint8_t kind);
XR_FUNC const char *xg_json_shape_kind_name(uint8_t kind);
XR_FUNC const char *xg_json_access_kind_name(uint8_t kind);
XR_FUNC const char *xg_json_codec_kind_name(uint8_t kind);
XR_FUNC const char *xg_record_shape_kind_name(uint8_t kind);
XR_FUNC const char *xg_record_access_kind_name(uint8_t kind);
XR_FUNC const char *xg_options_action_name(uint8_t action);
XR_FUNC const char *xg_map_container_kind_name(uint8_t kind);
XR_FUNC const char *xg_map_shape_source_name(uint8_t source);
XR_FUNC const char *xg_key_access_op_name(uint8_t op);
XR_FUNC const char *xg_hash_eq_kind_name(uint8_t kind);
XR_FUNC const char *xg_body_effect_name(uint32_t effect);
XR_FUNC const uint32_t *xg_body_effect_catalog(uint32_t *out_count);
XR_FUNC const char *xg_body_escape_name(uint32_t escape);
XR_FUNC const uint32_t *xg_body_escape_catalog(uint32_t *out_count);
XR_FUNC const char *xg_interface_object_use_name(uint32_t reason);
XR_FUNC const uint32_t *xg_interface_object_use_catalog(uint32_t *out_count);
XR_FUNC const char *xg_capability_name(uint32_t capability);
XR_FUNC const uint32_t *xg_capability_catalog(uint32_t *out_count);
XR_FUNC const char *xg_metadata_name(uint32_t metadata);
XR_FUNC const uint32_t *xg_metadata_catalog(uint32_t *out_count);
XR_FUNC const char *xg_static_data_name(uint32_t static_data);
XR_FUNC const uint32_t *xg_static_data_catalog(uint32_t *out_count);
XR_FUNC const char *xg_evidence_cache_phase_name(uint32_t phase);

XR_FUNC void xg_global_evidence_init(XgGlobalEvidence *evidence, XgBuildKey key);
XR_FUNC void xg_global_evidence_free(XgGlobalEvidence *evidence);

XR_FUNC bool xg_global_evidence_reserve_modules(XgGlobalEvidence *evidence, uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_decls(XgGlobalEvidence *evidence, uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_classes(XgGlobalEvidence *evidence, uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_class_fields(XgGlobalEvidence *evidence, uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_methods(XgGlobalEvidence *evidence, uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_interface_impls(XgGlobalEvidence *evidence,
                                                        uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_interface_extends(XgGlobalEvidence *evidence,
                                                          uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_interface_methods(XgGlobalEvidence *evidence,
                                                          uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_interface_object_uses(XgGlobalEvidence *evidence,
                                                              uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_bodies(XgGlobalEvidence *evidence, uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_param_storages(XgGlobalEvidence *evidence,
                                                       uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_callsites(XgGlobalEvidence *evidence, uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_link_deps(XgGlobalEvidence *evidence, uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_generic_insts(XgGlobalEvidence *evidence,
                                                      uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_generic_body_uses(XgGlobalEvidence *evidence,
                                                          uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_generic_storages(XgGlobalEvidence *evidence,
                                                         uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_generic_code_sizes(XgGlobalEvidence *evidence,
                                                           uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_sequence_accesses(XgGlobalEvidence *evidence,
                                                          uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_capacity_ops(XgGlobalEvidence *evidence, uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_bulk_ops(XgGlobalEvidence *evidence, uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_encoding_ops(XgGlobalEvidence *evidence, uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_derives(XgGlobalEvidence *evidence, uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_derived_fields(XgGlobalEvidence *evidence,
                                                       uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_derived_methods(XgGlobalEvidence *evidence,
                                                        uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_json_shapes(XgGlobalEvidence *evidence, uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_json_fields(XgGlobalEvidence *evidence, uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_json_accesses(XgGlobalEvidence *evidence,
                                                      uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_json_codecs(XgGlobalEvidence *evidence, uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_record_shapes(XgGlobalEvidence *evidence,
                                                      uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_record_fields(XgGlobalEvidence *evidence,
                                                      uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_record_accesses(XgGlobalEvidence *evidence,
                                                        uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_record_merges(XgGlobalEvidence *evidence,
                                                      uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_options_bags(XgGlobalEvidence *evidence, uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_map_shapes(XgGlobalEvidence *evidence, uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_map_entries(XgGlobalEvidence *evidence, uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_key_accesses(XgGlobalEvidence *evidence, uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_hash_eqs(XgGlobalEvidence *evidence, uint32_t capacity);

XR_FUNC XgModuleSummary *xg_global_evidence_add_module(XgGlobalEvidence *evidence,
                                                       const XgModuleSummary *summary);
XR_FUNC XgDeclSummary *xg_global_evidence_add_decl(XgGlobalEvidence *evidence,
                                                   const XgDeclSummary *summary);
XR_FUNC XgClassSummary *xg_global_evidence_add_class(XgGlobalEvidence *evidence,
                                                     const XgClassSummary *summary);
XR_FUNC XgClassFieldSummary *xg_global_evidence_add_class_field(XgGlobalEvidence *evidence,
                                                                const XgClassFieldSummary *summary);
XR_FUNC XgMethodSummary *xg_global_evidence_add_method(XgGlobalEvidence *evidence,
                                                       const XgMethodSummary *summary);
XR_FUNC XgInterfaceImplSummary *
xg_global_evidence_add_interface_impl(XgGlobalEvidence *evidence,
                                      const XgInterfaceImplSummary *summary);
XR_FUNC XgInterfaceExtendsSummary *
xg_global_evidence_add_interface_extends(XgGlobalEvidence *evidence,
                                         const XgInterfaceExtendsSummary *summary);
XR_FUNC XgInterfaceMethodSummary *
xg_global_evidence_add_interface_method(XgGlobalEvidence *evidence,
                                        const XgInterfaceMethodSummary *summary);
XR_FUNC XgInterfaceObjectUseSummary *
xg_global_evidence_add_interface_object_use(XgGlobalEvidence *evidence,
                                            const XgInterfaceObjectUseSummary *summary);
XR_FUNC XgBodySummary *xg_global_evidence_add_body(XgGlobalEvidence *evidence,
                                                   const XgBodySummary *summary);
XR_FUNC XgParamStorageSummary *
xg_global_evidence_add_param_storage(XgGlobalEvidence *evidence,
                                     const XgParamStorageSummary *summary);
XR_FUNC XgCallsiteSummary *xg_global_evidence_add_callsite(XgGlobalEvidence *evidence,
                                                           const XgCallsiteSummary *summary);
XR_FUNC XgLinkDependencySummary *
xg_global_evidence_add_link_dependency(XgGlobalEvidence *evidence,
                                       const XgLinkDependencySummary *summary);
XR_FUNC XgGenericInstSummary *
xg_global_evidence_add_generic_inst(XgGlobalEvidence *evidence,
                                    const XgGenericInstSummary *summary);
XR_FUNC XgGenericBodyUseSummary *
xg_global_evidence_add_generic_body_use(XgGlobalEvidence *evidence,
                                        const XgGenericBodyUseSummary *summary);
XR_FUNC XgGenericStorageSummary *
xg_global_evidence_add_generic_storage(XgGlobalEvidence *evidence,
                                       const XgGenericStorageSummary *summary);
XR_FUNC XgGenericCodeSizeSummary *
xg_global_evidence_add_generic_code_size(XgGlobalEvidence *evidence,
                                         const XgGenericCodeSizeSummary *summary);
XR_FUNC XgSequenceAccessSummary *
xg_global_evidence_add_sequence_access(XgGlobalEvidence *evidence,
                                       const XgSequenceAccessSummary *summary);
XR_FUNC XgCapacityOpSummary *xg_global_evidence_add_capacity_op(XgGlobalEvidence *evidence,
                                                                const XgCapacityOpSummary *summary);
XR_FUNC XgBulkOpSummary *xg_global_evidence_add_bulk_op(XgGlobalEvidence *evidence,
                                                        const XgBulkOpSummary *summary);
XR_FUNC XgEncodingOpSummary *xg_global_evidence_add_encoding_op(XgGlobalEvidence *evidence,
                                                                const XgEncodingOpSummary *summary);
XR_FUNC XgDeriveSummary *xg_global_evidence_add_derive(XgGlobalEvidence *evidence,
                                                       const XgDeriveSummary *summary);
XR_FUNC XgDerivedFieldSummary *
xg_global_evidence_add_derived_field(XgGlobalEvidence *evidence,
                                     const XgDerivedFieldSummary *summary);
XR_FUNC XgDerivedMethodSummary *
xg_global_evidence_add_derived_method(XgGlobalEvidence *evidence,
                                      const XgDerivedMethodSummary *summary);
XR_FUNC XgJsonShapeSummary *xg_global_evidence_add_json_shape(XgGlobalEvidence *evidence,
                                                              const XgJsonShapeSummary *summary);
XR_FUNC XgJsonFieldSummary *xg_global_evidence_add_json_field(XgGlobalEvidence *evidence,
                                                              const XgJsonFieldSummary *summary);
XR_FUNC XgJsonAccessSummary *xg_global_evidence_add_json_access(XgGlobalEvidence *evidence,
                                                                const XgJsonAccessSummary *summary);
XR_FUNC XgJsonCodecSummary *xg_global_evidence_add_json_codec(XgGlobalEvidence *evidence,
                                                              const XgJsonCodecSummary *summary);
XR_FUNC XgRecordShapeSummary *
xg_global_evidence_add_record_shape(XgGlobalEvidence *evidence,
                                    const XgRecordShapeSummary *summary);
XR_FUNC XgRecordFieldSummary *
xg_global_evidence_add_record_field(XgGlobalEvidence *evidence,
                                    const XgRecordFieldSummary *summary);
XR_FUNC XgRecordAccessSummary *
xg_global_evidence_add_record_access(XgGlobalEvidence *evidence,
                                     const XgRecordAccessSummary *summary);
XR_FUNC XgRecordMergeSummary *
xg_global_evidence_add_record_merge(XgGlobalEvidence *evidence,
                                    const XgRecordMergeSummary *summary);
XR_FUNC XgOptionsBagSummary *xg_global_evidence_add_options_bag(XgGlobalEvidence *evidence,
                                                                const XgOptionsBagSummary *summary);
XR_FUNC XgMapShapeSummary *xg_global_evidence_add_map_shape(XgGlobalEvidence *evidence,
                                                            const XgMapShapeSummary *summary);
XR_FUNC XgMapEntrySummary *xg_global_evidence_add_map_entry(XgGlobalEvidence *evidence,
                                                            const XgMapEntrySummary *summary);
XR_FUNC XgKeyAccessSummary *xg_global_evidence_add_key_access(XgGlobalEvidence *evidence,
                                                              const XgKeyAccessSummary *summary);
XR_FUNC XgHashEqSummary *xg_global_evidence_add_hash_eq(XgGlobalEvidence *evidence,
                                                        const XgHashEqSummary *summary);
XR_FUNC const XgClassFieldSummary *
xg_global_evidence_find_class_field(const XgGlobalEvidence *evidence, XgFieldId field_id);
XR_FUNC const XgCallsiteSummary *xg_global_evidence_find_callsite(const XgGlobalEvidence *evidence,
                                                                  XgCallsiteId callsite_id);
XR_FUNC const XgGenericInstSummary *
xg_global_evidence_find_generic_inst(const XgGlobalEvidence *evidence,
                                     XgGenericInstId generic_inst_id);
XR_FUNC const XgGenericBodyUseSummary *
xg_global_evidence_find_generic_body_use(const XgGlobalEvidence *evidence,
                                         XgGenericBodyUseId use_id);
XR_FUNC const XgGenericStorageSummary *
xg_global_evidence_find_generic_storage(const XgGlobalEvidence *evidence,
                                        XgGenericStorageId storage_id);
XR_FUNC const XgGenericCodeSizeSummary *
xg_global_evidence_find_generic_code_size(const XgGlobalEvidence *evidence,
                                          XgGenericCodeSizeId code_size_id);
XR_FUNC const XgSequenceAccessSummary *
xg_global_evidence_find_sequence_access(const XgGlobalEvidence *evidence,
                                        XgSequenceAccessId access_id);
XR_FUNC const XgCapacityOpSummary *
xg_global_evidence_find_capacity_op(const XgGlobalEvidence *evidence, XgCapacityOpId op_id);
XR_FUNC const XgBulkOpSummary *xg_global_evidence_find_bulk_op(const XgGlobalEvidence *evidence,
                                                               XgBulkOpId op_id);
XR_FUNC const XgEncodingOpSummary *
xg_global_evidence_find_encoding_op(const XgGlobalEvidence *evidence, XgEncodingOpId op_id);
XR_FUNC const XgJsonShapeSummary *
xg_global_evidence_find_json_shape(const XgGlobalEvidence *evidence, XgJsonShapeId json_shape_id);
XR_FUNC const XgJsonFieldSummary *
xg_global_evidence_find_json_field(const XgGlobalEvidence *evidence, XgJsonFieldId field_id);
XR_FUNC const XgJsonCodecSummary *
xg_global_evidence_find_json_codec(const XgGlobalEvidence *evidence, XgJsonCodecId codec_id);
XR_FUNC const XgRecordShapeSummary *
xg_global_evidence_find_record_shape(const XgGlobalEvidence *evidence,
                                     XgRecordShapeId record_shape_id);
XR_FUNC const XgRecordFieldSummary *
xg_global_evidence_find_record_field(const XgGlobalEvidence *evidence, XgRecordFieldId field_id);
XR_FUNC const XgRecordMergeSummary *
xg_global_evidence_find_record_merge(const XgGlobalEvidence *evidence,
                                     XgRecordMergeId record_merge_id);
XR_FUNC const XgOptionsBagSummary *
xg_global_evidence_find_options_bag(const XgGlobalEvidence *evidence, XgOptionsId options_id);
XR_FUNC const XgMapShapeSummary *xg_global_evidence_find_map_shape(const XgGlobalEvidence *evidence,
                                                                   XgMapShapeId shape_id);
XR_FUNC const XgHashEqSummary *xg_global_evidence_find_hash_eq(const XgGlobalEvidence *evidence,
                                                               uint32_t type_key);
XR_FUNC bool xg_body_effects_compose_closed_world_calls(const XgGlobalEvidence *evidence,
                                                        const XgBodySummary *body,
                                                        uint32_t *out_effect_bits);
XR_FUNC bool xg_body_reachability_mark_closed_world_calls(const XgGlobalEvidence *evidence,
                                                          XgFuncId root_func_id, uint8_t *reachable,
                                                          uint32_t reachable_count);

XR_FUNC uint64_t xg_global_evidence_hash(const XgGlobalEvidence *evidence);
XR_FUNC XgEvidenceCacheKey xg_global_evidence_cache_key(const XgGlobalEvidence *evidence,
                                                        uint32_t phase);
XR_FUNC XgEvidenceCacheRequestKey
xg_evidence_cache_request_key_from_build_key(const XgBuildKey *build_key, uint32_t phase);
XR_FUNC XgEvidenceCacheRequestKey
xg_global_evidence_cache_request_key(const XgGlobalEvidence *evidence, uint32_t phase);
XR_FUNC uint64_t xg_evidence_cache_request_key_hash(const XgEvidenceCacheRequestKey *key);
XR_FUNC bool xg_evidence_cache_request_key_matches(const XgEvidenceCacheRequestKey *cached,
                                                   const XgEvidenceCacheRequestKey *expected);
XR_FUNC bool xg_evidence_cache_request_key_format(const XgEvidenceCacheRequestKey *key, char *buf,
                                                  size_t buf_len);
XR_FUNC bool xg_evidence_cache_request_key_parse(const char *text,
                                                 XgEvidenceCacheRequestKey *out_key);
XR_FUNC uint64_t xg_evidence_cache_key_hash(const XgEvidenceCacheKey *key);
XR_FUNC bool xg_evidence_cache_key_matches(const XgEvidenceCacheKey *cached,
                                           const XgEvidenceCacheKey *expected);
XR_FUNC bool xg_evidence_cache_key_format(const XgEvidenceCacheKey *key, char *buf, size_t buf_len);
XR_FUNC bool xg_evidence_cache_key_parse(const char *text, XgEvidenceCacheKey *out_key);
XR_FUNC XgEvidenceCacheManifest xg_global_evidence_cache_manifest(const XgGlobalEvidence *evidence);
XR_FUNC const XgEvidenceCacheKey *
xg_evidence_cache_manifest_find(const XgEvidenceCacheManifest *manifest, uint32_t phase);
XR_FUNC bool xg_evidence_cache_manifest_phase_matches(const XgEvidenceCacheManifest *manifest,
                                                      const XgEvidenceCacheKey *expected);
XR_FUNC bool xg_evidence_cache_manifest_format(const XgEvidenceCacheManifest *manifest, char *buf,
                                               size_t buf_len);
XR_FUNC bool xg_evidence_cache_manifest_parse(const char *text,
                                              XgEvidenceCacheManifest *out_manifest);
XR_FUNC char *xg_global_evidence_cache_payload_dump(const XgGlobalEvidence *evidence,
                                                    uint32_t phase);
XR_FUNC bool xg_evidence_cache_payload_parse(const char *text,
                                             XgEvidenceCachePayloadInfo *out_info);
XR_FUNC bool xg_evidence_cache_payload_matches(const char *text,
                                               const XgEvidenceCacheKey *expected);
XR_FUNC bool xg_evidence_cache_payload_request_matches(const char *text,
                                                       const XgEvidenceCacheRequestKey *expected);
XR_FUNC bool xg_evidence_cache_payload_materialize(const char *text,
                                                   XgGlobalEvidence *out_evidence);
XR_FUNC bool xg_imported_summary_hash_from_package_payloads(uint64_t seed,
                                                            const char *const *payloads,
                                                            uint32_t payload_count,
                                                            uint64_t *out_hash);
XR_FUNC bool xg_global_evidence_import_package_payload(XgGlobalEvidence *target,
                                                       const char *payload,
                                                       XgEvidencePackageImportReport *out_report);
XR_FUNC bool
xg_global_evidence_import_package_payload_set(XgGlobalEvidence *target, const char *const *payloads,
                                              uint32_t payload_count,
                                              XgEvidencePackageImportReport *out_report);
XR_FUNC char *xg_global_evidence_dump(const XgGlobalEvidence *evidence);

#endif  // XGLOBAL_SUMMARY_H
