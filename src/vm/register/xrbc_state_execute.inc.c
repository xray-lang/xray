/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_state_execute.inc.c - Bound State caches and exact scalar transport
 *
 * KEY CONCEPT:
 *   Caches borrow the receiving Layout and State for one execution. Registers
 *   transport scalar bits or owner borrows, never independent owned fields.
 */
static XrtManagedStatus xr_rbc_state_caches(XrtModuleLayout *layout, XrtModuleState *state,
    XrtModuleAccess **out) {
    if (!state || !out || *out) return XRT_MANAGED_ARGUMENT;
    XrtModuleLayoutView view = {0};
    XrtManagedStatus status = xrt_module_layout_view(layout,&view);
    if (status != XRT_MANAGED_OK || !view.slot_count) return status;
    size_t count = view.slot_count;
    if (count > SIZE_MAX / sizeof(XrtModuleAccess) / 2) return XRT_MANAGED_OVERFLOW;
    XrtModuleAccess *accesses = xr_calloc(count * 2,sizeof(*accesses));
    if (!accesses) return XRT_MANAGED_OOM;
    for (size_t slot = 0; slot < count; ++slot) {
        status = xrt_module_cache(state,(uint32_t)slot,&accesses[slot * 2 + 1]);
        if (status != XRT_MANAGED_OK) { xr_free(accesses); return status; }
    }
    *out = accesses;
    return XRT_MANAGED_OK;
}

static XR_AINLINE void xr_rbc_state_ready(const XrtModuleAccess *access, uint32_t source) {
    if ((*access->flags & access->required_flags) != access->required_flags)
        xrt_module_uninitialized(source);
}

/* Exact-width objects also make these transfers independent of host byte
 * order. A signed narrow load extends its value into the register lane. */
static XR_AINLINE bool rbc_state_scalar_load(XrtValueKind kind, const void *payload, uint64_t *bits) {
#define RBC_STATE_LOAD(k,t,convert) case k: { t value; memcpy(&value,payload,sizeof(value)); *bits = (convert); return true; }
    switch (kind) {
    case XRT_VALUE_UNIT: return true;
    RBC_STATE_LOAD(XRT_VALUE_BOOL,uint8_t,(uint64_t)value)
    RBC_STATE_LOAD(XRT_VALUE_I8,int8_t,(uint64_t)(int64_t)value)
    RBC_STATE_LOAD(XRT_VALUE_I16,int16_t,(uint64_t)(int64_t)value)
    RBC_STATE_LOAD(XRT_VALUE_I32,int32_t,(uint64_t)(int64_t)value)
    RBC_STATE_LOAD(XRT_VALUE_U8,uint8_t,(uint64_t)value)
    RBC_STATE_LOAD(XRT_VALUE_U16,uint16_t,(uint64_t)value)
    RBC_STATE_LOAD(XRT_VALUE_U32,uint32_t,(uint64_t)value)
    RBC_STATE_LOAD(XRT_VALUE_F32,uint32_t,(uint64_t)value)
    case XRT_VALUE_RUNE: case XRT_VALUE_I64: case XRT_VALUE_U64: case XRT_VALUE_F64:
        memcpy(bits,payload,sizeof(*bits)); return true;
    default: return false;
    }
#undef RBC_STATE_LOAD
}

static XR_AINLINE bool rbc_state_scalar_store(XrtValueKind kind, uint64_t bits, void *payload) {
    int64_t signed_value;
    memcpy(&signed_value,&bits,sizeof(signed_value));
#define RBC_STATE_STORE(k,t,check,convert) case k: { if (!(check)) return false; \
    t value = (convert); memcpy(payload,&value,sizeof(value)); return true; }
    switch (kind) {
    case XRT_VALUE_UNIT: return true;
    RBC_STATE_STORE(XRT_VALUE_BOOL,uint8_t,bits <= 1,(uint8_t)bits)
    RBC_STATE_STORE(XRT_VALUE_I8,int8_t,signed_value >= INT8_MIN && signed_value <= INT8_MAX,(int8_t)signed_value)
    RBC_STATE_STORE(XRT_VALUE_I16,int16_t,signed_value >= INT16_MIN && signed_value <= INT16_MAX,(int16_t)signed_value)
    RBC_STATE_STORE(XRT_VALUE_I32,int32_t,signed_value >= INT32_MIN && signed_value <= INT32_MAX,(int32_t)signed_value)
    RBC_STATE_STORE(XRT_VALUE_U8,uint8_t,bits <= UINT8_MAX,(uint8_t)bits)
    RBC_STATE_STORE(XRT_VALUE_U16,uint16_t,bits <= UINT16_MAX,(uint16_t)bits)
    RBC_STATE_STORE(XRT_VALUE_U32,uint32_t,bits <= UINT32_MAX,(uint32_t)bits)
    RBC_STATE_STORE(XRT_VALUE_F32,uint32_t,bits <= UINT32_MAX,(uint32_t)bits)
    case XRT_VALUE_RUNE:
        if (bits > 0x10ffff || (bits >= 0xd800 && bits <= 0xdfff)) return false;
        memcpy(payload,&bits,sizeof(bits)); return true;
    case XRT_VALUE_I64: case XRT_VALUE_U64: case XRT_VALUE_F64:
        memcpy(payload,&bits,sizeof(bits)); return true;
    default: return false;
    }
#undef RBC_STATE_STORE
}

static XR_AINLINE XrRbcStatus xr_rbc_state_read(const XrtModuleAccess *access,
    XrRbcRegister *registers, uint32_t word) {
    uint32_t destination = (word >> 8) & 255, kind = word >> 24;
    if ((uint32_t)access->kind != kind ||
        !rbc_state_scalar_load(access->kind,access->payload,&registers[destination].bits)) return XR_RBC_INVALID;
    return XR_RBC_OK;
}

/* Metadata references do not observe publication. Fused reads check readiness
 * at their own source point, and use the same scalar receiver as plain reads. */
static XR_AINLINE XrRbcStatus xr_rbc_state_access(const XrRbcFunction *function,
    XrRbcRegister *registers, XrtModuleAccess *accesses, uint32_t word, uint32_t source,
    size_t *pc) {
    uint32_t op = word & 255, a = (word >> 8) & 255, b = (word >> 16) & 255, c = word >> 24;
    if (op == XR_RBC_STATE_REFERENCE) {
        XrRbcStateAccess row = function->state_accesses[word >> 16];
        XrtModuleAccess *published = &accesses[(size_t)row.slot * 2 + 1];
        registers[a].state_ref = published;
        if (*pc < function->code_count) {
            uint32_t next = function->code[*pc];
            if ((next & 255) == XR_RBC_STATE_READ && ((next >> 16) & 255) == a) {
                uint32_t read_source = function->source_ids ? function->source_ids[*pc] : 0;
                ++*pc;
                xr_rbc_state_ready(published,read_source);
                return xr_rbc_state_read(published,registers,next);
            }
        }
        return XR_RBC_OK;
    }
    XrtModuleAccess *access = registers[op == XR_RBC_STATE_READ ? b : a].state_ref;
    if ((uint32_t)access->kind != c) return XR_RBC_INVALID;
    xr_rbc_state_ready(access,source);
    if (op == XR_RBC_STATE_READ) return xr_rbc_state_read(access,registers,word);
    if (!access->is_mutable) return XR_RBC_INVALID;
    if (c == XRT_VALUE_STRING) {
        /* The register lends an owner kept alive by its actual SSA home. The
         * transactional State kernel retains before changing the binding. */
        XrtManagedHeader *borrow;
        memcpy(&borrow,&registers[b].bits,sizeof(borrow));
        xrt_module_require(xrt_module_replace(access->reference,&borrow),source);
    } else if (!rbc_state_scalar_store(access->kind,registers[b].bits,access->payload)) return XR_RBC_INVALID;
    return XR_RBC_OK;
}

/* The local payload contains only scalar bytes or an owner borrow. Any real
 * reference acquired by construction belongs to State before failure exits. */
static XR_NOINLINE XrRbcStatus xr_rbc_state_step(const XrRbcFunction *function,
    XrRbcRegister *registers, XrtModuleAccess *accesses, uint32_t word, uint32_t source) {
    uint32_t op = word & 255, a = (word >> 8) & 255;
    XrtManagedStatus status;
    if (op == XR_RBC_MODULE_BEGIN || op == XR_RBC_MODULE_READY) {
        status = op == XR_RBC_MODULE_BEGIN ? xrt_module_begin(registers[a].state,word >> 16) :
            xrt_module_ready(registers[a].state,word >> 16);
    } else if (op == XR_RBC_STATE_PUBLISH) {
        status = xrt_module_publish(registers[a].state_ref->reference);
    } else {
        XrRbcStateAccess row = function->state_accesses[word >> 16];
        XrtModuleAccess *published = &accesses[(size_t)row.slot * 2 + 1];
        XrtModuleState *state = registers[op == XR_RBC_STATE_INITIALIZE ? a : row.state].state;
        union { uint64_t bits; XrtManagedHeader *borrow; } payload = {0};
        const void *value = &payload;
        if (published->kind == XRT_VALUE_UNIT) value = NULL;
        else if (published->kind == XRT_VALUE_STRING)
            memcpy(&payload.borrow,&registers[row.value].bits,sizeof(payload.borrow));
        else if (!rbc_state_scalar_store(published->kind,registers[row.value].bits,&payload)) return XR_RBC_INVALID;
        if (op == XR_RBC_STATE_INITIALIZE) status = xrt_module_initialize(state,row.slot,value);
        else {
            XrtModuleRef *reference = NULL;
            status = xrt_module_construct(state,row.slot,value,&reference);
            if (status == XRT_MANAGED_OK) {
                XrtModuleAccess *initial = &accesses[(size_t)row.slot * 2];
                status = xrt_module_reference_cache(reference,initial);
                if (status == XRT_MANAGED_OK) registers[a].state_ref = initial;
            }
        }
    }
    xrt_module_require(status,source);
    return XR_RBC_OK;
}
