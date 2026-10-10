/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * struct_string_current_packet_shape.h - Original Cell-rooted struct String facts
 *
 * KEY CONCEPT:
 *   Each verified artifact independently exposes the original value-copy roots,
 *   ordered fields, three writes and precise comparison and result operands.
 *   These consumer observations grant no product admission or old copy census.
 */
#ifndef STRUCT_STRING_CURRENT_PACKET_SHAPE_H
#define STRUCT_STRING_CURRENT_PACKET_SHAPE_H
#include "xir/xxir_types.h"
#include "xir/xxir_nominal.h"
#include "xir/xxir_declarations.h"
#include <string.h>

enum { STRUCT_STRING_FIRST, STRUCT_STRING_SECOND, STRUCT_STRING_WRAPPED, STRUCT_STRING_REWRITTEN, STRUCT_STRING_ROOT_COUNT };
typedef struct StructStringShape {
    const XrXirModule *module;
    const XrXirFunction *function;
    uint32_t report, envelope, constructors[2], cells[STRUCT_STRING_ROOT_COUNT];
    XrXirType report_type, envelope_type;
} StructStringShape;
typedef struct StructStringReadPath {
    uint32_t root, depth, fields[2];
    XrXirType type;
} StructStringReadPath;
typedef struct StructStringComparison {
    uint32_t root, depth, fields[2];
    const char *literal;
} StructStringComparison;

static bool struct_string_packet_literal_equal(XrXirLiteral literal, const char *text) {
    size_t length = strlen(text);
    return literal.length == length && literal.bytes && !memcmp(literal.bytes, text, length);
}

static const XrXirInstruction *struct_string_packet_operand(const StructStringShape *shape, uint32_t id, uint32_t before) {
    CHECK(shape->function && !shape->function->parameter_count && id < before &&
        before <= shape->function->instruction_count);
    return &shape->function->instructions[id];
}

static XrXirLiteral struct_string_packet_string_value(const StructStringShape *shape, uint32_t id, uint32_t before) {
    const XrXirInstruction *value = struct_string_packet_operand(shape, id, before);
    const XrXirDeclarations *declarations = shape->module->declarations;
    CHECK(value->op == XR_XIR_CONST_STRING && value->type == XR_XIR_STRING && value->immediate >= 0 &&
        (uint64_t)value->immediate < declarations->literal_count && declarations->literals);
    return declarations->literals[value->immediate];
}

static void struct_string_packet_integer_value(const StructStringShape *shape, uint32_t id, uint32_t before, int64_t expected) {
    const XrXirInstruction *value = struct_string_packet_operand(shape, id, before);
    CHECK(value->op == XR_XIR_CONST_INT && value->type == XR_XIR_I64 && value->immediate == expected);
}

static XrXirType struct_string_packet_root_type(const StructStringShape *shape, uint32_t root) {
    CHECK(root < STRUCT_STRING_ROOT_COUNT);
    return root < STRUCT_STRING_WRAPPED ? shape->report_type : shape->envelope_type;
}

static void struct_string_packet_nominal_names(StructStringShape *shape) {
    const XrXirNominalTable *table = shape->module->types->nominals;
    const XrXirDeclarations *declarations = shape->module->declarations;
    CHECK(table && table->count == 2 && (table->declarations || table->identities));
    CHECK(declarations->root_module < declarations->module_count && declarations->modules);
    const XrXirSourceModule *root = &declarations->modules[declarations->root_module];
    for (uint32_t n = 0; n < table->count; ++n) {
        XrXirLiteral name, module;
        uint32_t kind, arity, exported, field_count;
        if (table->declarations) {
            const XrXirNominalDeclaration *record = &table->declarations[n];
            name = record->name; module = record->module; kind = record->kind;
            arity = record->parameter_count; exported = record->exported; field_count = record->field_count;
            CHECK(record->fields);
        } else {
            const XrXirNominalIdentity *record = &table->identities[n];
            name = record->name; module = record->module; kind = record->kind;
            arity = record->arity; exported = record->exported; field_count = record->field_count;
            CHECK(record->fields);
        }
        CHECK(kind == XR_XIR_NOMINAL_STRUCT && !arity && !exported && field_count == 3);
        CHECK(module.length == root->name_length && module.bytes && root->name &&
            !memcmp(module.bytes, root->name, module.length));
        const char *names[3];
        if (struct_string_packet_literal_equal(name, "Report")) {
            CHECK(shape->report == UINT32_MAX); shape->report = n;
            names[0] = "label"; names[1] = "footer"; names[2] = "code";
        } else {
            CHECK(struct_string_packet_literal_equal(name, "Envelope") && shape->envelope == UINT32_MAX); shape->envelope = n;
            names[0] = "prefix"; names[1] = "report"; names[2] = "suffix";
        }
        for (uint32_t f = 0; f < 3; ++f) {
            XrXirLiteral field = table->declarations ? table->declarations[n].fields[f].name : table->identities[n].fields[f].name;
            CHECK(struct_string_packet_literal_equal(field, names[f]));
        }
    }
    CHECK(shape->report != UINT32_MAX && shape->envelope != UINT32_MAX && shape->report != shape->envelope);
}

static const uint32_t *struct_string_packet_constructor_arguments(const StructStringShape *shape, uint32_t instruction) {
    const XrXirInstruction *op = &shape->function->instructions[instruction];
    CHECK(op->op == XR_XIR_STRUCT_NEW && op->args[1] == 3 && shape->function->operands &&
        shape->function->operand_count >= 3 && op->args[0] <= shape->function->operand_count - 3);
    return shape->function->operands + op->args[0];
}

static void struct_string_packet_constructors(StructStringShape *shape) {
    for (uint32_t i = 0; i < shape->function->instruction_count; ++i) {
        const XrXirInstruction *op = &shape->function->instructions[i];
        if (op->op != XR_XIR_STRUCT_NEW) continue;
        const XrXirTypeNode *node = xr_xir_type_node(shape->module->types, op->type);
        CHECK(node && node->kind == XR_XIR_TYPE_NOMINAL && !node->nominal.argument_count &&
            node->nominal.field_count == 3 && node->nominal.fields);
        bool report = node->nominal.declaration == shape->report;
        CHECK(report || node->nominal.declaration == shape->envelope);
        unsigned slot = report ? 0 : 1;
        CHECK(shape->constructors[slot] == UINT32_MAX); shape->constructors[slot] = i;
        const uint32_t *arguments = struct_string_packet_constructor_arguments(shape, i);
        CHECK(struct_string_packet_literal_equal(struct_string_packet_string_value(shape, arguments[0], i), report ? "ready" : "start"));
        if (report) {
            shape->report_type = op->type;
            CHECK(node->nominal.fields[0] == XR_XIR_STRING && node->nominal.fields[1] == XR_XIR_STRING &&
                node->nominal.fields[2] == XR_XIR_I64);
            CHECK(struct_string_packet_literal_equal(struct_string_packet_string_value(shape, arguments[1], i), "end"));
            struct_string_packet_integer_value(shape, arguments[2], i, 40);
        } else {
            shape->envelope_type = op->type;
            CHECK(node->nominal.fields[0] == XR_XIR_STRING && node->nominal.fields[2] == XR_XIR_STRING);
            CHECK(struct_string_packet_literal_equal(struct_string_packet_string_value(shape, arguments[2], i), "stop"));
        }
    }
    CHECK(shape->constructors[0] != UINT32_MAX && shape->constructors[1] != UINT32_MAX);
    const XrXirTypeNode *envelope = xr_xir_type_node(shape->module->types, shape->envelope_type);
    CHECK(envelope && envelope->nominal.fields[1] == shape->report_type);
    const XrXirNominalTable *table = shape->module->types->nominals;
    if (table->declarations) {
        const XrXirType *report = xr_xir_type_node(shape->module->types, shape->report_type)->nominal.fields;
        for (uint32_t f = 0; f < 3; ++f) {
            CHECK(table->declarations[shape->report].fields[f].type == report[f]);
            CHECK(table->declarations[shape->envelope].fields[f].type == envelope->nominal.fields[f]);
        }
    }
}

static void struct_string_packet_bindings(StructStringShape *shape) {
    for (uint32_t i = 0; i < shape->function->instruction_count; ++i) {
        const XrXirInstruction *op = &shape->function->instructions[i];
        if (op->op != XR_XIR_CELL_NEW) continue;
        const XrXirTypeNode *cell = xr_xir_type_node(shape->module->types, op->type);
        CHECK(cell && cell->kind == XR_XIR_TYPE_CELL);
        if (cell->element != shape->report_type && cell->element != shape->envelope_type) continue;
        bool report = cell->element == shape->report_type;
        const XrXirInstruction *initial = struct_string_packet_operand(shape, op->args[0], i);
        CHECK(initial->type == cell->element);
        uint32_t root;
        if (initial->op == XR_XIR_STRUCT_NEW) {
            root = report ? STRUCT_STRING_FIRST : STRUCT_STRING_WRAPPED;
            CHECK(op->args[0] == shape->constructors[report ? 0 : 1]);
        } else {
            root = report ? STRUCT_STRING_SECOND : STRUCT_STRING_REWRITTEN;
            uint32_t original = report ? STRUCT_STRING_FIRST : STRUCT_STRING_WRAPPED;
            CHECK(initial->op == XR_XIR_CELL_READ && shape->cells[original] != UINT32_MAX &&
                initial->args[0] == shape->cells[original] && initial->args[0] < op->args[0]);
        }
        CHECK(shape->cells[root] == UINT32_MAX); shape->cells[root] = i;
    }
    for (uint32_t root = 0; root < STRUCT_STRING_ROOT_COUNT; ++root) {
        CHECK(shape->cells[root] != UINT32_MAX);
        for (uint32_t other = 0; other < root; ++other) CHECK(shape->cells[root] != shape->cells[other]);
    }
    uint32_t envelope = shape->constructors[1];
    uint32_t first = struct_string_packet_constructor_arguments(shape, envelope)[1];
    const XrXirInstruction *read = struct_string_packet_operand(shape, first, envelope);
    CHECK(read->op == XR_XIR_CELL_READ && read->type == shape->report_type &&
        read->args[0] == shape->cells[STRUCT_STRING_FIRST] && read->args[0] < first);
}

/* Trace real value reads, without retaining a runtime interior address. */
static StructStringReadPath struct_string_packet_read_path(const StructStringShape *shape, uint32_t id, uint32_t before) {
    const XrXirInstruction *op = struct_string_packet_operand(shape, id, before);
    StructStringReadPath path = {UINT32_MAX, 0, {0, 0}, op->type};
    while (op->op == XR_XIR_STRUCT_GET) {
        CHECK(path.depth < 2 && op->immediate >= 0);
        const XrXirInstruction *parent = struct_string_packet_operand(shape, op->args[0], id);
        const XrXirTypeNode *node = xr_xir_type_node(shape->module->types, parent->type);
        CHECK(node && node->kind == XR_XIR_TYPE_NOMINAL && node->nominal.fields &&
            (uint64_t)op->immediate < node->nominal.field_count &&
            op->type == node->nominal.fields[op->immediate]);
        path.fields[path.depth++] = (uint32_t)op->immediate;
        id = op->args[0]; op = parent;
    }
    CHECK(op->op == XR_XIR_CELL_READ && op->args[0] < id);
    for (uint32_t root = 0; root < STRUCT_STRING_ROOT_COUNT; ++root)
        if (op->args[0] == shape->cells[root]) { CHECK(path.root == UINT32_MAX); path.root = root; }
    CHECK(path.root != UINT32_MAX && op->type == struct_string_packet_root_type(shape, path.root));
    if (path.depth == 2) {
        uint32_t first = path.fields[0]; path.fields[0] = path.fields[1]; path.fields[1] = first;
    }
    return path;
}

static bool struct_string_packet_path_equal(StructStringReadPath path, const StructStringComparison *expected) {
    return path.root == expected->root && path.depth == expected->depth &&
        (!path.depth || path.fields[0] == expected->fields[0]) &&
        (path.depth != 2 || path.fields[1] == expected->fields[1]);
}

static void struct_string_packet_writes(const StructStringShape *shape) {
    unsigned writes = 0;
    for (uint32_t i = 0; i < shape->function->instruction_count; ++i) {
        const XrXirInstruction *op = &shape->function->instructions[i];
        if (op->op != XR_XIR_PLACE_WRITE) continue;
        CHECK(writes < 3 && op->type == XR_XIR_UNIT);
        const XrXirInstruction *field = struct_string_packet_operand(shape, op->args[0], i);
        CHECK(field->op == XR_XIR_FIELD_PLACE && field->immediate == (writes ? 2 : 0));
        CHECK(field->type == (writes == 1 ? XR_XIR_I64 : XR_XIR_STRING));
        const XrXirInstruction *place = struct_string_packet_operand(shape, field->args[0], op->args[0]);
        uint32_t root = writes == 2 ? STRUCT_STRING_REWRITTEN : STRUCT_STRING_SECOND;
        CHECK(place->op == XR_XIR_CELL_PLACE && place->type == struct_string_packet_root_type(shape, root) &&
            place->args[0] == shape->cells[root] && place->args[0] < field->args[0]);
        if (writes == 1) struct_string_packet_integer_value(shape, op->args[1], i, 42);
        else CHECK(struct_string_packet_literal_equal(struct_string_packet_string_value(shape, op->args[1], i), writes ? "done" : "go"));
        ++writes;
    }
    CHECK(writes == 3);
}

static void struct_string_packet_comparisons(const StructStringShape *shape) {
    static const StructStringComparison strings[12] = {
        {STRUCT_STRING_FIRST, 1, {0, 0}, "ready"}, {STRUCT_STRING_SECOND, 1, {0, 0}, "go"},
        {STRUCT_STRING_FIRST, 1, {1, 0}, "end"}, {STRUCT_STRING_SECOND, 1, {1, 0}, "end"},
        {STRUCT_STRING_WRAPPED, 1, {0, 0}, "start"}, {STRUCT_STRING_REWRITTEN, 1, {0, 0}, "start"},
        {STRUCT_STRING_WRAPPED, 2, {1, 0}, "ready"}, {STRUCT_STRING_REWRITTEN, 2, {1, 0}, "ready"},
        {STRUCT_STRING_WRAPPED, 2, {1, 1}, "end"}, {STRUCT_STRING_REWRITTEN, 2, {1, 1}, "end"},
        {STRUCT_STRING_WRAPPED, 1, {2, 0}, "stop"}, {STRUCT_STRING_REWRITTEN, 1, {2, 0}, "done"}
    };
    static const StructStringComparison integers[3] = {
        {STRUCT_STRING_FIRST, 1, {2, 0}, NULL}, {STRUCT_STRING_WRAPPED, 2, {1, 2}, NULL}, {STRUCT_STRING_REWRITTEN, 2, {1, 2}, NULL}
    };
    uint32_t string_mask = 0, integer_mask = 0;
    unsigned string_count = 0, integer_count = 0;
    for (uint32_t i = 0; i < shape->function->instruction_count; ++i) {
        const XrXirInstruction *op = &shape->function->instructions[i];
        if (op->op != XR_XIR_EQUAL) continue;
        CHECK(op->type == XR_XIR_BOOL && !op->immediate);
        StructStringReadPath path = struct_string_packet_read_path(shape, op->args[0], i);
        CHECK(path.type == xr_xir_operand_type(shape->function, op->args[1]));
        uint32_t found = UINT32_MAX;
        if (path.type == XR_XIR_STRING) {
            XrXirLiteral literal = struct_string_packet_string_value(shape, op->args[1], i);
            for (uint32_t comparison = 0; comparison < 12; ++comparison)
                if (struct_string_packet_path_equal(path, &strings[comparison]) && struct_string_packet_literal_equal(literal, strings[comparison].literal)) {
                    CHECK(found == UINT32_MAX); found = comparison;
                }
            CHECK(found != UINT32_MAX && !(string_mask & (1u << found)));
            string_mask |= 1u << found; ++string_count;
        } else {
            CHECK(path.type == XR_XIR_I64); struct_string_packet_integer_value(shape, op->args[1], i, 40);
            for (uint32_t comparison = 0; comparison < 3; ++comparison)
                if (struct_string_packet_path_equal(path, &integers[comparison])) { CHECK(found == UINT32_MAX); found = comparison; }
            CHECK(found != UINT32_MAX && !(integer_mask & (1u << found)));
            integer_mask |= 1u << found; ++integer_count;
        }
    }
    CHECK(string_count == 12 && string_mask == 4095u && integer_count == 3 && integer_mask == 7u);
}

static void struct_string_packet_returns(const StructStringShape *shape) {
    static const StructStringComparison result = {STRUCT_STRING_SECOND, 1, {2, 0}, NULL};
    unsigned success = 0, failure = 0;
    for (uint32_t i = 0; i < shape->function->instruction_count; ++i) {
        const XrXirInstruction *op = &shape->function->instructions[i];
        if (op->op != XR_XIR_RETURN) continue;
        CHECK(op->type == XR_XIR_UNIT);
        const XrXirInstruction *value = struct_string_packet_operand(shape, op->args[0], i);
        if (value->op == XR_XIR_CONST_INT) {
            struct_string_packet_integer_value(shape, op->args[0], i, 0); ++failure;
        } else {
            StructStringReadPath path = struct_string_packet_read_path(shape, op->args[0], i);
            CHECK(path.type == XR_XIR_I64 && struct_string_packet_path_equal(path, &result)); ++success;
        }
    }
    CHECK(success == 1 && failure == 1);
}

static void struct_string_current_packet_shape(const XrXirModule *module) {
    CHECK(module && module->types && module->types->nominals && module->declarations &&
        module->functions && module->declarations->functions);
    StructStringShape shape = {0}; shape.module = module;
    shape.report = shape.envelope = UINT32_MAX;
    shape.constructors[0] = shape.constructors[1] = UINT32_MAX;
    for (uint32_t root = 0; root < STRUCT_STRING_ROOT_COUNT; ++root) shape.cells[root] = UINT32_MAX;
    struct_string_packet_nominal_names(&shape);
    unsigned answers = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        if (module->declarations->functions[f].module != module->declarations->root_module ||
            function->name_length != 6 || memcmp(function->name, "answer", 6)) continue;
        ++answers; CHECK(answers == 1 && !module->declarations->functions[f].exported &&
            !function->parameter_count && function->result == XR_XIR_I64 && function->instructions);
        shape.function = function;
        struct_string_packet_constructors(&shape); struct_string_packet_bindings(&shape); struct_string_packet_writes(&shape); struct_string_packet_comparisons(&shape); struct_string_packet_returns(&shape);
    }
    CHECK(answers == 1);
}
#endif // STRUCT_STRING_CURRENT_PACKET_SHAPE_H
