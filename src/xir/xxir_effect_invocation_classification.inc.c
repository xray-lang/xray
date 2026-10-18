/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_classification.inc.c - Owned physical ordinal classification
 *
 * KEY CONCEPT:
 *   One immutable invocation input owns its physical classification. The
 *   records locate existing consumers; they contain no permission or origin.
 */
enum {
    EFFECT_PHYSICAL_CALLABLE=1,
    EFFECT_PHYSICAL_REFERENCE=2,
    EFFECT_PHYSICAL_BARRIER=4,
    EFFECT_PHYSICAL_TRANSFER=8,
    EFFECT_PHYSICAL_INVOCATION=16,
    EFFECT_PHYSICAL_RETURN=32,
    EFFECT_PHYSICAL_CLASS=64,
    EFFECT_PHYSICAL_DIRECT=128,
    EFFECT_PHYSICAL_CONTROL=256,
    EFFECT_PHYSICAL_GO=512,
    EFFECT_PHYSICAL_SLOT_PLACE=1024,
    EFFECT_PHYSICAL_ERROR_ESCAPE=2048,
    EFFECT_PHYSICAL_GENERATOR=4096,
    EFFECT_PHYSICAL_ITERATOR=8192,
    EFFECT_PHYSICAL_ITERATOR_ADVANCE=16384
};

typedef struct EffectPhysicalRecord { uint32_t value,mask; } EffectPhysicalRecord;
typedef struct EffectPhysicalFunction {
    uint32_t parameters,instructions,values,rows,origins,classes,count,capacity;
    EffectPhysicalRecord *records;
} EffectPhysicalFunction;
typedef struct EffectPhysicalDomain {
    XrCompileResources *resources;
    uint32_t count;
    EffectPhysicalFunction *functions;
} EffectPhysicalDomain;

static bool effect_invocation_iterator_operation(XrXirOp op) {
    return op==XR_XIR_ITERATOR_HAS_NEXT || op==XR_XIR_ITERATOR_NEXT || op==XR_XIR_ITERATOR_NTH;
}

static bool effect_physical_iterator(const XrXirTypes *types,XrXirType type) {
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    return node && node->kind==XR_XIR_TYPE_ITERATOR;
}
static bool effect_physical_origin(const XrXirTypes *types,XrXirType type) {
    return xr_xir_callable_signature(types,type)!=NULL || effect_physical_iterator(types,type);
}

/* The Error CFG and raw classifier use this one exhaustive opcode core.
 * Call identity, ROOT or NO_SUSPEND never substitute for its answer. */
static bool effect_physical_error_empty_opcode(XrXirOp op) {
    switch (op) {
    /* View faults enter the panic protocol; none throws a language Error. */
    case XR_XIR_VIEW_NEW: case XR_XIR_VIEW_REBORROW: case XR_XIR_VIEW_LEN:
    case XR_XIR_VIEW_GET: case XR_XIR_VIEW_SET: case XR_XIR_VIEW_COPY:
    case XR_XIR_VIEW_END: case XR_XIR_VIEW_CLEANUP:
    case XR_XIR_STRING_COPY_BYTES: case XR_XIR_STRING_REPLACE:
        return true;
    case XR_XIR_CONST_BOOL: case XR_XIR_CONST_INT: case XR_XIR_CONST_STRING: case XR_XIR_SLOT_LOAD:
    case XR_XIR_SLOT_INIT: case XR_XIR_SLOT_STORE: case XR_XIR_CHANNEL_TRY_SEND: case XR_XIR_CHANNEL_TRY_RECV:
    case XR_XIR_CHANNEL_SEND: case XR_XIR_CHANNEL_RECV: case XR_XIR_CHANNEL_RECV_OR:
    case XR_XIR_CHANNEL_SEND_TIMEOUT: case XR_XIR_CHANNEL_RECV_TIMEOUT:
    case XR_XIR_CHANNEL_NEW: case XR_XIR_CHANNEL_CAPACITY: case XR_XIR_CHANNEL_IS_CLOSED: case XR_XIR_CHANNEL_CLOSE:
    case XR_XIR_ATOMIC_NEW: case XR_XIR_ATOMIC_LOAD:
    case XR_XIR_ATOMIC_STORE: case XR_XIR_ATOMIC_ADD: case XR_XIR_ATOMIC_SUB: case XR_XIR_ATOMIC_FETCH_ADD:
    case XR_XIR_ATOMIC_FETCH_SUB: case XR_XIR_ATOMIC_SWAP: case XR_XIR_ATOMIC_COMPARE_EXCHANGE: case XR_XIR_ATOMIC_TOGGLE:
    case XR_XIR_ATOMIC_TO_STRING: case XR_XIR_COPY: case XR_XIR_SCALAR_COPY: case XR_XIR_OWNED_RETAIN:
    case XR_XIR_CONCAT_STRING: case XR_XIR_OUTPUT: case XR_XIR_WRITE_STREAM: case XR_XIR_PRINT:
    case XR_XIR_ADD_INT: case XR_XIR_EQ_INT: case XR_XIR_LT_INT: case XR_XIR_SUSPEND: case XR_XIR_GENERATOR_YIELD:
    case XR_XIR_JUMP: case XR_XIR_BRANCH: case XR_XIR_RETURN: case XR_XIR_LOCAL_NEW:
    case XR_XIR_LOCAL_READ: case XR_XIR_LOCAL_WRITE: case XR_XIR_SCALAR_LOCAL_NEW: case XR_XIR_SCALAR_LOCAL_READ:
    case XR_XIR_SCALAR_LOCAL_WRITE: case XR_XIR_OWNED_LOCAL_NEW: case XR_XIR_OWNED_LOCAL_READ: case XR_XIR_OWNED_LOCAL_WRITE:
    case XR_XIR_SUB_INT: case XR_XIR_MUL_INT: case XR_XIR_DIV_INT: case XR_XIR_REM_INT:
    case XR_XIR_NE_INT: case XR_XIR_LE_INT: case XR_XIR_GT_INT: case XR_XIR_GE_INT:
    case XR_XIR_AND_INT: case XR_XIR_OR_INT: case XR_XIR_XOR_INT: case XR_XIR_SHL_INT:
    case XR_XIR_SHR_INT: case XR_XIR_PHI: case XR_XIR_FUNCTION_REF: case XR_XIR_CELL_NEW:
    case XR_XIR_CELL_READ: case XR_XIR_CELL_WRITE: case XR_XIR_CELL_STRING_APPEND: case XR_XIR_CELL_PROJECT:
    case XR_XIR_CONVERT_NUMBER: case XR_XIR_CONST_FLOAT:
    case XR_XIR_NEG_FLOAT: case XR_XIR_EQ_FLOAT: case XR_XIR_NE_FLOAT: case XR_XIR_LT_FLOAT:
    case XR_XIR_LE_FLOAT: case XR_XIR_GT_FLOAT: case XR_XIR_GE_FLOAT: case XR_XIR_CELL_PLACE:
    case XR_XIR_SLOT_PLACE: case XR_XIR_MAP_NEW: case XR_XIR_SET_NEW: case XR_XIR_MAP_GET: case XR_XIR_MAP_INDEX: case XR_XIR_HASH_KEYS: case XR_XIR_HASH_VALUES: case XR_XIR_MAP_ENTRIES: case XR_XIR_HASH_CONTAINS:
    case XR_XIR_HASH_KEY_AT: case XR_XIR_MAP_VALUE_AT:
    case XR_XIR_MAP_CONTAINS_VALUE:
    case XR_XIR_SET_UNION: case XR_XIR_SET_INTERSECTION: case XR_XIR_SET_DIFFERENCE:
    case XR_XIR_SET_SYMMETRIC_DIFFERENCE: case XR_XIR_SET_IS_SUBSET: case XR_XIR_SET_IS_SUPERSET:
    case XR_XIR_HASH_LEN: case XR_XIR_MAP_SET: case XR_XIR_SET_ADD: case XR_XIR_HASH_DELETE: case XR_XIR_HASH_CLEAR:
    case XR_XIR_ARRAY_NEW: case XR_XIR_ARRAY_GET: case XR_XIR_ARRAY_SET:
    case XR_XIR_ARRAY_PUSH: case XR_XIR_ARRAY_LEN: case XR_XIR_ADD_FLOAT: case XR_XIR_SUB_FLOAT:
    case XR_XIR_MUL_FLOAT: case XR_XIR_DIV_FLOAT: case XR_XIR_STRUCT_NEW: case XR_XIR_STRUCT_GET:
    case XR_XIR_STRUCT_SET: case XR_XIR_LOCAL_UNINIT: case XR_XIR_STRING_LEN: case XR_XIR_EQ_STRING:
    case XR_XIR_NE_STRING: case XR_XIR_STRING_CONTAINS: case XR_XIR_STRING_STARTS_WITH: case XR_XIR_STRING_ENDS_WITH:
    case XR_XIR_STRING_INDEX_OF: case XR_XIR_STRING_LAST_INDEX_OF: case XR_XIR_ENUM_NEW: case XR_XIR_ENUM_TAG:
    case XR_XIR_ENUM_GET: case XR_XIR_MATCH_FAIL: case XR_XIR_ERROR_ERASE: case XR_XIR_INVOKE_RESULT:
    case XR_XIR_ERROR_IS: case XR_XIR_ERROR_NARROW: case XR_XIR_PANIC_CATCH: case XR_XIR_PANIC_CODE:
    case XR_XIR_PANIC_MESSAGE: case XR_XIR_CLEANUP_REGISTER: case XR_XIR_CLEANUP_LEAVE: case XR_XIR_CLEANUP_ERROR:
    case XR_XIR_CELL_LOCAL_WRITE: case XR_XIR_FIELD_PLACE: case XR_XIR_INDEX_PLACE: case XR_XIR_PLACE_READ:
    case XR_XIR_PLACE_WRITE: case XR_XIR_FUNCTION_WEAKEN: case XR_XIR_CLASS_NEW: case XR_XIR_CLASS_GET:
    case XR_XIR_CLASS_SET: case XR_XIR_ASSERT_CONDITION: case XR_XIR_INVOKE_DISCARD: case XR_XIR_EQUAL:
    case XR_XIR_NULLABLE_NONE: case XR_XIR_NULLABLE_SOME: case XR_XIR_CLOCK_NANOS: case XR_XIR_UTC_OFFSET_AT:
    case XR_XIR_TIMER_AFTER_MS: case XR_XIR_TO_STRING: case XR_XIR_OBJECT_PLACE: case XR_XIR_ARRAY_REPEAT:
    case XR_XIR_NULLABLE_IS_SOME: case XR_XIR_NULLABLE_UNWRAP: case XR_XIR_CONST_RUNE: case XR_XIR_RUNE_TO_INTEGER:
    case XR_XIR_INTEGER_TO_RUNE: case XR_XIR_LT_STRING: case XR_XIR_LE_STRING: case XR_XIR_GT_STRING:
    case XR_XIR_GE_STRING: case XR_XIR_TUPLE_NEW: case XR_XIR_TUPLE_FIELD: case XR_XIR_SLOT_GROUP_INIT:
    case XR_XIR_ITERATOR_CLOSE: case XR_XIR_RANGE_CHECK: case XR_XIR_ARRAY_CAPACITY: case XR_XIR_ARRAY_WITH_CAPACITY: case XR_XIR_ARRAY_RESERVE:
        return true;
    default: return false;
    }
}

/* Classification preserves non-callable transfers and aggregate stores.
 * A missing callable row is storage absence, never an effect fact. */
static uint32_t effect_physical_opcode(XrXirOp op) {
    uint32_t mask=0;
    if (op==XR_XIR_FUNCTION_REF) mask|=EFFECT_PHYSICAL_REFERENCE;
    if (op==XR_XIR_GENERATOR_NEW) mask|=EFFECT_PHYSICAL_GENERATOR;
    if (effect_invocation_iterator_operation(op)) mask|=EFFECT_PHYSICAL_ITERATOR_ADVANCE;
    if (op==XR_XIR_RETURN) mask|=EFFECT_PHYSICAL_RETURN;
    if (op==XR_XIR_SLOT_PLACE) mask|=EFFECT_PHYSICAL_SLOT_PLACE;
    if (!effect_physical_error_empty_opcode(op)) mask|=EFFECT_PHYSICAL_ERROR_ESCAPE;
    if (op==XR_XIR_GO) mask|=EFFECT_PHYSICAL_GO;
    switch (op) {
    case XR_XIR_ITERATOR_HAS_NEXT: case XR_XIR_ITERATOR_NEXT: case XR_XIR_ITERATOR_NTH: case XR_XIR_SUSPEND: case XR_XIR_TIMER_AFTER_MS: case XR_XIR_TASK_AWAIT: case XR_XIR_GENERATOR_YIELD:
    case XR_XIR_CHANNEL_SEND: case XR_XIR_CHANNEL_RECV: case XR_XIR_CHANNEL_RECV_OR:
    case XR_XIR_CHANNEL_SEND_TIMEOUT: case XR_XIR_CHANNEL_RECV_TIMEOUT:
    case XR_XIR_GO: case XR_XIR_CALL_REQUIREMENT:
    case XR_XIR_CALL_INDIRECT: case XR_XIR_INVOKE_INDIRECT:
        mask|=EFFECT_PHYSICAL_CONTROL;break;
    default:break;
    }
    switch (op) {
    case XR_XIR_CALL: case XR_XIR_INVOKE: case XR_XIR_CALL_DEFAULT:
    case XR_XIR_INVOKE_DEFAULT: case XR_XIR_CLEANUP_REGISTER:
        mask|=EFFECT_PHYSICAL_DIRECT;break;
    default:break;
    }
    switch (op) {
    case XR_XIR_CELL_NEW: case XR_XIR_CELL_WRITE: case XR_XIR_CELL_STRING_APPEND: case XR_XIR_CELL_LOCAL_WRITE:
    case XR_XIR_PLACE_WRITE: case XR_XIR_SLOT_INIT: case XR_XIR_SLOT_GROUP_INIT:
    case XR_XIR_MAP_NEW: case XR_XIR_SET_NEW: case XR_XIR_MAP_SET: case XR_XIR_SET_ADD:
    case XR_XIR_SLOT_STORE: case XR_XIR_ARRAY_NEW: case XR_XIR_ARRAY_SET:
    case XR_XIR_ARRAY_PUSH: case XR_XIR_STRUCT_NEW: case XR_XIR_STRUCT_SET:
    case XR_XIR_CLASS_NEW: case XR_XIR_CLASS_SET: case XR_XIR_ENUM_NEW:
    case XR_XIR_TUPLE_NEW: case XR_XIR_NULLABLE_SOME: case XR_XIR_GENERATOR_NEW:
        mask|=EFFECT_PHYSICAL_BARRIER;break;
    default:break;
    }
    switch (op) {
    case XR_XIR_CALL: case XR_XIR_CALL_DEFAULT: case XR_XIR_CALL_INDIRECT:
    case XR_XIR_INVOKE_RESULT: case XR_XIR_FUNCTION_REF: case XR_XIR_GENERATOR_NEW: case XR_XIR_CELL_READ:
    case XR_XIR_COPY: case XR_XIR_SCALAR_COPY: case XR_XIR_OWNED_RETAIN:
    case XR_XIR_LOCAL_NEW: case XR_XIR_SCALAR_LOCAL_NEW: case XR_XIR_OWNED_LOCAL_NEW:
    case XR_XIR_LOCAL_READ: case XR_XIR_SCALAR_LOCAL_READ: case XR_XIR_OWNED_LOCAL_READ:
    case XR_XIR_LOCAL_WRITE: case XR_XIR_SCALAR_LOCAL_WRITE: case XR_XIR_OWNED_LOCAL_WRITE:
    case XR_XIR_PHI:
        mask|=EFFECT_PHYSICAL_TRANSFER;break;
    default:break;
    }
    switch (op) {
    case XR_XIR_CALL: case XR_XIR_INVOKE: case XR_XIR_CLEANUP_REGISTER:
    case XR_XIR_CALL_DEFAULT: case XR_XIR_INVOKE_DEFAULT: case XR_XIR_CALL_REQUIREMENT:
    case XR_XIR_GO: case XR_XIR_GENERATOR_NEW: case XR_XIR_CALL_INDIRECT: case XR_XIR_INVOKE_INDIRECT:
    case XR_XIR_FUNCTION_REF: case XR_XIR_ITERATOR_HAS_NEXT: case XR_XIR_ITERATOR_NEXT: case XR_XIR_ITERATOR_NTH:
    case XR_XIR_CELL_READ: case XR_XIR_CELL_WRITE: case XR_XIR_CELL_STRING_APPEND:
    case XR_XIR_CELL_LOCAL_WRITE: case XR_XIR_PLACE_READ: case XR_XIR_PLACE_WRITE:
    case XR_XIR_CELL_PROJECT:
        mask|=EFFECT_PHYSICAL_INVOCATION;break;
    default:break;
    }
    return mask;
}

static void effect_physical_free(EffectPhysicalDomain *domain) {
    if (!domain) return;
    for (uint32_t f=0;f<domain->count;++f) {
        xr_compile_resources_free(domain->functions[f].records);
    }
    xr_compile_resources_free(domain->functions);xr_compile_resources_free(domain);
}

static XrXirStatus effect_physical_record(const XrXirCompileContext *work,
    EffectPhysicalFunction *body,uint32_t value,uint32_t mask) {
    if (!mask || value>=body->values || body->count>=body->values ||
        body->count>body->capacity) return XR_XIR_BAD_STRUCTURE;
    if (body->count==body->capacity) {
        uint32_t capacity=body->capacity?body->capacity:8;
        if (capacity>body->values) capacity=body->values;
        else if (body->capacity) capacity=capacity>body->values/2?body->values:capacity*2;
        if (capacity<=body->count || (uint64_t)capacity>SIZE_MAX/sizeof(*body->records))
            return XR_XIR_BUDGET;
        XrXirStatus status=XR_XIR_OK;
        EffectPhysicalRecord *records=xir_compile_alloc(work,(size_t)capacity*sizeof(*records),&status);
        if (!records) return status;
        if (!xir_compile_work(work,(uint64_t)body->count*sizeof(*records))) {
            xr_compile_resources_free(records);return XR_XIR_BUDGET;
        }
        if (body->count) memcpy(records,body->records,(size_t)body->count*sizeof(*records));
        xr_compile_resources_free(body->records);body->records=records;body->capacity=capacity;
    }
    if (!xir_compile_work(work,sizeof(*body->records))) return XR_XIR_BUDGET;
    body->records[body->count++]=(EffectPhysicalRecord){value,mask};return XR_XIR_OK;
}

/* A complete writer can submit op and type from its real physical read.
 * The row records only ordinal membership, never a ROOT or access fact. */
static XrXirStatus effect_physical_value(const XrXirCompileContext *work,
    const XrXirTypes *types,EffectPhysicalFunction *body,uint32_t value,
    XrXirType type,XrXirOp op) {
    if (!body || value>=body->values || body->parameters>body->values ||
        (uint64_t)body->parameters+body->instructions!=body->values)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t mask=0;
    if (value<body->parameters) {
        if (op!=XR_XIR_INVALID) return XR_XIR_BAD_STRUCTURE;
    } else {
        if (op<=XR_XIR_INVALID || op>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
        mask=effect_physical_opcode(op);
    }
    if (xr_xir_callable_signature(types,type)) mask|=EFFECT_PHYSICAL_CALLABLE;
    if (effect_physical_iterator(types,type)) mask|=EFFECT_PHYSICAL_ITERATOR;
    if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
    if (xr_xir_type_is_class(types,type)) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        mask|=EFFECT_PHYSICAL_CLASS;++body->classes;
    }
    if (mask&EFFECT_PHYSICAL_CALLABLE) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        ++body->rows;
    }
    if(mask&(EFFECT_PHYSICAL_CALLABLE|EFFECT_PHYSICAL_ITERATOR)) {
        if(!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        ++body->origins;
    }
    return mask?effect_physical_record(work,body,value,mask):XR_XIR_OK;
}

static XrXirStatus effect_physical_function(const XrXirCompileContext *work,
    const XrXirModule *module,const XrXirFunction *function,EffectPhysicalFunction *body) {
    uint64_t values=(uint64_t)function->parameter_count+function->instruction_count;
    if (values>UINT32_MAX) return XR_XIR_BUDGET;
    if ((function->parameter_count && !function->parameters) ||
        (function->instruction_count && !function->instructions)) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,3)) return XR_XIR_BUDGET;
    body->parameters=function->parameter_count;body->instructions=function->instruction_count;
    body->values=(uint32_t)values;
    for (uint32_t v=0;v<body->values;++v) {
        if (!xir_compile_work(work,v<body->parameters?2:4)) return XR_XIR_BUDGET;
        XrXirOp op=XR_XIR_INVALID;
        if (v>=body->parameters) {
            op=function->instructions[v-body->parameters].op;
            if (op<=XR_XIR_INVALID || op>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
            XrXirFunctionEffects local={0};
            if (!effect_seed(module,function,&function->instructions[v-body->parameters],&local))
                return XR_XIR_BAD_STRUCTURE;
        }
        XrXirStatus status=effect_physical_value(work,module->types,body,v,
            xr_xir_operand_type(function,v),op);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

/* A fresh domain is made after the current Source refinement and dense
 * physical vector have been assigned. The construction graph may transfer
 * that one storage owner to the invocation certificate in this generation. */
static inline XrXirStatus effect_physical_new(const XrXirCompileContext *work,
    const XrXirModule *module,EffectPhysicalDomain **output) {
    if (!xir_compile_context_valid(work) || !module || !output || *output ||
        (module->function_count && !module->functions)) return XR_XIR_BAD_STRUCTURE;
    if (module->function_count>work->limits.functions ||
        (uint64_t)module->function_count>SIZE_MAX/sizeof(EffectPhysicalFunction)) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    EffectPhysicalDomain *domain=xir_compile_calloc(work,1,sizeof(*domain),&status);
    if (!domain) return status;
    domain->resources=work->resources;
    domain->functions=module->function_count?xir_compile_calloc(work,module->function_count,
        sizeof(*domain->functions),&status):NULL;
    if (module->function_count && !domain->functions) { effect_physical_free(domain);return status; }
    domain->count=module->function_count;
    for (uint32_t f=0;status==XR_XIR_OK && f<domain->count;++f)
        status=effect_physical_function(work,module,&module->functions[f],&domain->functions[f]);
    if (status!=XR_XIR_OK) { effect_physical_free(domain);return status; }
    *output=domain;return XR_XIR_OK;
}
