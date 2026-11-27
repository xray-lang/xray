/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "xir/xxir_construction.h"
#include "xir/xxir_types.h"
#include <inttypes.h>

typedef struct CheckedMathInterface {
    const XrXirModule *checked;
    const XrXirSlotFacts *slots;
    const XrXirFunction *initializer;
    XrXirEffects *effects;
    XrFingerprint source_fingerprint;
    uint32_t root, functions, binding_rows[MATH_SCALAR_EXPORT_COUNT];
} CheckedMathInterface;

static bool interface_type(const XrXirTypes *types, XrXirType type, XrXirBuiltinType *output) {
    if (type == XR_XIR_BOOL || type == XR_XIR_I64 || type == XR_XIR_F64) {
        *output = (XrXirBuiltinType){type, false};
        return true;
    }
    if (xr_xir_type_is_array(types, type) && xr_xir_array_element(types, type) == XR_XIR_F64) {
        *output = (XrXirBuiltinType){XR_XIR_F64, true};
        return true;
    }
    return false;
}

static bool interface_name(XrXirLiteral name) {
    return name.bytes && name.length && !memchr(name.bytes, '\0', name.length);
}

static bool interface_function(const CheckedMathInterface *view, uint32_t index) {
    const XrXirModule *module = view->checked;
    const XrXirFunction *function = &module->functions[index];
    const XrXirFunctionIdentity *identity = &module->declarations->functions[index];
    const XrXirParameterNames *names = xr_xir_parameter_names_find(module->declarations,
        XR_XIR_PARAMETER_FUNCTION, index, 0);
    XrXirBuiltinType result;
    if (!interface_name((XrXirLiteral){function->name, function->name_length}) ||
        !function->block_count || identity->method_kind != XR_XIR_NON_MEMBER ||
        identity->nominal_owner || identity->native_callable || identity->cleanup_owner ||
        identity->capture_owner || identity->capture_count || identity->test_role || identity->test_timeout_milliseconds ||
        (identity->promises & ~(XR_XIR_FUNCTION_NO_SUSPEND | XR_XIR_FUNCTION_REST)) ||
        (module->generics && module->generics[index].parameter_count) ||
        function->error_type != XR_XIR_NEVER || function->escaping_parameter_count ||
        !interface_type(module->types, function->result, &result) || result.array ||
        !names || names->parameter_offset || names->count != function->parameter_count) return false;

    for (uint32_t p = 0; p < function->parameter_count; ++p) {
        XrXirBuiltinType type;
        bool rest = (identity->promises & XR_XIR_FUNCTION_REST) && p + 1 == function->parameter_count;
        if (!interface_name(names->names[p]) || !interface_type(module->types, function->parameters[p], &type) ||
            type.array != rest || xr_xir_declared_parameter_mode(module->types, function->parameters[p],
                function->consume_parameters, function->consume_parameter_count, p) != XR_PARAM_READ) return false;
    }
    XrXirEffectFacts facts = xr_xir_effects_read(view->effects, index);
    return facts.status == XR_XIR_OK;
}

static bool interface_initializer(CheckedMathInterface *view) {
    const XrXirDeclarations *declarations = view->checked->declarations;
    const XrXirSourceModule *root = &declarations->modules[view->root];
    if (root->initializer >= view->checked->function_count || root->dependency_count ||
        root->lexical_scope || root->storage != XR_XIR_MODULE_FRESH_STORAGE) return false;
    const XrXirFunction *initializer = &view->checked->functions[root->initializer];
    if (initializer->parameter_count || initializer->result != XR_XIR_UNIT ||
        (initializer->error_type != XR_XIR_NEVER && initializer->error_type != XR_XIR_ERROR) || initializer->operand_count ||
        initializer->block_count != 1 || initializer->blocks[0].first || initializer->blocks[0].panic ||
        initializer->blocks[0].frontier || initializer->blocks[0].count != initializer->instruction_count) return false;

    for (uint32_t i = 0; i < initializer->instruction_count; ++i) {
        const XrXirInstruction *operation = &initializer->instructions[i];
        switch (operation->op) {
            case XR_XIR_CONST_INT: case XR_XIR_SUB_INT:
                if (operation->type != XR_XIR_I64) return false;
                break;
            case XR_XIR_CONST_FLOAT: case XR_XIR_DIV_FLOAT:
                if (operation->type != XR_XIR_F64) return false;
                break;
            case XR_XIR_SLOT_INIT:
                if (operation->immediate < 0 || (uint64_t)operation->immediate >= declarations->slot_count ||
                    declarations->slots[operation->immediate].module != view->root) return false;
                break;
            case XR_XIR_RETURN:
                break;
            default:
                return false;
        }
    }
    view->initializer = initializer;
    return true;
}

static bool interface_checked_function(const CheckedMathInterface *view, const char *name) {
    for (uint32_t i = 0; i < view->checked->function_count; ++i) {
        const XrXirFunctionIdentity *identity = &view->checked->declarations->functions[i];
        const XrXirFunction *function = &view->checked->functions[i];
        if (identity->module == view->root && identity->exported &&
            same_name(function->name, function->name_length, name)) return true;
    }
    return false;
}

static bool interface_checked_slot(const CheckedMathInterface *view, const XrXirSourceDeclaration *declaration) {
    if (declaration->mutable || !declaration->type.known) return false;
    for (uint32_t i = 0; i < view->slots->count; ++i) {
        const XrXirSlotFact *fact = &view->slots->rows[i];
        if (same_name(fact->name, fact->name_length, declaration->name))
            return view->checked->declarations->slots[i].type == declaration->type.type;
    }
    return false;
}

static bool interface_source(const XrXirSourceSnapshot *snapshot, CheckedMathInterface *view) {
    const XrXirSourceView *source = xr_xir_compile_source_snapshot_view(snapshot);
    if (!source || !source->complete) return false;
    const XrXirSourceModule *root = &view->checked->declarations->modules[view->root];
    uint32_t module = UINT32_MAX;
    const XrFingerprint empty = {0};
    for (uint32_t i = 0; i < source->module_count; ++i) {
        const XrXirSourceQueryModule *unit = &source->modules[i];
        if (!same_name(root->name, root->name_length, unit->identity)) continue;
        if (module != UINT32_MAX || !unit->path || !memcmp(&unit->fingerprint, &empty, sizeof(empty))) return false;
        module = i;
    }
    if (module == UINT32_MAX) return false;

    /* The Source snapshot includes exported nominal and interface declarations
     * that are absent from the Checked function and slot lists. */
    uint32_t functions = 0, slots = 0;
    for (uint32_t i = 0; i < source->declaration_count; ++i) {
        const XrXirSourceDeclaration *declaration = &source->declarations[i];
        if (declaration->range.module != module || !declaration->exported) continue;
        if (declaration->parent || !declaration->name) return false;
        switch (declaration->kind) {
        case XR_XIR_SOURCE_FUNCTION:
            if (++functions > view->functions || !interface_checked_function(view, declaration->name)) return false;
            break;
        case XR_XIR_SOURCE_BINDING:
            if (++slots > view->slots->count || !interface_checked_slot(view, declaration)) return false;
            break;
        default:
            return false;
        }
    }
    if (functions != view->functions || slots != view->slots->count) return false;
    view->source_fingerprint = source->modules[module].fingerprint;
    return true;
}

static bool prepare_interface(const XrXirSourceResult *source, const ScalarOutput *exports, CheckedMathInterface *view) {
    view->checked = xr_xir_compile_artifact_module(source->checked);
    const XrXirModule *checked = view->checked;
    const XrXirDeclarations *declarations = checked->declarations;
    view->root = declarations->root_module;
    view->slots = xr_xir_compile_construction_slots(xr_xir_compile_artifact_construction(source->checked));
    if (!view->slots || view->slots->count != declarations->slot_count ||
        (view->slots->count && !view->slots->rows) ||
        (checked->defaults && checked->defaults->count) || !interface_initializer(view) ||
        xr_xir_compile_effects_analyze(source->checked, &view->effects) != XR_XIR_OK) return false;

    for (uint32_t i = 0; i < view->slots->count; ++i) {
        const XrXirSlot *slot = &declarations->slots[i];
        const XrXirSlotFact *fact = &view->slots->rows[i];
        if (slot->module != view->root || slot->mutable || (slot->type != XR_XIR_I64 && slot->type != XR_XIR_F64) ||
            fact->slot != i || fact->exported != 1 || !interface_name((XrXirLiteral){fact->name, fact->name_length}) ||
            (fact->constant_key.kind != XR_XIR_KEY_DYNAMIC && fact->constant_key.kind != XR_XIR_KEY_SCALAR)) return false;
    }
    for (uint32_t binding = 0; binding < MATH_SCALAR_EXPORT_COUNT; ++binding)
        view->binding_rows[binding] = UINT32_MAX;
    for (uint32_t i = 0; i < checked->function_count; ++i) {
        const XrXirFunctionIdentity *identity = &declarations->functions[i];
        if (identity->module != view->root || !identity->exported) continue;
        if (!interface_function(view, i)) return false;
        for (uint32_t binding = 0; binding < MATH_SCALAR_EXPORT_COUNT; ++binding)
            if (i == exports[binding].checked_index) view->binding_rows[binding] = view->functions;
        ++view->functions;
    }
    for (uint32_t binding = 0; binding < MATH_SCALAR_EXPORT_COUNT; ++binding)
        if (view->binding_rows[binding] >= view->functions) return false;
    return interface_source(source->snapshot, view);
}

static void write_interface_name(FILE *file, XrXirLiteral name) {
    fputs("{\"", file);
    for (uint32_t byte = 0; byte < name.length; ++byte) {
        unsigned value = (unsigned char)name.bytes[byte];
        if (value < 32 || value > 126 || value == '"' || value == '\\' || value == '?')
            fprintf(file, "\\%03o", value);
        else fputc((int)value, file);
    }
    fprintf(file, "\", %u}", name.length);
}

static void write_interface_parameters(FILE *file, const CheckedMathInterface *view) {
    const XrXirModule *module = view->checked;
    for (uint32_t i = 0; i < module->function_count; ++i) {
        const XrXirFunction *function = &module->functions[i];
        const XrXirFunctionIdentity *identity = &module->declarations->functions[i];
        if (identity->module != view->root || !identity->exported || !function->parameter_count) continue;
        const XrXirParameterNames *names = xr_xir_parameter_names_find(module->declarations,
            XR_XIR_PARAMETER_FUNCTION, i, 0);
        fprintf(file, "static const XrXirBuiltinParameter parameters_%u[] = {\n", i);
        for (uint32_t p = 0; p < names->count; ++p) {
            XrXirBuiltinType type = {0};
            (void)interface_type(module->types, function->parameters[p], &type);
            fputs("    {", file);
            write_interface_name(file, names->names[p]);
            fprintf(file, ", {%u, %u}, XR_PARAM_READ},\n", (unsigned)type.scalar, type.array ? 1u : 0u);
        }
        fputs("};\n", file);
    }
}

static void write_interface_functions(FILE *file, const CheckedMathInterface *view) {
    const XrXirModule *module = view->checked;
    fputs("\nstatic const XrXirBuiltinFunction functions[] = {\n", file);
    for (uint32_t i = 0; i < module->function_count; ++i) {
        const XrXirFunction *function = &module->functions[i];
        const XrXirFunctionIdentity *identity = &module->declarations->functions[i];
        if (identity->module != view->root || !identity->exported) continue;
        XrXirEffectFacts facts = xr_xir_effects_read(view->effects, i);
        fputs("    {", file);
        write_interface_name(file, (XrXirLiteral){function->name, function->name_length});
        if (function->parameter_count) fprintf(file, ", parameters_%u", i);
        else fputs(", NULL", file);
        fprintf(file, ", %u, {%u, 0}, %u, %u, {%u, %u}, {%u, %u}},\n", function->parameter_count,
            (unsigned)function->result, (unsigned)function->error_type, identity->promises,
            (unsigned)facts.control.suspend, (unsigned)facts.control.throws,
            facts.root.requires_root ? 1u : 0u, facts.root.unresolved ? 1u : 0u);
    }
    fputs("};\n", file);
}

static void write_interface_slots(FILE *file, const CheckedMathInterface *view) {
    if (!view->slots->count) return;
    fputs("\nstatic const XrXirBuiltinSlot slots[] = {\n", file);
    for (uint32_t i = 0; i < view->slots->count; ++i) {
        const XrXirSlot *slot = &view->checked->declarations->slots[i];
        const XrXirSlotFact *fact = &view->slots->rows[i];
        fputs("    {", file);
        write_interface_name(file, (XrXirLiteral){fact->name, fact->name_length});
        fprintf(file, ", %u, {.kind = %u, .bits = UINT64_C(0x%016" PRIx64 ")}},\n", (unsigned)slot->type,
            (unsigned)fact->constant_key.kind, fact->constant_key.bits);
    }
    fputs("};\n", file);
}

static void write_interface_initializer(FILE *file, const XrXirFunction *initializer) {
    fputs("\nstatic const XrXirInstruction initializer[] = {\n", file);
    for (uint32_t i = 0; i < initializer->instruction_count; ++i) {
        XrXirInstruction instruction = initializer->instructions[i];
        fprintf(file, "    {.op = XR_XIR_%s, .type = %u, .args = {%u, %u}, .targets = {%u, %u}, .immediate = ",
            xr_xir_op_name(instruction.op), (unsigned)instruction.type, instruction.args[0], instruction.args[1],
            instruction.targets[0], instruction.targets[1]);
        if (instruction.immediate == INT64_MIN) fputs("INT64_MIN", file);
        else fprintf(file, "INT64_C(%" PRId64 ")", instruction.immediate);
        fprintf(file, ", .type_arguments = {%u, %u}},\n", instruction.type_arguments[0], instruction.type_arguments[1]);
    }
    fputs("};\n", file);
}

static void write_interface_bindings(FILE *file, const CheckedMathInterface *view) {
    for (uint32_t i = 0; i < MATH_SCALAR_EXPORT_COUNT; ++i) {
        const ScalarExport *spec = &scalar_exports[i];
        const char *parameters = spec->abi == XR_XIR_BUILTIN_F64_UNARY ? "double" : "double, double, double";
        fprintf(file, "\nextern double %s(%s);\n", spec->symbol, parameters);
    }
    fputs("\nstatic const XrXirBuiltinScalarBinding bindings[] = {\n", file);
    for (uint32_t i = 0; i < MATH_SCALAR_EXPORT_COUNT; ++i) {
        const ScalarExport *spec = &scalar_exports[i];
        bool unary = spec->abi == XR_XIR_BUILTIN_F64_UNARY;
        fprintf(file, "    {%u, %s, {.%s = %s}},\n", view->binding_rows[i],
            unary ? "XR_XIR_BUILTIN_F64_UNARY" : "XR_XIR_BUILTIN_F64_TERNARY",
            unary ? "unary" : "ternary", spec->symbol);
    }
    fputs("};\n", file);
}

static void write_interface(FILE *file, const CheckedMathInterface *view, const XrXirTarget *target) {
    fputs("/* Generated from the verified standard library declarations. */\n"
        "#include \"xir/xxir_builtin_stdlib.h\"\n\n", file);
    write_interface_parameters(file, view);
    write_interface_functions(file, view);
    write_interface_slots(file, view);
    write_interface_initializer(file, view->initializer);
    write_interface_bindings(file, view);
    const XrXirSourceModule *owner = &view->checked->declarations->modules[view->root];
    fputs("\nconst XrXirBuiltinStdlibModule xray_stdlib_math_interface = {\n    .canonical = ", file);
    write_interface_name(file, (XrXirLiteral){owner->name, owner->name_length});
    fputs(",\n    .logical_path = ", file);
    write_interface_name(file, (XrXirLiteral){module_path, sizeof(module_path) - 1});
    fprintf(file, ",\n    .producer_target = {%u, %u, %u},\n    .source_fingerprint = {{",
        target->os, target->architecture, target->abi_version);
    for (size_t i = 0; i < sizeof(view->source_fingerprint.bytes); ++i)
        fprintf(file, "%s0x%02x", i ? ", " : "", (unsigned)view->source_fingerprint.bytes[i]);
    fprintf(file, "}},\n    .functions = functions, .function_count = %u,\n", view->functions);
    if (view->slots->count) fprintf(file, "    .slots = slots, .slot_count = %u,\n", view->slots->count);
    fprintf(file, "    .initializer = initializer, .initializer_count = %u, .initializer_error = %u,\n"
        "    .bindings = bindings, .binding_count = %u,\n};\n", view->initializer->instruction_count,
        (unsigned)view->initializer->error_type, (unsigned)MATH_SCALAR_EXPORT_COUNT);
}
