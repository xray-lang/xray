/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_query.c - Owned facts agree with the single source checker
 *
 * KEY CONCEPT:
 *   Queries survive source destruction without granting rejected capabilities.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_source_query_internal.h"
#include "xir/xxir_types.h"
#include "toolchain/xcompiler_session.h"
#include "../test_win_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
static void write_source(const char *path, const char *source) {
    FILE *file = fopen(path, "wb"); CHECK(file);
    size_t length = strlen(source);
    CHECK(fwrite(source, 1, length, file) == length && fclose(file) == 0);
}
static const XrXirSourceDeclaration *declaration(const XrXirSourceView *view, const char *name, uint32_t parent) {
    for (uint32_t i = 0; i < view->declaration_count; ++i) {
        const XrXirSourceDeclaration *decl = &view->declarations[i];
        CHECK(decl->id == i + 1);
        if (decl->parent == parent && !strcmp(decl->name, name)) return decl;
    }
    return NULL;
}
static unsigned references(const XrXirSourceView *view, uint32_t binding, uint32_t target) {
    unsigned count = 0;
    for (uint32_t i = 0; i < view->reference_count; ++i) {
        const XrXirSourceReference *ref = &view->references[i];
        CHECK(ref->declaration && ref->declaration <= view->declaration_count);
        CHECK(ref->target && ref->target <= view->declaration_count);
        if (ref->declaration == binding && ref->target == target) ++count;
    }
    return count;
}
static void generic_facts(const XrXirSourceView *view) {
    const XrXirSourceDeclaration *first = declaration(view, "first", 0), *second = declaration(view, "second", 0);
    const XrXirSourceDeclaration *capture = declaration(view, "capture", 0);
    CHECK(first && second && capture && first->id != second->id);
    CHECK(first->type.known && second->type.known && first->type.type == second->type.type);
    CHECK(first->type.type == (XrXirType) XR_XIR_TYPE_PARAMETER_BASE);
    CHECK(first->type.generic_owner == first->id && second->type.generic_owner == second->id);
    CHECK(first->parameter_count == 1 && first->parameters[0].generic_owner == first->id);
    const XrXirSourceDeclaration *parameter = declaration(view, "value", capture->id); CHECK(parameter);
    CHECK(parameter->kind == XR_XIR_SOURCE_PARAMETER && parameter->type.generic_owner == capture->id);
    CHECK(parameter->range.line == 5 && parameter->range.column == 15 && parameter->range.end_column == 20);
    CHECK(references(view, parameter->id, parameter->id) == 1);
    CHECK(capture->type.generic_owner == capture->id && view->types);
    const XrXirTypeNode *signature = xr_xir_callable_signature(view->types, capture->type.type);
    CHECK(signature && signature->parameter_count == 0 && signature->result == (XrXirType) XR_XIR_TYPE_PARAMETER_BASE);
    unsigned closures = 0, generic_expressions = 0;
    for (uint32_t i = 0; i < view->declaration_count; ++i) {
        const XrXirSourceDeclaration *decl = &view->declarations[i];
        if (decl->parent == capture->id && decl->kind == XR_XIR_SOURCE_FUNCTION) {
            CHECK(decl->type.known && decl->type.type == signature->result);
            CHECK(decl->type.generic_owner == capture->id && decl->type.generic_owner != decl->id);
            CHECK(decl->parameter_count == 0); ++closures;
        }
    }
    for (uint32_t i = 0; i < view->expression_count; ++i) {
        const XrXirSourceExpression *expr = &view->expressions[i];
        CHECK(expr->node && expr->type.known);
        if (expr->range.line == 5 && expr->type.type == (XrXirType) XR_XIR_TYPE_PARAMETER_BASE) {
            CHECK(expr->type.generic_owner == capture->id); ++generic_expressions;
        }
    }
    CHECK(closures == 1 && generic_expressions == 1);
}
static void shadow_and_imports(const XrXirSourceView *view) {
    const XrXirSourceDeclaration *alias = declaration(view, "alias", 0), *module = declaration(view, "lib", 0);
    const XrXirSourceDeclaration *visible = declaration(view, "visible", 0), *hidden = declaration(view, "hidden", 0);
    CHECK(alias && module && visible && hidden);
    CHECK(alias->kind == XR_XIR_SOURCE_IMPORT && alias->target == visible->id);
    CHECK(visible->exported && !hidden->exported && visible->parameter_count == 1);
    CHECK(visible->type.type == XR_XIR_I64 && visible->parameters[0].type == XR_XIR_I64);
    const XrXirSourceDeclaration *parameter = declaration(view, "value", visible->id);
    CHECK(parameter && !parameter->exported);
    CHECK(references(view, alias->id, visible->id) == 1);
    CHECK(references(view, module->id, visible->id) == 2);
    CHECK(references(view, module->id, hidden->id) == 0);
    const XrXirSourceDeclaration *shadow = declaration(view, "shadow", 0); CHECK(shadow);
    unsigned bindings = 0, uses = 0;
    for (uint32_t i = 0; i < view->declaration_count; ++i) {
        const XrXirSourceDeclaration *decl = &view->declarations[i];
        if (decl->parent != shadow->id || strcmp(decl->name, "value")) continue;
        CHECK(decl->type.known && decl->type.type == XR_XIR_I64);
        CHECK(references(view, decl->id, decl->id) == 1); ++bindings; ++uses;
    }
    CHECK(bindings == 2 && uses == 2);
}
static void constructed_facts(const XrXirSourceView *view) {
    CHECK(view->types && view->types->nodes);
    XrXirBudget budget = xr_xir_default_budget();
    CHECK(xr_xir_types_verify(view->types, &budget) == XR_XIR_OK);
    unsigned generic_cells = 0, scalar_cells = 0, callable_cells = 0;
    for (uint32_t i = 0; i < view->types->count; ++i) {
        const XrXirTypeNode *node = &view->types->nodes[i];
        XrXirType type = (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + i);
        CHECK(xr_xir_type_node(view->types, type) == node);
        CHECK(!xr_xir_type_is_array(view->types, type));
        if (node->kind == XR_XIR_TYPE_CALLABLE) {
            CHECK(xr_xir_callable_signature(view->types, type) == node);
            continue;
        }
        CHECK(node->kind == XR_XIR_TYPE_CELL && xr_xir_type_is_cell(view->types, type));
        CHECK(!xr_xir_callable_signature(view->types, type));
        CHECK(!node->parameters && !node->parameter_count && node->result == XR_XIR_UNIT);
        CHECK(xr_xir_cell_element(view->types, type) == node->element);
        if (node->element == (XrXirType) XR_XIR_TYPE_PARAMETER_BASE) {
            CHECK(node->parameter_span == 1); ++generic_cells;
        } else if (node->element == XR_XIR_I64) {
            CHECK(!node->parameter_span); ++scalar_cells;
        } else {
            CHECK(xr_xir_type_is_callable(view->types, node->element));
            CHECK((uint32_t) node->element < (uint32_t) type); ++callable_cells;
        }
    }
    CHECK(generic_cells == 1 && scalar_cells == 1 && callable_cells == 1);
    const char *owners[] = {"mutableCapture", "mutableOther"};
    for (unsigned i = 0; i < 2; ++i) {
        const XrXirSourceDeclaration *owner = declaration(view, owners[i], 0); CHECK(owner);
        const XrXirSourceDeclaration *held = declaration(view, "held", owner->id); CHECK(held);
        CHECK(held->mutable && held->type.type == (XrXirType) XR_XIR_TYPE_PARAMETER_BASE);
        CHECK(held->type.generic_owner == owner->id && owner->type.generic_owner == owner->id);
        CHECK(references(view, held->id, held->id) == 1);
        unsigned closures = 0;
        for (uint32_t d = 0; d < view->declaration_count; ++d) {
            const XrXirSourceDeclaration *nested = &view->declarations[d];
            if (nested->parent == owner->id && nested->kind == XR_XIR_SOURCE_FUNCTION) {
                CHECK(nested->type.type == held->type.type && nested->type.generic_owner == owner->id);
                CHECK(nested->type.generic_owner != nested->id); ++closures;
            }
        }
        CHECK(closures == 1);
    }
}
static const char accepted_source[] =
    "import { visible as alias } from \"./lib\"\r\n"
    "import \"./lib\" as lib\r\n"
    "fn first<T>(value:T)->T { const kept=value; return kept }\r\n"
    "fn second<T>(value:T)->T { return value }\r\n"
    "fn capture<T>(value:T)->fn()->T { return fn()->T { return value } }\r\n"
    "fn shadow(value:i64)->i64 { { const value=2; print(value) }; return value }\r\n"
    "const text=\"\xF0\x9F\x98\x80\"; const answer=alias(7)\r\n"
    "const again=lib.visible(answer)\r\n"
    "const callback=lib.visible\r\n"
    "const result=callback(9)\r\n"
    "fn mutableCapture<T>(value:T)->fn()->T { var held=value; return fn()->T { return held } }\r\n"
    "fn mutableOther<T>(value:T)->fn()->T { var held=value; return fn()->T { return held } }\r\n"
    "fn callableCell(cb:fn(i64)->i64)->fn(i64)->i64 { var held=cb; return fn(x:i64)->i64 { return held(x) } }\r\n"
    "fn scalarCells()->i64 { var left=1; var right=2; return left+right }\r\n";
static XrXirSourceResult accepted(const XrXirSourceRequest *request) {
    write_source(request->entry_path, accepted_source);
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic;
    XrXirStatus status = xr_xir_source_check(request, &result, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "source: %s\n", diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked && result.snapshot);
    const XrXirSourceView *view = xr_xir_source_snapshot_view(result.snapshot); CHECK(view);
    CHECK(view->complete && view->diagnostic.status == XR_XIR_OK && view->module_count == 2);
    generic_facts(view); shadow_and_imports(view); constructed_facts(view);
    const XrXirSourceDeclaration *answer = declaration(view, "answer", 0); CHECK(answer);
    const char *line = strstr(accepted_source, "const text="), *name = strstr(line, "answer="); CHECK(line && name);
    CHECK(answer->range.line == 7 && answer->range.column == (int) (name - line) + 1);
    CHECK(answer->range.end_line == 7 && answer->range.end_column == answer->range.column + 6);
    CHECK(answer->type.known && answer->type.type == XR_XIR_I64);
    const XrXirModule *module = xr_xir_artifact_module(result.checked);
    const XrXirSourceModule *checked_module = &module->declarations->modules[answer->range.module];
    const char *query_identity = view->modules[answer->range.module].identity;
    CHECK(checked_module->name != query_identity);
    CHECK(checked_module->name_length == strlen(query_identity));
    CHECK(!memcmp(checked_module->name, query_identity, checked_module->name_length));
    CHECK(module->types && module->types != view->types && module->types->nodes != view->types->nodes);
    CHECK(module->types->count == view->types->count);
    for (uint32_t i = 0; i < module->types->count; ++i) {
        const XrXirTypeNode *checked = &module->types->nodes[i], *queried = &view->types->nodes[i];
        CHECK(checked->kind == queried->kind && checked->element == queried->element);
        CHECK(checked->result == queried->result && checked->parameter_count == queried->parameter_count);
        CHECK(checked->flags == queried->flags && checked->parameter_span == queried->parameter_span);
        if (checked->parameter_count) {
            CHECK(checked->parameters != queried->parameters);
            for (uint32_t p = 0; p < checked->parameter_count; ++p) {
                CHECK(checked->parameters[p].type == queried->parameters[p].type);
                CHECK(checked->parameters[p].mode == queried->parameters[p].mode);
            }
        }
    }
    return result;
}
static void publication_budgets(XrXirSourceRequest *request) {
    write_source(request->entry_path, "fn identity(value:i64)->i64 { var held=value; return held }\nconst callback=identity\n");
    for (unsigned mode = 0; mode < 2; ++mode) {
        uint64_t low = 0, high = 65536;
        XrXirBudget budget = xr_xir_default_budget();
        request->budget = &budget;
        while (low + 1 < high) {
            uint64_t middle = low + (high - low) / 2;
            if (mode) budget.work = middle; else budget.metadata_bytes = middle;
            XrXirSourceResult result = {0};
            XrXirStatus status = xr_xir_source_check(request, &result, NULL);
            CHECK(status == XR_XIR_OK || status == XR_XIR_BUDGET);
            if (status == XR_XIR_OK) { CHECK(result.checked && result.snapshot); high = middle; }
            else { CHECK(!result.checked && !result.snapshot); low = middle; }
            xr_xir_source_result_free(&result);
        }
        if (mode) budget.work = low; else budget.metadata_bytes = low;
        XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic;
        CHECK(xr_xir_source_check(request, &result, &diagnostic) == XR_XIR_BUDGET);
        CHECK(!result.checked && !result.snapshot);
        CHECK(!strcmp(diagnostic.message, "source query snapshot publication failed"));
    }
    request->budget = NULL;
}
static void multiline_declaration(const XrXirSourceRequest *request) {
    write_source(request->entry_path, "fn\r\nname(value:i64)->i64 { return value }\r\n");
    XrXirSourceResult result = {0};
    CHECK(xr_xir_source_check(request, &result, NULL) == XR_XIR_OK);
    const XrXirSourceView *view = xr_xir_source_snapshot_view(result.snapshot);
    const XrXirSourceDeclaration *name = declaration(view, "name", 0); CHECK(name);
    CHECK(name->range.line == 2 && name->range.column == 1 && name->range.end_line == 2 && name->range.end_column == 5);
    xr_xir_source_result_free(&result);
}
static void failures(XrXirSourceRequest *request) {
    write_source(request->entry_path, "const kept=1\nprint(kept)\nprint(missing)\nconst later=2\n");
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic;
    CHECK(xr_xir_source_check(request, &result, &diagnostic) == XR_XIR_BAD_VALUE);
    CHECK(!result.checked && result.snapshot);
    const XrXirSourceView *view = xr_xir_source_snapshot_view(result.snapshot);
    CHECK(!view->complete && view->diagnostic.status == diagnostic.status && diagnostic.line == 3);
    CHECK(!strcmp(view->diagnostic.message, diagnostic.message));
    const XrXirSourceDeclaration *kept = declaration(view, "kept", 0), *later = declaration(view, "later", 0);
    CHECK(kept && kept->type.known && later && !later->type.known);
    CHECK(references(view, kept->id, kept->id) == 1);
    xr_xir_source_result_free(&result); CHECK(!result.checked && !result.snapshot);
    write_source(request->entry_path, "import \"./lib\" as lib\nconst kept=1\nlib.hidden(1)\n");
    CHECK(xr_xir_source_check(request, &result, &diagnostic) != XR_XIR_OK);
    view = xr_xir_source_snapshot_view(result.snapshot); CHECK(view && !view->complete && !result.checked);
    const XrXirSourceDeclaration *alias = declaration(view, "lib", 0), *hidden = declaration(view, "hidden", 0);
    CHECK(alias && hidden && !alias->target && !references(view, alias->id, hidden->id));
    xr_xir_source_result_free(&result);
    write_source(request->entry_path, "import { hidden } from \"./lib\"\n");
    CHECK(xr_xir_source_check(request, &result, &diagnostic) != XR_XIR_OK);
    view = xr_xir_source_snapshot_view(result.snapshot); CHECK(view && !view->complete && !view->reference_count);
    CHECK(view->declarations[0].kind == XR_XIR_SOURCE_IMPORT && !view->declarations[0].target);
    CHECK(diagnostic.module == 0 && view->diagnostic.module == 0 && strstr(view->modules[0].path, "root.xr"));
    xr_xir_source_result_free(&result);
    write_source(request->entry_path, "const ok=1\n");
    for (unsigned mode = 0; mode < 2; ++mode) {
        XrXirBudget budget = xr_xir_default_budget();
        if (mode) budget.work = 0; else budget.metadata_bytes = 1;
        request->budget = &budget;
        CHECK(xr_xir_source_check(request, &result, &diagnostic) == XR_XIR_BUDGET);
        CHECK(!result.checked && !result.snapshot && diagnostic.status == XR_XIR_BUDGET);
    }
    request->budget = NULL;
    CHECK(xr_xir_source_check(request, &result, &diagnostic) == XR_XIR_OK);
    view = xr_xir_source_snapshot_view(result.snapshot);
    CHECK(view && view->complete && view->declaration_count == 1 && !strcmp(view->declarations[0].name, "ok"));
    xr_xir_source_result_free(&result);
}
static XrXirSourceResult native_array_type_facts(XrXirSourceRequest *request) {
    write_source(request->entry_path,
        "fn first<T>(x:Array<T>)->Array<T> { return x }\n"
        "fn second<T>(x:Array<T>)->Array<T> { return x }\n"
        "fn nested(x:Array<Array<string>>)->Array<Array<string>> { return x }\n");
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_source_check(request, &result, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "native types: %d %s\n", status, diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked && result.snapshot);
    const XrXirSourceView *view = xr_xir_source_snapshot_view(result.snapshot);
    const XrXirSourceDeclaration *array = declaration(view, "Array", 0);
    const XrXirSourceDeclaration *first = declaration(view, "first", 0);
    const XrXirSourceDeclaration *second = declaration(view, "second", 0);
    CHECK(array && first && second && array->kind == XR_XIR_SOURCE_TYPE && array->native_identity == 1);
    CHECK(array->range.module == 1 && array->exported && !array->type.known);
    CHECK(view->module_count == 2 && !strcmp(view->modules[1].identity, "xray-native:prelude/Array"));
    CHECK(!strcmp(view->modules[1].path, "stdlib/types/array.xr"));
    const XrXirSourceDeclaration *binder = declaration(view, "T", array->id);
    CHECK(binder && binder->kind == XR_XIR_SOURCE_TYPE_PARAMETER && binder->type.generic_owner == array->id);
    CHECK(first->type.type == second->type.type && first->type.generic_owner == first->id &&
        second->type.generic_owner == second->id && first->id != second->id);
    CHECK(xr_xir_type_is_array(view->types, first->type.type));
    CHECK(xr_xir_array_element(view->types, first->type.type) == (XrXirType) XR_XIR_TYPE_PARAMETER_BASE);
    CHECK(references(view, array->id, array->id) == 8);
    xr_xir_artifact_free(result.checked); result.checked = NULL;
    return result;
}
static void native_array_type_rejections(XrXirSourceRequest *request) {
    const char *sources[] = {
        "fn bad(x:Array<()>){ }\n", "fn bad(x:Array<i64,string>){ }\n",
        "fn bad<Array>(x:Array<i64>){ }\n",
        "fn bad(x:Array<i64>){ }\nfn Array()->i64 { return 1 }\n",
        "class Array<T> {}\nfn bad(x:Array<i64>){ }\n",
        "fn bad(x:Array<i64>){ }\nclass Array<T> {}\n",
        "struct Array<T> {}\nfn bad(x:Array<i64>){ }\n",
        "fn bad(x:Array<i64>){ }\nstruct Array<T> {}\n",
    };
    for (unsigned i = 0; i < sizeof(sources) / sizeof(sources[0]); ++i) {
        write_source(request->entry_path, sources[i]);
        XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
        XrXirStatus status = xr_xir_source_check(request, &result, &diagnostic);
        if (i == 2) {
            CHECK(status == XR_XIR_BAD_STRUCTURE && !result.snapshot);
            CHECK(strstr(diagnostic.message, "parse"));
        } else {
            CHECK(status == XR_XIR_BAD_TYPE && result.snapshot &&
                !xr_xir_source_snapshot_view(result.snapshot)->complete);
        }
        CHECK(!result.checked);
        if (i >= 4) {
            const XrXirSourceView *view = xr_xir_source_snapshot_view(result.snapshot);
            const XrXirSourceDeclaration *bad = declaration(view, "bad", 0);
            CHECK(i >= 6 ? !bad : (bad && !bad->type.known));
            for (uint32_t d = 0; d < view->declaration_count; ++d)
                CHECK(view->declarations[d].kind != XR_XIR_SOURCE_TYPE || !view->declarations[d].native_identity);
            for (uint32_t r = 0; r < view->reference_count; ++r)
                CHECK(view->references[r].access != XR_XIR_SOURCE_TYPE_USE);
        }
        xr_xir_source_result_free(&result);
    }
}
static void native_array_binding_decisions(XrXirSourceRequest *request, const char *library) {
    write_source(library, "export fn len(value:string)->string { return value }\n");
    write_source(request->entry_path, "import { len } from \"./lib\"\nconst text:string=len(\"named\")\n");
    XrXirSourceResult result = {0};
    CHECK(xr_xir_source_check(request, &result, NULL) == XR_XIR_OK);
    const XrXirSourceView *view = xr_xir_source_snapshot_view(result.snapshot);
    unsigned alias = 0, calls = 0;
    for (uint32_t d = 0; d < view->declaration_count; ++d) {
        CHECK(view->declarations[d].kind != XR_XIR_SOURCE_INTRINSIC);
        if (view->declarations[d].kind == XR_XIR_SOURCE_IMPORT) alias = view->declarations[d].id;
    }
    for (uint32_t r = 0; r < view->reference_count; ++r)
        if (view->references[r].declaration == alias && view->references[r].access == XR_XIR_SOURCE_CALL) ++calls;
    CHECK(alias && calls == 1); xr_xir_source_result_free(&result);
    write_source(request->entry_path, "var values=[1]\nvalues.iterator()\n");
    CHECK(xr_xir_source_check(request, &result, NULL) == XR_XIR_BAD_TYPE);
    view = xr_xir_source_snapshot_view(result.snapshot);
    CHECK(view && !view->complete && !result.checked);
    for (uint32_t d = 0; d < view->declaration_count; ++d) CHECK(view->declarations[d].kind != XR_XIR_SOURCE_MEMBER);
    xr_xir_source_result_free(&result);
    write_source(request->entry_path,
        "var a=[7]\nconst early=read()\nvar index:i64=0\nfn read()->i64 { return a[index] }\n");
    CHECK(xr_xir_source_check(request, &result, NULL) == XR_XIR_OK);
    const XrXirModule *module = xr_xir_artifact_module(result.checked);
    unsigned loads = 0, gets = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        if (function->name_length != 4 || memcmp(function->name, "read", 4)) continue;
        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            const XrXirInstruction *op = &function->instructions[i];
            CHECK(op->op != XR_XIR_SLOT_PLACE);
            if (op->op == XR_XIR_SLOT_LOAD) ++loads;
            if (op->op == XR_XIR_ARRAY_GET) {
                CHECK(i >= 2 && op->args[0] < op->args[1]); ++gets;
            }
        }
    }
    CHECK(loads == 2 && gets == 1); xr_xir_source_result_free(&result);
    write_source(library, "export fn visible(value:i64)->i64 { return value }\nfn hidden(value:i64)->i64 { return value }\n");
}
static void nominal_query_boundary(void) {
    XrXirNominalIdentity identity = {0};
    XrXirNominalTable declarations = {NULL, 1, &identity};
    XrXirTypes types = {NULL, 0, &declarations};
    XrXirSourceView view = {0}; view.types = &types;
    XrXirBudget budget = xr_xir_default_budget(), original = budget;
    XrXirSourceSnapshot *snapshot = (XrXirSourceSnapshot *) (uintptr_t) 1;
    CHECK(xr_xir_source_snapshot_copy(&view, &budget, &snapshot) == XR_XIR_BAD_STAGE && !snapshot);
    CHECK(!memcmp(&budget, &original, sizeof(budget)));
}
static void source_struct_facts(XrXirSourceRequest *request) {
    write_source(request->entry_path, "struct Pair { value:i64\n label:string }\n"
        "var p=Pair{label:\"owned\",value:7}\nconst old=p\np.value=23\n"
        "fn read(p:Pair)->i64{return p.value}\nprint(read(p),old.value,p.label)\n"
        "struct Defaults { n:i64=17; text:string=\"default\" }\nvar d=Defaults{}\n"
        "struct Secret { private n:i64=7; callback:fn()->i64=fn()->i64{"
        "return Secret{n:11,callback:fn()->i64{return 0}}.n} }\n"
        "var defaulted:Defaults\nvar constructed=Defaults()\nvar secret=Secret()\nvar integer:i16\nvar flag:bool\nvar decimal:f32\n"
        "struct Empty{}\nvar empty=Empty()\n");
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_source_check(request, &result, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "struct %u: %s\n", status, diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked && result.snapshot);
    const XrXirSourceView *view = xr_xir_source_snapshot_view(result.snapshot);
    CHECK(view->types && view->types->nominals && view->types->nominals->count == 4);
    const XrXirSourceDeclaration *pair = declaration(view, "Pair", 0);
    CHECK(pair && pair->kind == XR_XIR_SOURCE_TYPE && pair->type.known);
    CHECK(declaration(view, "value", pair->id) && declaration(view, "label", pair->id));
    xr_xir_source_result_free(&result);
    static const char *const invalid[] = {
        "struct S{x:i64}\nvar s=S{}", "struct S{x:i64}\nvar s=S{y:1}",
        "struct S{x:i64;y:i64}\nvar s=S{x:1,x:2}", "struct S{x:i64}\nvar s=S{x:\"bad\"}",
        "struct S{private x:i64}\nvar s=S{x:1}", "struct S{x:i64}\nconst s=S{x:1}\ns.x=2",
        "struct S{const x:i64}\nvar s=S{x:1}\ns.x=2", "struct S{x:S}",
        "struct S{x:i64=\"bad\"}", "struct S<T>{x:T}", "struct S{x:Unknown}"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        write_source(request->entry_path, invalid[i]);
        CHECK(xr_xir_source_check(request, &result, &diagnostic) == XR_XIR_BAD_TYPE && !result.checked);
        xr_xir_source_result_free(&result);
    }
    static const struct { const char *source; XrXirStatus status; } defaults[] = {
        {"struct S{x:i64=missing}", XR_XIR_BAD_VALUE},
        {"struct S{x:i64=caller}\nfn make()->S{const caller=7;return S{}}", XR_XIR_BAD_VALUE},
        {"struct S{x:i64=\"bad\"}\nvar s=S{x:1}", XR_XIR_BAD_TYPE},
        {"struct S{private x:i64=1}\nvar s=S{}", XR_XIR_BAD_TYPE},
        {"struct S{x:i64=1;y:i64}\nvar s=S{}", XR_XIR_BAD_TYPE},
        {"struct S{x:string}\nvar s=S()", XR_XIR_BAD_TYPE},
        {"struct S{x:Array<i64>}\nvar s:S", XR_XIR_BAD_TYPE},
        {"var s:string", XR_XIR_BAD_TYPE}, {"var f:fn()->i64", XR_XIR_BAD_TYPE},
        {"var a:Atomic<i64>", XR_XIR_BAD_TYPE}, {"struct S{x:S}\nvar s:S", XR_XIR_BAD_TYPE},
        {"struct S{x:i64}\nvar s=S(1)", XR_XIR_BAD_TYPE},
        {"struct S{x:i64}\nvar s=S<i64>()", XR_XIR_BAD_TYPE}
    };
    for (size_t i = 0; i < sizeof(defaults) / sizeof(*defaults); ++i) {
        write_source(request->entry_path, defaults[i].source);
        status = xr_xir_source_check(request, &result, &diagnostic);
        if (status != defaults[i].status) fprintf(stderr, "default rejection %zu: %u %s\n", i, status, diagnostic.message);
        CHECK(status == defaults[i].status && !result.checked);
        xr_xir_source_result_free(&result);
    }
    write_source(request->entry_path, "struct S{x:i64}\nvar s:S\n");
    XrXirBudget budget = xr_xir_default_budget(); budget.functions = 2;
    request->budget = &budget;
    CHECK(xr_xir_source_check(request, &result, &diagnostic) == XR_XIR_BUDGET && !result.checked && !result.snapshot);
    CHECK(strstr(diagnostic.message, "default constructor function budget"));
    xr_xir_source_result_free(&result);
    budget.functions = 3;
    CHECK(xr_xir_source_check(request, &result, &diagnostic) == XR_XIR_OK && result.checked);
    xr_xir_source_result_free(&result); request->budget = NULL;
}

int main(void) {
    nominal_query_boundary();
    char directory[XR_TEST_PATH_MAX] = "xir-source-query-XXXXXX", absolute[XR_TEST_PATH_MAX];
    CHECK(xr_test_mkdtemp(directory) && xr_test_realpath_buf(directory, absolute, sizeof(absolute)));
    char root[XR_TEST_PATH_MAX], library[XR_TEST_PATH_MAX];
    CHECK(snprintf(root, sizeof(root), "%s/root.xr", absolute) > 0);
    CHECK(snprintf(library, sizeof(library), "%s/lib.xr", absolute) > 0);
    write_source(library, "export fn visible(value:i64)->i64 { return value }\nfn hidden(value:i64)->i64 { return value }\n");
    XrCompilerSession *session = xr_compiler_session_new(NULL); CHECK(session);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, absolute};
    XrXirSourceRequest request = {session, root, &authority, NULL, NULL};
    source_struct_facts(&request);
    failures(&request);
    publication_budgets(&request);
    multiline_declaration(&request);
    native_array_type_rejections(&request);
    XrXirSourceResult native = native_array_type_facts(&request);
    native_array_binding_decisions(&request, library);
    XrXirSourceResult result = accepted(&request);
    xr_xir_artifact_free(result.checked); result.checked = NULL;
    xr_compiler_session_delete(session);
    const XrXirSourceView *native_view = xr_xir_source_snapshot_view(native.snapshot);
    const XrXirSourceDeclaration *native_array = declaration(native_view, "Array", 0);
    CHECK(native_array && !strcmp(native_view->modules[native_array->range.module].path, "stdlib/types/array.xr"));
    xr_xir_source_result_free(&native);
    CHECK(xr_test_unlink(root) == 0 && xr_test_unlink(library) == 0 && xr_test_rmdir(directory) == 0);
    const XrXirSourceView *view = xr_xir_source_snapshot_view(result.snapshot);
    generic_facts(view); shadow_and_imports(view); constructed_facts(view);
    CHECK(view->modules[0].identity[0] && strstr(view->modules[0].path, "root.xr"));
    xr_xir_source_result_free(&result); xr_xir_source_result_free(&result);
    CHECK(!result.checked && !result.snapshot && !xr_xir_source_snapshot_view(NULL));
    puts("Owned source facts: unified types, lifetime, lexical binding, generic owner, byte ranges and fail-closed publication passed");
    return 0;
}
