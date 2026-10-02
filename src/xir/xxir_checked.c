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
#include "xxir_nominal.h"
#include "xxir_interface.h"
#include "xxir_implementation.h"
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
/* Decode work is cumulative; writer sizing uses its separate capacity contract. */
static bool checked_read_work(CheckedCursor *c, size_t bytes) {
    if (c->status != XR_XIR_OK) return false;
    if (!c->reading) return true;
    if (bytes > c->remaining.work) { c->status = XR_XIR_BUDGET; return false; }
    c->remaining.work -= bytes;
    return true;
}
static uint64_t checked_integer(CheckedCursor *c, uint64_t value, unsigned width) {
    if (!checked_room(c, width) || !checked_read_work(c,width)) return 0;
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
    if (!checked_room(c, *length) || !checked_read_work(c,*length)) return NULL;
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
    XrXirBlock *blocks = checked_array(c, f->blocks, f->block_count, sizeof(*blocks), 16);
    f->blocks = blocks;
    for (uint32_t i = 0; i < f->block_count && c->status == XR_XIR_OK; ++i) {
        XrXirBlock b = blocks[i];
        b.first = checked_u32(c, b.first); b.count = checked_u32(c, b.count);
        b.panic = checked_u32(c, b.panic);
        b.frontier = checked_u32(c, b.frontier);
        if (c->reading) blocks[i] = b;
    }
    f->instruction_count = checked_count(c, f->instruction_count, &c->remaining.instructions);
    XrXirInstruction *instructions = checked_array(c, f->instructions, f->instruction_count, sizeof(*instructions), 40);
    f->instructions = instructions;
    for (uint32_t i = 0; i < f->instruction_count && c->status == XR_XIR_OK; ++i) {
        XrXirInstruction in = instructions[i];
        in.op = (XrXirOp) checked_u32(c, (uint32_t) in.op);
        in.type = (XrXirType) checked_u32(c, (uint32_t) in.type);
        for (unsigned j = 0; j < 2; ++j) in.args[j] = checked_u32(c, in.args[j]);
        for (unsigned j = 0; j < 2; ++j) in.targets[j] = checked_u32(c, in.targets[j]);
        uint64_t bits = checked_integer(c, (uint64_t) in.immediate, 8);
        in.immediate = bits <= INT64_MAX ? (int64_t) bits : -1 - (int64_t) (UINT64_MAX - bits);
        for (unsigned j = 0; j < 2; ++j) in.type_arguments[j] = checked_u32(c, in.type_arguments[j]);
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
static void checked_application(CheckedCursor *c, XrXirInterfaceApplication *app) {
    app->declaration = checked_u32(c, app->declaration);
    uint32_t count = checked_u32(c, app->argument_count);
    XrXirType *arguments = checked_array(c, app->arguments, count, sizeof(*arguments), 4);
    app->arguments = arguments; app->argument_count = arguments ? count : 0;
    for (uint32_t a = 0; a < app->argument_count && c->status == XR_XIR_OK; ++a) {
        XrXirType type = (XrXirType)checked_u32(c, (uint32_t)arguments[a]);
        if (c->reading) arguments[a] = type;
    }
}
static void checked_implementations(CheckedCursor *c, XrXirDeclarations *d) {
    uint32_t count = checked_u32(c, d->implementations ? d->implementations->count : 0);
    if (!count || c->status != XR_XIR_OK) return;
    XrXirImplementationTable *table = checked_array(c, d->implementations, 1, sizeof(*table), 16);
    if (c->reading) d->implementations = table;
    if (!table) return;
    XrXirImplementation *records = checked_array(c, table->records, count, sizeof(*records), 16);
    if (c->reading) { table->records = records; table->count = records ? count : 0; }
    for (uint32_t i = 0; records && i < count && c->status == XR_XIR_OK; ++i) {
        XrXirImplementation record = records[i];
        record.nominal_declaration = checked_u32(c, record.nominal_declaration);
        checked_application(c, &record.interface);
        uint32_t bindings_count = checked_u32(c, record.binding_count);
        XrXirImplementationBinding *bindings = checked_array(c, record.bindings,
            bindings_count, sizeof(*bindings), 16);
        record.bindings = bindings; record.binding_count = bindings ? bindings_count : 0;
        for (uint32_t b = 0; b < record.binding_count && c->status == XR_XIR_OK; ++b) {
            XrXirImplementationBinding binding = bindings[b];
            checked_application(c, &binding.requirement);
            binding.member = checked_u32(c, binding.member);
            binding.function = checked_u32(c, binding.function);
            if (c->reading) bindings[b] = binding;
        }
        if (c->reading) records[i] = record;
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
    XrXirFunctionIdentity *identities = checked_array(c, d->functions, functions, sizeof(*identities), 28);
    d->functions = identities;
    for (uint32_t i = 0; i < functions && c->status == XR_XIR_OK; ++i) {
        XrXirFunctionIdentity id = identities[i];
        id.module = checked_u32(c, id.module); id.exported = checked_u32(c, id.exported);
        id.nominal_owner = checked_u32(c, id.nominal_owner);
        id.member_access = checked_u32(c, id.member_access);
        id.cleanup_owner = checked_u32(c, id.cleanup_owner);
        id.promises = checked_u32(c, id.promises);
        id.method_kind = checked_u32(c, id.method_kind);
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
    checked_implementations(c, d);
}
static XrXirInterfaceApplication *checked_applications(CheckedCursor *c,
    const XrXirInterfaceApplication *source, uint32_t *count) {
    uint32_t size = checked_u32(c, *count);
    XrXirInterfaceApplication *applications = checked_array(c, source, size, sizeof(*applications), 8);
    *count = applications ? size : 0;
    for (uint32_t i = 0; i < *count && c->status == XR_XIR_OK; ++i) {
        XrXirInterfaceApplication app = applications[i];
        checked_application(c, &app);
        if (c->reading) applications[i] = app;
    }
    return applications;
}
static XrXirConstraint *checked_constraints(CheckedCursor *c,
    const XrXirConstraint *source, uint32_t count) {
    XrXirConstraint *constraints = checked_array(c, source, count, sizeof(*constraints), 8);
    for (uint32_t p = 0; constraints && p < count && c->status == XR_XIR_OK; ++p) {
        XrXirConstraint constraint = constraints[p];
        constraint.markers = checked_u32(c, constraint.markers);
        constraint.interfaces = checked_applications(c, constraint.interfaces, &constraint.interface_count);
        if (c->reading) constraints[p] = constraint;
    }
    return constraints;
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
        uint32_t kinds_present = checked_u32(c, g.parameter_kinds ? 1u : 0u);
        if (kinds_present > 1 || (kinds_present && !g.parameter_count)) {
            c->status = XR_XIR_BAD_STRUCTURE; break;
        }
        uint32_t *kinds = kinds_present ? checked_array(c, g.parameter_kinds,
            g.parameter_count, sizeof(*kinds), 4) : NULL;
        bool result_variable = false;
        for (uint32_t p = 0; kinds && p < g.parameter_count && c->status == XR_XIR_OK; ++p) {
            uint32_t kind = checked_u32(c, kinds[p]);
            if (kind > XR_XIR_BINDER_RESULT_VARIABLE) c->status = XR_XIR_BAD_STRUCTURE;
            result_variable |= kind == XR_XIR_BINDER_RESULT_VARIABLE;
            if (c->reading) kinds[p] = kind;
        }
        if (c->status == XR_XIR_OK && kinds_present && !result_variable) c->status = XR_XIR_BAD_STRUCTURE;
        g.parameter_kinds = kinds;
        g.constraints = checked_constraints(c, g.constraints, g.parameter_count);
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
static XrXirLiteral checked_nominal_name(CheckedCursor *c, XrXirLiteral name) {
    name.length = checked_u32(c, name.length);
    if (!checked_room(c, name.length)) return (XrXirLiteral) {0};
    if (c->reading) {
        if (name.length == UINT32_MAX) { c->status = XR_XIR_BUDGET; return (XrXirLiteral) {0}; }
        char *bytes = checked_array(c, NULL, name.length + 1, 1, 1);
        if (!bytes) return (XrXirLiteral) {0};
        memcpy(bytes, c->input + c->position, name.length);
        name.bytes = bytes;
    } else if (c->output && name.length) memcpy(c->output + c->position, name.bytes, name.length);
    c->position += name.length;
    return name;
}
static void checked_nominals(CheckedCursor *c, XrXirTypes *types, uint32_t count) {
    if (!count || c->status != XR_XIR_OK) return;
    XrXirNominalTable *table = checked_array(c, types->nominals, 1, sizeof(*table), 20);
    if (c->reading) types->nominals = table;
    if (!table) return;
    XrXirNominalDeclaration *declarations = checked_array(c, table->declarations, count, sizeof(*declarations), 20);
    if (c->reading) { table->declarations = declarations; table->count = declarations ? count : 0; }
    if (!declarations) return;
    for (uint32_t i = 0; i < count && c->status == XR_XIR_OK; ++i) {
        XrXirNominalDeclaration d = declarations[i];
        d.module = checked_nominal_name(c, d.module);
        d.name = checked_nominal_name(c, d.name);
        d.exported = checked_u32(c, d.exported);
        d.kind = checked_u32(c, d.kind);
        d.flags = checked_u32(c, d.flags);
        uint32_t parameters = checked_count(c, d.parameter_count, &c->remaining.parameters);
        XrXirConstraint *constraints = checked_constraints(c, d.constraints, parameters);
        d.constraints = constraints; d.parameter_count = constraints ? parameters : 0;
        uint32_t fields = checked_u32(c, d.field_count);
        XrXirNominalField *members = checked_array(c, d.fields, fields, sizeof(*members), 12);
        d.fields = members; d.field_count = members ? fields : 0;
        for (uint32_t j = 0; j < d.field_count && c->status == XR_XIR_OK; ++j) {
            XrXirNominalField field = members[j];
            field.name = checked_nominal_name(c, field.name);
            field.type = (XrXirType) checked_u32(c, (uint32_t) field.type);
            field.flags = checked_u32(c, field.flags);
            if (c->reading) members[j] = field;
        }
        uint32_t variants = checked_u32(c, d.variant_count);
        XrXirNominalVariant *cases = checked_array(c, d.variants, variants, sizeof(*cases), 12);
        d.variants = cases; d.variant_count = cases ? variants : 0;
        for (uint32_t j = 0; j < d.variant_count && c->status == XR_XIR_OK; ++j) {
            XrXirNominalVariant variant = cases[j];
            variant.name = checked_nominal_name(c, variant.name);
            variant.field_begin = checked_u32(c, variant.field_begin);
            variant.field_count = checked_u32(c, variant.field_count);
            if (c->reading) cases[j] = variant;
        }
        if (c->reading) declarations[i] = d;
    }
}
static void checked_interface_parents(CheckedCursor *c, XrXirInterfaceDeclaration *d) {
    d->parents = checked_applications(c, d->parents, &d->parent_count);
}
static void checked_interfaces(CheckedCursor *c, XrXirTypes *types, uint32_t count) {
    if (!count || c->status != XR_XIR_OK) return;
    XrXirInterfaceTable *table = checked_array(c, types->interfaces, 1, sizeof(*table), 24);
    if (c->reading) types->interfaces = table;
    if (!table) return;
    XrXirInterfaceDeclaration *declarations = checked_array(c, table->declarations, count, sizeof(*declarations), 24);
    if (c->reading) { table->declarations = declarations; table->count = declarations ? count : 0; }
    if (!declarations) return;
    for (uint32_t i = 0; i < count && c->status == XR_XIR_OK; ++i) {
        XrXirInterfaceDeclaration d = declarations[i];
        d.module = checked_nominal_name(c, d.module);
        d.name = checked_nominal_name(c, d.name);
        d.exported = checked_u32(c, d.exported);
        uint32_t parameters = checked_count(c, d.parameter_count, &c->remaining.parameters);
        XrXirConstraint *constraints = checked_constraints(c, d.constraints, parameters);
        d.constraints = constraints; d.parameter_count = constraints ? parameters : 0;
        checked_interface_parents(c, &d);
        uint32_t count_methods = checked_u32(c, d.method_count);
        XrXirInterfaceMethod *methods = checked_array(c, d.methods, count_methods, sizeof(*methods), 16);
        d.methods = methods; d.method_count = methods ? count_methods : 0;
        for (uint32_t j = 0; j < d.method_count && c->status == XR_XIR_OK; ++j) {
            XrXirInterfaceMethod method = methods[j];
            method.name = checked_nominal_name(c, method.name);
            method.signature = (XrXirType) checked_u32(c, (uint32_t) method.signature);
            method.receiver = checked_u32(c, method.receiver);
            uint32_t own = checked_count(c,method.own_parameter_count,&c->remaining.parameters);
            XrXirConstraint *own_constraints = checked_constraints(c,method.constraints,own);
            method.constraints = own_constraints;
            method.own_parameter_count = own_constraints ? own : 0;
            if (c->reading) methods[j] = method;
        }
        if (c->reading) declarations[i] = d;
    }
}
static void checked_types(CheckedCursor *c, XrXirModule *m) {
    uint32_t count = checked_u32(c, m->types ? m->types->count : 0);
    uint32_t nominals = checked_u32(c, m->types && m->types->nominals ? m->types->nominals->count : 0);
    uint32_t interfaces = checked_u32(c, m->types && m->types->interfaces ? m->types->interfaces->count : 0);
    if (c->status != XR_XIR_OK || (!count && !nominals && !interfaces)) return;
    if (count > XR_XIR_CONSTRUCTED_TYPE_LIMIT - XR_XIR_CONSTRUCTED_TYPE_BASE) { c->status = XR_XIR_BUDGET; return; }
    XrXirTypes *types = checked_array(c, m->types, 1, sizeof(*types), 12);
    if (!types) return;
    m->types = types;
    XrXirTypeNode *signatures = checked_array(c, types->nodes, count, sizeof(*signatures), 12);
    if (c->reading) { types->nodes = signatures; types->count = signatures ? count : 0; }
    if (count && !signatures) return;
    for (uint32_t i = 0; i < count && c->status == XR_XIR_OK; ++i) {
        XrXirTypeNode s = signatures[i];
        s.kind = checked_u32(c, s.kind);
        s.parameter_span = checked_u32(c, s.parameter_span);
        if (s.kind == XR_XIR_TYPE_CALLABLE) {
            s.parameter_count = checked_count(c, s.parameter_count, &c->remaining.parameters);
            XrXirCallableParameter *parameters = checked_array(c, s.parameters, s.parameter_count, sizeof(*parameters), 8);
            s.parameters = parameters;
            for (uint32_t p = 0; p < s.parameter_count && c->status == XR_XIR_OK; ++p) {
                XrXirCallableParameter parameter = parameters[p];
                parameter.type = (XrXirType) checked_u32(c, (uint32_t) parameter.type);
                parameter.mode = checked_u32(c, parameter.mode);
                if (c->reading) parameters[p] = parameter;
            }
            s.result = (XrXirType) checked_u32(c, (uint32_t) s.result);
            s.flags = checked_u32(c, s.flags);
        } else if (s.kind == XR_XIR_TYPE_ARRAY || s.kind == XR_XIR_TYPE_CELL || s.kind == XR_XIR_TYPE_NULLABLE) {
            s.element = (XrXirType) checked_u32(c, (uint32_t) s.element);
        } else if (s.kind == XR_XIR_TYPE_NOMINAL) {
            s.nominal.declaration = checked_u32(c, s.nominal.declaration);
            s.nominal.argument_count = checked_count(c, s.nominal.argument_count, &c->remaining.parameters);
            XrXirType *arguments = checked_array(c, s.nominal.arguments, s.nominal.argument_count, sizeof(*arguments), 4);
            s.nominal.arguments = arguments;
            for (uint32_t a = 0; a < s.nominal.argument_count && c->status == XR_XIR_OK; ++a) {
                XrXirType argument = (XrXirType) checked_u32(c, (uint32_t) arguments[a]);
                if (c->reading) arguments[a] = argument;
            }
            s.nominal.field_count = checked_count(c, s.nominal.field_count, &c->remaining.parameters);
            XrXirType *fields = checked_array(c, s.nominal.fields, s.nominal.field_count, sizeof(*fields), 4);
            s.nominal.fields = fields;
            for (uint32_t f = 0; f < s.nominal.field_count && c->status == XR_XIR_OK; ++f) {
                XrXirType field = (XrXirType) checked_u32(c, (uint32_t) fields[f]);
                if (c->reading) fields[f] = field;
            }
        } else if (c->status == XR_XIR_OK) {
            c->status = XR_XIR_BAD_TYPE;
        }
        if (c->reading) signatures[i] = s;
    }
    checked_nominals(c, types, nominals);
    checked_interfaces(c, types, interfaces);
}
static void checked_defaults(CheckedCursor *c, XrXirModule *module) {
    const XrXirDefaultTable *original = module->defaults;
    uint32_t count = checked_u32(c, original ? original->count : 0);
    if (c->status != XR_XIR_OK || !count) return;
    XrXirDefaultTable *table = checked_array(c, original, 1, sizeof(*table), 16);
    if (c->reading) module->defaults = table;
    if (!table) return;
    XrXirDefaultBinding *records = checked_array(c, table->records, count, sizeof(*records), 16);
    if (c->reading) { table->records = records; table->count = records ? count : 0; }
    if (!records) return;
    for (uint32_t i = 0; i < count && c->status == XR_XIR_OK; ++i) {
        XrXirDefaultBinding record = records[i];
        record.owner_kind = checked_u32(c, record.owner_kind);
        record.owner = checked_u32(c, record.owner);
        record.ordinal = checked_u32(c, record.ordinal);
        record.function = checked_u32(c, record.function);
        if (c->reading) records[i] = record;
    }
}
static void checked_module(CheckedCursor *c, XrXirModule *m, bool allow_provenance);
static void checked_provenance(CheckedCursor *c, XrXirModule *m, bool allowed) {
    uint32_t present = checked_u32(c, m->provenance ? 1u : 0u);
    if (c->status != XR_XIR_OK) return;
    if (present > 1 || (present && !allowed)) { c->status = XR_XIR_BAD_STRUCTURE; return; }
    if (!present) return;
    XrXirProvenance *p = checked_array(c, m->provenance, 1, sizeof(*p), 8);
    if (c->reading) m->provenance = p;
    if (!p) return;
    XrXirArtifact *source = checked_array(c, p->source, 1, sizeof(*source), 8);
    if (c->reading) {
        p->source = source;
        if (source) { source->module.stage = XR_XIR_CHECKED; source->budget = c->remaining; }
    }
    if (!source) return;
    XrXirModule original = source->module;
    checked_module(c, &original, false);
    if (c->reading) source->module = original;
    if (c->status != XR_XIR_OK) return;
    XrXirOrigin *origins = checked_array(c, p->origins, m->function_count, sizeof(*origins), 8);
    if (c->reading) { p->origins = origins; p->count = origins ? m->function_count : 0; }
    for (uint32_t i = 0; i < m->function_count && c->status == XR_XIR_OK; ++i) {
        XrXirOrigin origin = origins[i];
        origin.function = checked_u32(c, origin.function);
        origin.argument_count = checked_u32(c, origin.argument_count);
        XrXirType *arguments = checked_array(c, origin.arguments, origin.argument_count, sizeof(*arguments), 4);
        origin.arguments = arguments;
        for (uint32_t a = 0; a < origin.argument_count && c->status == XR_XIR_OK; ++a) {
            XrXirType type = (XrXirType)checked_u32(c, (uint32_t)arguments[a]);
            if (c->reading) arguments[a] = type;
        }
        if (c->reading) origins[i] = origin;
    }
}
static void checked_module(CheckedCursor *c, XrXirModule *m, bool allow_provenance) {
    uint32_t kind = checked_u32(c, (uint32_t)m->linkage_kind);
    if (c->status == XR_XIR_OK && kind > XR_XIR_LIBRARY) c->status = XR_XIR_BAD_STRUCTURE;
    if (c->reading && c->status == XR_XIR_OK) m->linkage_kind = (XrXirLinkageKind)kind;
    if (c->status != XR_XIR_OK) return;
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
        XrXirDeclarations *owned = checked_array(c, m->declarations, 1, sizeof(*owned), 24);
        m->declarations = owned;
        if (!owned) return;
        XrXirDeclarations d = *owned;
        checked_declarations(c, &d, count);
        if (c->reading) *owned = d;
    }
    checked_generics(c, m);
    checked_types(c, m);
    if (c->status == XR_XIR_OK) checked_defaults(c, m);
    if (c->status == XR_XIR_OK) checked_provenance(c, m, allow_provenance);
}
static void checked_digest(const uint8_t *bytes, size_t size, uint8_t digest[32]) {
    XrSHA256Context sha;
    xr_sha256_init(&sha);
    xr_sha256_update(&sha, bytes, 32);
    xr_sha256_update(&sha, bytes + 64, size - 64);
    xr_sha256_final(&sha, digest);
}
static XrXirStatus checked_error(XrXirStatus status, XrXirDiagnostic *diagnostic) {
    if (diagnostic) *diagnostic = (XrXirDiagnostic) {status, UINT32_MAX, UINT32_MAX, UINT32_MAX, XR_XIR_DIAGNOSTIC_NONE};
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
    if (!artifact || artifact->module.stage != XR_XIR_CHECKED)
        return checked_error(XR_XIR_BAD_STAGE, diagnostic);
    XrXirBudget limits = budget ? *budget : xr_xir_default_budget();
    XrXirStatus status = xr_xir_artifact_verify(artifact, &limits, diagnostic);
    if (status != XR_XIR_OK) return status;
    size_t capacity = checked_capacity(&limits);
    if (capacity < 64) return checked_error(XR_XIR_BUDGET, diagnostic);
    CheckedCursor c = {NULL, NULL, 64, capacity, 0, limits, XR_XIR_OK, false};
    XrXirModule module = artifact->module;
    checked_module(&c, &module, true);
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
    checked_module(&c, &module, true);
    if (c.status != XR_XIR_OK || c.position != size) {
        xr_free(bytes); return checked_error(XR_XIR_BAD_STRUCTURE, diagnostic);
    }
    checked_digest(bytes, size, bytes + 32);
    *output = (XrXirCheckedPacket) {bytes, size};
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_checked_read_remaining(const void *bytes, size_t length,
    XrXirBudget *budget, XrXirArtifact **output, XrXirDiagnostic *diagnostic) {
    if (!output) return checked_error(XR_XIR_BAD_STRUCTURE, diagnostic);
    *output = NULL;
    XrXirBudget limits = budget ? *budget : xr_xir_default_budget();
    if (!bytes || length < 64) return checked_error(XR_XIR_BAD_STRUCTURE, diagnostic);
    if (length > checked_capacity(&limits)) return checked_error(XR_XIR_BUDGET, diagnostic);
    CheckedCursor c = {bytes, NULL, 8, length, 0, limits, XR_XIR_OK, true};
    XrXirArtifact *artifact = NULL;
    if (!checked_read_work(&c,8)) goto decode_failure;
    if (memcmp(bytes,"XRCHK\0\0\0",8)) { c.status=XR_XIR_BAD_STRUCTURE; goto decode_failure; }
    uint32_t schema = checked_u32(&c,0), contract = checked_u32(&c,0);
    uint32_t stage = checked_u32(&c,0), reserved = checked_u32(&c,0);
    uint64_t payload = checked_integer(&c,0,8);
    if (c.status != XR_XIR_OK) goto decode_failure;
    if (schema != XR_XIR_CHECKED_SCHEMA || contract != XR_XIR_CHECKED_CONTRACT || reserved || payload != length-64) {
        c.status=XR_XIR_BAD_STRUCTURE; goto decode_failure;
    }
    if (stage != XR_XIR_CHECKED) { c.status=XR_XIR_BAD_STAGE; goto decode_failure; }
    /* Hash reads length-32 bytes; comparing the stored digest reads another 32. */
    if (!checked_read_work(&c,length)) goto decode_failure;
    uint8_t digest[32]; checked_digest(bytes,length,digest);
    if (memcmp(digest,(const uint8_t *)bytes+32,32)) { c.status=XR_XIR_BAD_STRUCTURE; goto decode_failure; }
    if (sizeof(XrXirArtifact)>limits.metadata_bytes) { c.status=XR_XIR_BUDGET; goto decode_failure; }
    artifact=xr_calloc(1,sizeof(*artifact));
    if (!artifact) { c.status=XR_XIR_OUT_OF_MEMORY; goto decode_failure; }
    artifact->module.stage=XR_XIR_CHECKED; artifact->budget=limits;
    c.position=64; c.allocated=sizeof(*artifact);
    checked_module(&c,&artifact->module,true);
    if (c.status==XR_XIR_OK && c.position!=length) c.status=XR_XIR_BAD_STRUCTURE;
    if (c.status!=XR_XIR_OK) goto decode_failure;
    /* Metadata/counts describe one artifact: verifier charges those once.
     * Do not subtract decoded allocation bytes a second time. */
    limits.work=c.remaining.work;
    c.status=xr_xir_verify_remaining(&artifact->module,&limits,diagnostic);
    if (budget) *budget=limits;
    if (c.status!=XR_XIR_OK) { xr_xir_artifact_free(artifact); return c.status; }
    *output=artifact;
    return XR_XIR_OK;
decode_failure:
    if (budget) budget->work=c.remaining.work;
    xr_xir_artifact_free(artifact);
    return checked_error(c.status,diagnostic);
}

XrXirStatus xr_xir_checked_read(const void *bytes, size_t length,
    const XrXirBudget *budget, XrXirArtifact **output, XrXirDiagnostic *diagnostic) {
    XrXirBudget remaining=budget?*budget:xr_xir_default_budget();
    return xr_xir_checked_read_remaining(bytes,length,&remaining,output,diagnostic);
}
