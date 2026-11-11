/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_state_execute.inc.c - Prepared metadata for typed module accesses
 *
 * KEY CONCEPT:
 *   Metadata is prepared without granting publication. A reference checks its
 *   flags at its actual instruction and borrows the execution's stable cache.
 */
static XrXirValueStatus xr_rbc_state_caches(XrtModuleLayout *layout, XrtModuleState *state,
    XrtModuleAccess **out) {
    XrtModuleLayoutSpec view = {0};
    XrXirValueStatus status = xrt_module_layout_view(layout, &view);
    if (status != XR_XIR_VALUE_OK || !view.slot_count) return status;
    size_t count = view.slot_count;
    if (count > SIZE_MAX / sizeof(XrtModuleAccess) / 2) return XR_XIR_VALUE_OOM;
    XrtModuleAccess *accesses = xr_calloc(count * 2, sizeof(*accesses));
    if (!accesses) return XR_XIR_VALUE_OOM;
    for (size_t slot = 0; slot < count; ++slot) {
        status = xrt_module_cache(state, (uint32_t)slot, &accesses[slot * 2 + 1]);
        if (status != XR_XIR_VALUE_OK) { xr_free(accesses); return status; }
    }
    *out = accesses;
    return XR_XIR_VALUE_OK;
}

static XR_AINLINE void xr_rbc_state_ready(const XrtModuleAccess *access, uint32_t source) {
    if ((*access->flags & access->required_flags) != access->required_flags)
        xrt_module_uninitialized(source);
}

static XR_AINLINE void xr_rbc_state_read(const XrtModuleAccess *access,
    XrRbcRegister *registers, uint32_t word, uint32_t source) {
    uint32_t destination = (word >> 8) & 255, kind = word >> 24;
    if (kind == XRT_MODULE_STRING) {
        XrtString *string;
        memcpy(&string, access->payload, sizeof(string));
        registers[destination].string = xrt_string_retain(string, source);
    } else if (kind == XRT_MODULE_BOOL) {
        bool boolean;
        memcpy(&boolean, access->payload, sizeof(boolean));
        registers[destination].bits = boolean;
    } else if (kind != XRT_MODULE_UNIT) {
        memcpy(&registers[destination].bits, access->payload, sizeof(uint64_t));
    }
}

/* Accesses borrow stable execution metadata and cannot grow a register bank.
 * Inline them so each scalar load avoids publishing the interpreter's frame. */
static XR_AINLINE XrRbcStatus xr_rbc_state_access(const XrRbcFunction *function,
    XrRbcRegister *registers, XrtModuleAccess *accesses, uint32_t word, uint32_t source,
    size_t *pc) {
    uint32_t op = word & 255, a = (word >> 8) & 255, b = (word >> 16) & 255, c = word >> 24;
    if (op == XR_RBC_STATE_REFERENCE) {
        XrRbcStateAccess row = function->state_accesses[word >> 16];
        XrtModuleAccess *published = &accesses[(size_t)row.slot * 2 + 1];
        xr_rbc_state_ready(published, source);
        registers[a].state_ref = published;
        /* An adjacent read observes the same publication. Preserve the reference
         * producer and the read's source identity while consuming both words. */
        if (*pc < function->code_count) {
            uint32_t next = function->code[*pc];
            if ((next & 255) == XR_RBC_STATE_READ && ((next >> 16) & 255) == a) {
                uint32_t read_source = function->source_ids ? function->source_ids[*pc] : 0;
                ++*pc;
                xr_rbc_state_read(published, registers, next, read_source);
            }
        }
        return XR_RBC_OK;
    }
    XrtModuleAccess *access = registers[op == XR_RBC_STATE_READ ? b : a].state_ref;
    xr_rbc_state_ready(access, source);
    if (op == XR_RBC_STATE_READ) {
        xr_rbc_state_read(access, registers, word, source);
    } else if (c == XRT_MODULE_STRING) {
        XrXirValueStatus status = xrt_module_replace(access->reference, &registers[b].string);
        if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
    } else if (c == XRT_MODULE_BOOL) {
        if (registers[b].bits > 1) return XR_RBC_INVALID;
        bool boolean = registers[b].bits != 0;
        memcpy(access->payload, &boolean, sizeof(boolean));
    } else if (c != XRT_MODULE_UNIT) {
        memcpy(access->payload, &registers[b].bits, sizeof(uint64_t));
    }
    return XR_RBC_OK;
}

/* Initialization and publication use runtime ownership leaves. Keep their
 * calls outside the scalar dispatch function's register allocation. */
static XR_NOINLINE XrRbcStatus xr_rbc_state_step(const XrRbcFunction *function,
    XrRbcRegister *registers, XrtModuleAccess *accesses, uint32_t word, uint32_t source) {
    uint32_t op = word & 255, a = (word >> 8) & 255;
    XrXirValueStatus status;
    if (op == XR_RBC_MODULE_BEGIN || op == XR_RBC_MODULE_READY) {
        status = op == XR_RBC_MODULE_BEGIN ? xrt_module_begin(registers[a].state, word >> 16) :
            xrt_module_ready(registers[a].state, word >> 16);
    } else if (op == XR_RBC_STATE_PUBLISH) {
        status = xrt_module_publish(registers[a].state_ref->reference);
    } else {
        XrRbcStateAccess row = function->state_accesses[word >> 16];
        XrtModuleAccess *published = &accesses[(size_t)row.slot * 2 + 1];
        XrtModuleState *state = registers[op == XR_RBC_STATE_INITIALIZE ? a : row.state].state;
        const void *value = &registers[row.value];
        bool boolean = false;
        if (published->kind == XRT_MODULE_UNIT) value = NULL;
        else if (published->kind == XRT_MODULE_BOOL) {
            if (registers[row.value].bits > 1) return XR_RBC_INVALID;
            boolean = registers[row.value].bits != 0;
            value = &boolean;
        }
        if (op == XR_RBC_STATE_INITIALIZE) status = xrt_module_initialize(state, row.slot, value);
        else {
            XrtModuleRef *reference = NULL;
            status = xrt_module_construct(state, row.slot, value, &reference);
            if (status == XR_XIR_VALUE_OK) {
                XrtModuleAccess *initial = &accesses[(size_t)row.slot * 2];
                status = xrt_module_reference_cache(reference, initial);
                if (status == XR_XIR_VALUE_OK) registers[a].state_ref = initial;
            }
        }
    }
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
    return XR_RBC_OK;
}
