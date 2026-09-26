/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_checked.c - Bounded canonical Checked serialization and re-admission
 *
 * KEY CONCEPT:
 *   One field traversal owns the wire schema; decoding publishes only after
 *   independent semantic verification of fully owned data.
 */
#include "xxir_checked.h"
#include "xxir_internal.h"
#include "../base/xmalloc.h"
#include "../base/xsha256.h"

typedef struct CheckedCursor {
    const uint8_t *input;
    uint8_t *output;
    size_t position, capacity;
    uint64_t allocated;
    XrXirBudget remaining;
    XrXirStatus status;
    bool reading;
} CheckedCursor;

static bool checked_room(CheckedCursor *c, size_t size) {
    if (c->status != XR_XIR_OK) return false;
    if (size > c->capacity - c->position) {
        c->status = c->reading ? XR_XIR_BAD_STRUCTURE : XR_XIR_BUDGET;
        return false;
    }
    return true;
}
static uint64_t checked_integer(CheckedCursor *c, uint64_t value, unsigned width) {
    if (!checked_room(c, width)) return 0;
    if (c->reading) value = 0;
    for (unsigned i = 0; i < width; ++i) {
        if (c->reading) value |= (uint64_t) c->input[c->position + i] << (8 * i);
        else if (c->output) c->output[c->position + i] = (uint8_t) (value >> (8 * i));
    }
    c->position += width;
    return value;
}
static uint32_t checked_u32(CheckedCursor *c, uint32_t value) {
    return (uint32_t) checked_integer(c, value, 4);
}
static uint32_t checked_count(CheckedCursor *c, uint32_t value, uint32_t *remaining) {
    uint32_t count = checked_u32(c, value);
    if (c->status == XR_XIR_OK && count > *remaining) c->status = XR_XIR_BUDGET;
    if (c->status != XR_XIR_OK) return 0;
    *remaining -= count;
    return count;
}
static void *checked_array(CheckedCursor *c, const void *source, uint32_t count,
                           size_t size, size_t wire_minimum) {
    if (c->status != XR_XIR_OK || !count) return NULL;
    if (!c->reading) return (void *) source;
    if (count > (c->capacity - c->position) / wire_minimum) {
        c->status = XR_XIR_BAD_STRUCTURE; return NULL;
    }
    if (count > SIZE_MAX / size || (uint64_t) count * size > c->remaining.metadata_bytes - c->allocated) {
        c->status = XR_XIR_BUDGET; return NULL;
    }
    void *memory = xr_calloc(count, size);
    if (!memory) { c->status = XR_XIR_OUT_OF_MEMORY; return NULL; }
    c->allocated += (uint64_t) count * size;
    return memory;
}
static const char *checked_blob(CheckedCursor *c, const char *bytes, uint32_t *length) {
    *length = checked_u32(c, *length);
    if (!checked_room(c, *length)) return NULL;
    if (c->reading) {
        char *copy = checked_array(c, NULL, *length, 1, 1);
        if (*length && !copy) return NULL;
        if (*length) memcpy(copy, c->input + c->position, *length);
        bytes = copy;
    } else if (c->output && *length) memcpy(c->output + c->position, bytes, *length);
    c->position += *length;
    return bytes;
}
static void checked_function(CheckedCursor *c, XrXirFunction *f) {
    f->name = checked_blob(c, f->name, &f->name_length);
    f->parameter_count = checked_count(c, f->parameter_count, &c->remaining.parameters);
    XrXirType *parameters = checked_array(c, f->parameters, f->parameter_count, sizeof(*parameters), 4);
    f->parameters = parameters;
    for (uint32_t i = 0; i < f->parameter_count && c->status == XR_XIR_OK; ++i) {
        uint32_t type = checked_u32(c, (uint32_t) parameters[i]);
        if (c->reading) parameters[i] = (XrXirType) type;
    }
    f->result = (XrXirType) checked_u32(c, (uint32_t) f->result);
    f->block_count = checked_count(c, f->block_count, &c->remaining.blocks);
    XrXirBlock *blocks = checked_array(c, f->blocks, f->block_count, sizeof(*blocks), 8);
    f->blocks = blocks;
    for (uint32_t i = 0; i < f->block_count && c->status == XR_XIR_OK; ++i) {
        XrXirBlock b = blocks[i];
        b.first = checked_u32(c, b.first); b.count = checked_u32(c, b.count);
        if (c->reading) blocks[i] = b;
    }
    f->instruction_count = checked_count(c, f->instruction_count, &c->remaining.instructions);
    XrXirInstruction *instructions = checked_array(c, f->instructions, f->instruction_count, sizeof(*instructions), 32);
    f->instructions = instructions;
    for (uint32_t i = 0; i < f->instruction_count && c->status == XR_XIR_OK; ++i) {
        XrXirInstruction in = instructions[i];
        in.op = (XrXirOp) checked_u32(c, (uint32_t) in.op);
        in.type = (XrXirType) checked_u32(c, (uint32_t) in.type);
        for (unsigned j = 0; j < 2; ++j) in.args[j] = checked_u32(c, in.args[j]);
        for (unsigned j = 0; j < 2; ++j) in.targets[j] = checked_u32(c, in.targets[j]);
        uint64_t bits = checked_integer(c, (uint64_t) in.immediate, 8);
        in.immediate = bits <= INT64_MAX ? (int64_t) bits : -1 - (int64_t) (UINT64_MAX - bits);
        if (c->reading) instructions[i] = in;
    }
    f->operand_count = checked_u32(c, f->operand_count);
    uint32_t *operands = checked_array(c, f->operands, f->operand_count, sizeof(*operands), 4);
    f->operands = operands;
    for (uint32_t i = 0; i < f->operand_count && c->status == XR_XIR_OK; ++i) {
        uint32_t value = checked_u32(c, operands[i]);
        if (c->reading) operands[i] = value;
    }
}
static void checked_declarations(CheckedCursor *c, XrXirDeclarations *d, uint32_t functions) {
    d->module_count = checked_u32(c, d->module_count);
    d->slot_count = checked_u32(c, d->slot_count);
    d->literal_count = checked_u32(c, d->literal_count);
    d->root_module = checked_u32(c, d->root_module);
    d->entry_function = checked_u32(c, d->entry_function);
    XrXirSourceModule *modules = checked_array(c, d->modules, d->module_count, sizeof(*modules), 12);
    d->modules = modules;
    for (uint32_t i = 0; i < d->module_count && c->status == XR_XIR_OK; ++i) {
        XrXirSourceModule m = modules[i];
        m.name = checked_blob(c, m.name, &m.name_length);
        m.dependency_count = checked_u32(c, m.dependency_count);
        uint32_t *dependencies = checked_array(c, m.dependencies, m.dependency_count, sizeof(*dependencies), 4);
        m.dependencies = dependencies;
        for (uint32_t j = 0; j < m.dependency_count && c->status == XR_XIR_OK; ++j) {
            uint32_t id = checked_u32(c, dependencies[j]);
            if (c->reading) dependencies[j] = id;
        }
        m.initializer = checked_u32(c, m.initializer);
        if (c->reading) modules[i] = m;
    }
    XrXirFunctionIdentity *identities = checked_array(c, d->functions, functions, sizeof(*identities), 8);
    d->functions = identities;
    for (uint32_t i = 0; i < functions && c->status == XR_XIR_OK; ++i) {
        XrXirFunctionIdentity id = identities[i];
        id.module = checked_u32(c, id.module); id.exported = checked_u32(c, id.exported);
        if (c->reading) identities[i] = id;
    }
    XrXirSlot *slots = checked_array(c, d->slots, d->slot_count, sizeof(*slots), 12);
    d->slots = slots;
    for (uint32_t i = 0; i < d->slot_count && c->status == XR_XIR_OK; ++i) {
        XrXirSlot slot = slots[i];
        slot.module = checked_u32(c, slot.module);
        slot.type = (XrXirType) checked_u32(c, (uint32_t) slot.type);
        slot.mutable = checked_u32(c, slot.mutable);
        if (c->reading) slots[i] = slot;
    }
    XrXirLiteral *literals = checked_array(c, d->literals, d->literal_count, sizeof(*literals), 4);
    d->literals = literals;
    for (uint32_t i = 0; i < d->literal_count && c->status == XR_XIR_OK; ++i) {
        XrXirLiteral literal = literals[i];
        literal.bytes = checked_blob(c, literal.bytes, &literal.length);
        if (c->reading) literals[i] = literal;
    }
}
static void checked_generics(CheckedCursor *c, XrXirModule *m) {
    uint32_t present = checked_u32(c, m->generics ? 1u : 0u);
    if (c->status == XR_XIR_OK && present > 1) c->status = XR_XIR_BAD_STRUCTURE;
    if (!present || c->status != XR_XIR_OK) return;
    XrXirGeneric *generics = checked_array(c, m->generics, m->function_count, sizeof(*generics), 8);
    m->generics = generics;
    for (uint32_t f = 0; f < m->function_count && c->status == XR_XIR_OK; ++f) {
        XrXirGeneric g = generics[f];
        g.parameter_count = checked_count(c, g.parameter_count, &c->remaining.parameters);
        uint32_t *constraints = checked_array(c, g.constraints, g.parameter_count, sizeof(*constraints), 4);
        g.constraints = constraints;
        for (uint32_t p = 0; p < g.parameter_count && c->status == XR_XIR_OK; ++p) {
            uint32_t value = checked_u32(c, constraints[p]);
            if (c->reading) constraints[p] = value;
        }
        g.argument_count = checked_u32(c, g.argument_count);
        XrXirType *arguments = checked_array(c, g.arguments, g.argument_count, sizeof(*arguments), 4);
        g.arguments = arguments;
        for (uint32_t a = 0; a < g.argument_count && c->status == XR_XIR_OK; ++a) {
            XrXirType type = (XrXirType) checked_u32(c, (uint32_t) arguments[a]);
            if (c->reading) arguments[a] = type;
        }
        if (c->reading) generics[f] = g;
    }
}
static void checked_module(CheckedCursor *c, XrXirModule *m) {
    uint32_t count = checked_count(c, m->function_count, &c->remaining.functions);
    uint32_t declarations = checked_u32(c, m->declarations ? 1u : 0u);
    if (c->status == XR_XIR_OK && declarations > 1) c->status = XR_XIR_BAD_STRUCTURE;
    XrXirFunction *functions = checked_array(c, m->functions, count, sizeof(*functions), 24);
    m->functions = functions; m->function_count = functions ? count : 0;
    for (uint32_t i = 0; i < m->function_count && c->status == XR_XIR_OK; ++i) {
        XrXirFunction f = functions[i];
        checked_function(c, &f);
        if (c->reading) functions[i] = f;
    }
    if (c->status != XR_XIR_OK) return;
    if (declarations) {
        XrXirDeclarations *owned = checked_array(c, m->declarations, 1, sizeof(*owned), 20);
        m->declarations = owned;
        if (!owned) return;
        XrXirDeclarations d = *owned;
        checked_declarations(c, &d, count);
        if (c->reading) *owned = d;
    }
    checked_generics(c, m);
}
static void checked_digest(const uint8_t *bytes, size_t size, uint8_t digest[32]) {
    XrSHA256Context sha;
    xr_sha256_init(&sha);
    xr_sha256_update(&sha, bytes, 32);
    xr_sha256_update(&sha, bytes + 64, size - 64);
    xr_sha256_final(&sha, digest);
}
static XrXirStatus checked_error(XrXirStatus status, XrXirDiagnostic *diagnostic) {
    if (diagnostic) *diagnostic = (XrXirDiagnostic) {status, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    return status;
}
static size_t checked_capacity(const XrXirBudget *budget) {
    uint64_t limit = budget->metadata_bytes;
    if (limit > budget->work / 4) limit = budget->work / 4;
    return limit > SIZE_MAX ? SIZE_MAX : (size_t) limit;
}
void xr_xir_checked_packet_free(XrXirCheckedPacket *packet) {
    if (packet) { xr_free(packet->bytes); *packet = (XrXirCheckedPacket) {0}; }
}
XrXirStatus xr_xir_checked_write(const XrXirArtifact *artifact,
    const XrXirBudget *budget, XrXirCheckedPacket *output, XrXirDiagnostic *diagnostic) {
    if (!output) return checked_error(XR_XIR_BAD_STRUCTURE, diagnostic);
    *output = (XrXirCheckedPacket) {0};
    if (!artifact || artifact->module.stage != XR_XIR_CHECKED) return checked_error(XR_XIR_BAD_STAGE, diagnostic);
    XrXirBudget limits = budget ? *budget : xr_xir_default_budget();
    XrXirStatus status = xr_xir_artifact_verify(artifact, &limits, diagnostic);
    if (status != XR_XIR_OK) return status;
    size_t capacity = checked_capacity(&limits);
    if (capacity < 64) return checked_error(XR_XIR_BUDGET, diagnostic);
    CheckedCursor c = {NULL, NULL, 64, capacity, 0, limits, XR_XIR_OK, false};
    XrXirModule module = artifact->module;
    checked_module(&c, &module);
    if (c.status != XR_XIR_OK) return checked_error(c.status, diagnostic);
    size_t size = c.position;
    uint8_t *bytes = xr_calloc(size, 1);
    if (!bytes) return checked_error(XR_XIR_OUT_OF_MEMORY, diagnostic);
    memcpy(bytes, "XRCHK\0\0\0", 8);
    c = (CheckedCursor) {NULL, bytes, 8, size, 0, limits, XR_XIR_OK, false};
    checked_u32(&c, XR_XIR_CHECKED_SCHEMA); checked_u32(&c, XR_XIR_CHECKED_CONTRACT);
    checked_u32(&c, XR_XIR_CHECKED); checked_u32(&c, 0);
    checked_integer(&c, size - 64, 8);
    c.position = 64; module = artifact->module;
    checked_module(&c, &module);
    if (c.status != XR_XIR_OK || c.position != size) {
        xr_free(bytes); return checked_error(XR_XIR_BAD_STRUCTURE, diagnostic);
    }
    checked_digest(bytes, size, bytes + 32);
    *output = (XrXirCheckedPacket) {bytes, size};
    return XR_XIR_OK;
}
XrXirStatus xr_xir_checked_read(const void *bytes, size_t length,
    const XrXirBudget *budget, XrXirArtifact **output, XrXirDiagnostic *diagnostic) {
    if (!output) return checked_error(XR_XIR_BAD_STRUCTURE, diagnostic);
    *output = NULL;
    XrXirBudget limits = budget ? *budget : xr_xir_default_budget();
    if (!bytes || length < 64) return checked_error(XR_XIR_BAD_STRUCTURE, diagnostic);
    if (length > checked_capacity(&limits)) return checked_error(XR_XIR_BUDGET, diagnostic);
    if (memcmp(bytes, "XRCHK\0\0\0", 8)) return checked_error(XR_XIR_BAD_STRUCTURE, diagnostic);
    CheckedCursor c = {bytes, NULL, 8, length, 0, limits, XR_XIR_OK, true};
    uint32_t schema = checked_u32(&c, 0), contract = checked_u32(&c, 0);
    uint32_t stage = checked_u32(&c, 0), reserved = checked_u32(&c, 0);
    uint64_t payload = checked_integer(&c, 0, 8);
    if (schema != XR_XIR_CHECKED_SCHEMA || contract != XR_XIR_CHECKED_CONTRACT || reserved || payload != length - 64)
        return checked_error(XR_XIR_BAD_STRUCTURE, diagnostic);
    if (stage != XR_XIR_CHECKED) return checked_error(XR_XIR_BAD_STAGE, diagnostic);
    uint8_t digest[32]; checked_digest(bytes, length, digest);
    if (memcmp(digest, (const uint8_t *) bytes + 32, 32)) return checked_error(XR_XIR_BAD_STRUCTURE, diagnostic);
    if (sizeof(XrXirArtifact) > limits.metadata_bytes) return checked_error(XR_XIR_BUDGET, diagnostic);
    XrXirArtifact *artifact = xr_calloc(1, sizeof(*artifact));
    if (!artifact) return checked_error(XR_XIR_OUT_OF_MEMORY, diagnostic);
    artifact->module.stage = XR_XIR_CHECKED; artifact->budget = limits;
    c.position = 64; c.allocated = sizeof(*artifact);
    checked_module(&c, &artifact->module);
    if (c.status == XR_XIR_OK && c.position != length) c.status = XR_XIR_BAD_STRUCTURE;
    if (c.status != XR_XIR_OK) checked_error(c.status, diagnostic);
    else c.status = xr_xir_artifact_verify(artifact, &limits, diagnostic);
    if (c.status != XR_XIR_OK) { xr_xir_artifact_free(artifact); return c.status; }
    *output = artifact;
    return XR_XIR_OK;
}
