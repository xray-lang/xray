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
#include "xir/xxir_generic.h"
#include "xir/xxir_nominal.h"
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
    CHECK(first->generic_parameter_count == 1 && !first->generic_parent && !first->generic_parent_count);
    CHECK(second->generic_parameter_count == 1 && !second->generic_parent && !second->generic_parent_count);
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
    CHECK(xr_xir_types_structure_verify(view->types, &budget) == XR_XIR_OK);
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
    /* Long local names add query-owned bytes without growing executable XIR.
     * Probe a bounded family so verifier cost cannot hide publication failure. */
    for (unsigned mode = 0; mode < 2; ++mode) {
        bool publication_failed = false;
        for (size_t length = 32; length <= 2048 && !publication_failed; length *= 4) {
            char name[2049], source[4352];
            memset(name,'q',length); name[length] = 0;
            int written = snprintf(source,sizeof(source),
                "fn identity(value:i64)->i64 { var %s=value; return %s }\nconst callback=identity\n",name,name);
            CHECK(written > 0 && (size_t)written < sizeof(source));
            write_source(request->entry_path,source);
            uint64_t low = 0, high = 65536;
            XrXirBudget budget = xr_xir_default_budget(); request->budget = &budget;
            if (mode) budget.work = high; else budget.metadata_bytes = high;
            XrXirSourceResult successful = {0}; XrXirSourceDiagnostic upper = {0};
            XrXirStatus status = xr_xir_source_check(request,&successful,&upper);
            if (status != XR_XIR_OK) fprintf(stderr,"publication upper mode=%u name=%zu cap=%llu status=%u: %s\n",
                mode,length,(unsigned long long)high,(unsigned)status,upper.message);
            CHECK(status == XR_XIR_OK && successful.checked && successful.snapshot);
            CHECK(xr_xir_source_snapshot_view(successful.snapshot)->complete);
            xr_xir_source_result_free(&successful);
            while (low + 1 < high) {
                uint64_t middle = low + (high - low) / 2;
                if (mode) budget.work = middle; else budget.metadata_bytes = middle;
                XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
                status = xr_xir_source_check(request,&result,&diagnostic);
                if (status != XR_XIR_OK && status != XR_XIR_BUDGET)
                    fprintf(stderr,"publication probe mode=%u name=%zu cap=%llu status=%u: %s\n",
                        mode,length,(unsigned long long)middle,(unsigned)status,diagnostic.message);
                CHECK(status == XR_XIR_OK || status == XR_XIR_BUDGET);
                if (status == XR_XIR_OK) {
                    CHECK(result.checked && result.snapshot && xr_xir_source_snapshot_view(result.snapshot)->complete);
                    high = middle;
                } else { CHECK(!result.checked && !result.snapshot); low = middle; }
                xr_xir_source_result_free(&result);
            }
            if (mode) budget.work = low; else budget.metadata_bytes = low;
            XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
            CHECK(xr_xir_source_check(request,&result,&diagnostic) == XR_XIR_BUDGET);
            CHECK(!result.checked && !result.snapshot);
            publication_failed = !strcmp(diagnostic.message,"source query snapshot publication failed");
            fprintf(stderr,"publication boundary mode=%u name=%zu fail=%llu pass=%llu stage=%s\n",
                mode,length,(unsigned long long)low,(unsigned long long)high,diagnostic.message);
        }
        CHECK(publication_failed);
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
        if (i >= 6) {
            CHECK(status == XR_XIR_OK && result.checked && result.snapshot);
            const XrXirSourceView *view = xr_xir_source_snapshot_view(result.snapshot);
            const XrXirSourceDeclaration *array = declaration(view, "Array", 0);
            CHECK(array && !array->native_identity && array->kind == XR_XIR_SOURCE_TYPE);
            xr_xir_source_result_free(&result); continue;
        }
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
            CHECK(!bad);
            CHECK(!strcmp(diagnostic.message,"class execution currently requires an explicit final root declaration"));
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
    XrXirTypes types = {NULL, 0, &declarations, NULL};
    XrXirSourceView view = {0}; view.types = &types;
    XrXirBudget budget = xr_xir_default_budget(), original = budget;
    XrXirSourceSnapshot *snapshot = (XrXirSourceSnapshot *) (uintptr_t) 1;
    CHECK(xr_xir_source_snapshot_copy(&view, &budget, &snapshot) == XR_XIR_BAD_STAGE && !snapshot);
    CHECK(!memcmp(&budget, &original, sizeof(budget)));
}
static void source_method_generic_facts(XrXirSourceRequest *request) {
    write_source(request->entry_path,
        "struct C<T>{static choose<U>(outer:T,value:U, transform:fn(T,U)->U=fn(a:T,b:U)->U{return b})->U{return transform(outer,value)}}\n"
        "const value=C<i64>.choose<string>(7,\"ok\")\n");
    XrXirSourceResult result = {0};
    CHECK(xr_xir_source_check(request, &result, NULL) == XR_XIR_OK && result.checked);
    xr_xir_artifact_free(result.checked); result.checked = NULL;
    const XrXirSourceView *view = xr_xir_source_snapshot_view(result.snapshot);
    const XrXirSourceDeclaration *owner = declaration(view, "C", 0); CHECK(owner);
    const XrXirSourceDeclaration *method = declaration(view, "choose", owner->id); CHECK(method);
    CHECK(method->generic_parent == owner->id && method->generic_parent_count == 1 && method->generic_parameter_count == 2);
    CHECK(method->parameter_count == 3 && method->parameters[0].generic_owner == method->id && method->parameters[1].generic_owner == method->id);
    CHECK(method->parameters[0].type == XR_XIR_TYPE_PARAMETER_BASE && method->parameters[1].type == XR_XIR_TYPE_PARAMETER_BASE + 1);
    CHECK(method->type.generic_owner == method->id && method->type.type == XR_XIR_TYPE_PARAMETER_BASE + 1);
    CHECK(!declaration(view, "this", method->id));
    unsigned closures = 0;
    for (uint32_t i = 0; i < view->declaration_count; ++i) {
        const XrXirSourceDeclaration *nested = &view->declarations[i];
        if (nested->parent != method->id || nested->kind != XR_XIR_SOURCE_FUNCTION) continue;
        CHECK(nested->generic_parent == method->id && nested->generic_parent_count == 2 && nested->generic_parameter_count == 2);
        CHECK(nested->type.generic_owner == method->id && nested->type.type == XR_XIR_TYPE_PARAMETER_BASE + 1);
        ++closures;
    }
    CHECK(closures == 1);
    xr_xir_source_result_free(&result);
}
static void source_static_method_facts(XrXirSourceRequest *request) {
    write_source(request->entry_path,
        "struct C<T> { static identity(value:T)->T { return value } }\n"
        "const direct=C<i64>.identity(7)\nconst saved=C<string>.identity\nconst text=saved(\"ok\")\n");
    XrXirSourceResult result = {0};
    CHECK(xr_xir_source_check(request, &result, NULL) == XR_XIR_OK && result.checked);
    const XrXirSourceView *view = xr_xir_source_snapshot_view(result.snapshot);
    const XrXirSourceDeclaration *owner = declaration(view, "C", 0);
    CHECK(owner);
    const XrXirSourceDeclaration *method = declaration(view, "identity", owner->id);
    CHECK(method && method->parameter_count == 1 && method->parameters[0].generic_owner == owner->id);
    CHECK(owner->generic_parameter_count == 1 && !owner->generic_parent && !owner->generic_parent_count);
    CHECK(method->generic_parent == owner->id && method->generic_parent_count == 1 && method->generic_parameter_count == 1);
    CHECK(method->type.generic_owner == owner->id && !declaration(view, "this", method->id));
    const XrXirSourceDeclaration *parameter = declaration(view, "value", method->id);
    CHECK(parameter && parameter->type.generic_owner == owner->id && references(view, parameter->id, parameter->id) == 1);
    unsigned calls = 0, values = 0, types = 0;
    for (uint32_t i = 0; i < view->reference_count; ++i) {
        const XrXirSourceReference *ref = &view->references[i];
        if (ref->target == method->id && ref->access == XR_XIR_SOURCE_CALL) ++calls;
        if (ref->target == method->id && ref->access == XR_XIR_SOURCE_FUNCTION_VALUE) ++values;
        if (ref->target == owner->id && ref->access == XR_XIR_SOURCE_TYPE_USE) ++types;
    }
    CHECK(calls == 1 && values == 1 && types == 2);
    xr_xir_source_result_free(&result);
}
static void source_default_argument_facts(XrXirSourceRequest *request) {
    write_source(request->entry_path,
        "fn choose<T>(value:T, transform:fn(T)->T=fn(item:T)->T { return item })->T { return transform(value) }\n"
        "const selected=choose<i64>(7)\n");
    XrXirSourceResult result = {0};
    CHECK(xr_xir_source_check(request, &result, NULL) == XR_XIR_OK && result.checked);
    const XrXirSourceView *view = xr_xir_source_snapshot_view(result.snapshot);
    const XrXirSourceDeclaration *owner = declaration(view, "choose", 0);
    CHECK(owner && owner->parameter_count == 2 && owner->type.generic_owner == owner->id);
    CHECK(owner->generic_parameter_count == 1 && !owner->generic_parent);
    CHECK(owner->parameters[1].known && owner->parameters[1].generic_owner == owner->id);
    unsigned closures = 0, calls = 0;
    for (uint32_t i = 0; i < view->declaration_count; ++i) {
        const XrXirSourceDeclaration *nested = &view->declarations[i];
        if (nested->parent == owner->id && nested->kind == XR_XIR_SOURCE_FUNCTION) {
            CHECK(nested->parameter_count == 1 && nested->type.generic_owner == owner->id);
            CHECK(nested->generic_parent == owner->id && nested->generic_parent_count == 1 && nested->generic_parameter_count == 1);
            const XrXirSourceDeclaration *item = declaration(view, "item", nested->id);
            CHECK(item && item->type.generic_owner == owner->id);
            CHECK(references(view, item->id, item->id) == 1); ++closures;
        }
        CHECK(strcmp(nested->name, "$argument_default"));
    }
    for (uint32_t i = 0; i < view->reference_count; ++i)
        if (view->references[i].access == XR_XIR_SOURCE_CALL && view->references[i].target == owner->id) ++calls;
    CHECK(closures == 1 && calls == 1);
    xr_xir_source_result_free(&result);
    write_source(request->entry_path, "fn value(x:i64=7)->i64 { return x }\nprint(value())\n");
    XrXirBudget budget = xr_xir_default_budget(); budget.functions = 3; request->budget = &budget;
    CHECK(xr_xir_source_check(request, &result, NULL) == XR_XIR_BUDGET && !result.checked);
    xr_xir_source_result_free(&result); budget.functions = 4;
    CHECK(xr_xir_source_check(request, &result, NULL) == XR_XIR_OK && result.checked);
    xr_xir_source_result_free(&result); request->budget = NULL;
}
static void source_constructor_facts(XrXirSourceRequest *request) {
    write_source(request->entry_path, "struct C<T>{\n const value:T\n constructor(\n value\n ){this.value=value}\n}\nconst c=C<i64>(7)\n");
    XrXirSourceResult result = {0};
    CHECK(xr_xir_source_check(request, &result, NULL) == XR_XIR_OK);
    const XrXirSourceView *view = xr_xir_source_snapshot_view(result.snapshot);
    const XrXirSourceDeclaration *owner = declaration(view, "C", 0);
    CHECK(owner);
    const XrXirSourceDeclaration *constructor = declaration(view, "constructor", owner->id);
    CHECK(constructor && constructor->kind == XR_XIR_SOURCE_FUNCTION && constructor->parameter_count == 1);
    CHECK(constructor->parameters[0].known && constructor->parameters[0].generic_owner == owner->id);
    const XrXirSourceDeclaration *parameter = declaration(view, "value", constructor->id);
    CHECK(parameter && parameter->range.line == 4 && parameter->range.column == 2);
    CHECK(parameter->range.end_line == 4 && parameter->range.end_column == 7);
    unsigned calls = 0;
    for (uint32_t i = 0; i < view->reference_count; ++i) {
        const XrXirSourceReference *reference = &view->references[i];
        if (reference->access == XR_XIR_SOURCE_CALL) {
            CHECK(reference->target == constructor->id && reference->declaration == owner->id);
            ++calls;
        }
    }
    CHECK(calls == 1);
    xr_xir_source_result_free(&result);
}
static void source_struct_facts(XrXirSourceRequest *request) {
    write_source(request->entry_path, "struct Box<T>{value:T}\nstruct Outer<T>{inner:Box<T>}\n"
        "fn wrap<T>(x:T)->Outer<T>{return Outer<T>{inner:Box<T>{value:x}}}\n"
        "var a=wrap<i64>(7)\nvar b=wrap<string>(\"generic\")\nprint(a.inner.value,b.inner.value)\n");
    XrXirSourceResult generic = {0}; XrXirSourceDiagnostic generic_diagnostic = {0};
    XrXirStatus generic_status = xr_xir_source_check(request, &generic, &generic_diagnostic);
    if (generic_status != XR_XIR_OK) fprintf(stderr,"generic struct %u: %s\n",generic_status,generic_diagnostic.message);
    CHECK(generic_status == XR_XIR_OK && generic.checked && generic.snapshot);
    const XrXirSourceView *generic_view = xr_xir_source_snapshot_view(generic.snapshot);
    const XrXirSourceDeclaration *box = declaration(generic_view,"Box",0);
    CHECK(box && box->type.generic_owner == box->id);
    CHECK(declaration(generic_view,"value",box->id)->type.generic_owner == box->id);
    XrXirArtifact *specialized = NULL, *lowered = NULL;
    CHECK(xr_xir_specialize(generic.checked,NULL,&specialized,NULL) == XR_XIR_OK);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(specialized,&target,NULL,&lowered,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(lowered); xr_xir_artifact_free(specialized); xr_xir_source_result_free(&generic);
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
    write_source(request->entry_path,
        "struct S<T>{\n value:T\n get(\n input:T\n )->T{return this.value}\n}\nconst s=S<i64>{value:7};const n=s.get(7);const bound=s.get\n");
    CHECK(xr_xir_source_check(request, &result, &diagnostic) == XR_XIR_OK && result.checked && result.snapshot);
    view = xr_xir_source_snapshot_view(result.snapshot);
    const XrXirSourceDeclaration *method_owner = declaration(view, "S", 0);
    CHECK(method_owner);
    const XrXirSourceDeclaration *method = declaration(view, "get", method_owner->id);
    CHECK(method && method->kind == XR_XIR_SOURCE_FUNCTION && method->parameter_count == 2);
    CHECK(method->type.known && method->type.generic_owner == method_owner->id);
    CHECK(method->parameters[0].known && method->parameters[0].generic_owner == method_owner->id);
    const XrXirSourceDeclaration *input_parameter = declaration(view, "input", method->id);
    CHECK(input_parameter && input_parameter->range.line == 4 && input_parameter->range.column == 2);
    CHECK(input_parameter->range.end_line == 4 && input_parameter->range.end_column == 7);
    const XrXirSourceDeclaration *bound = declaration(view, "bound", 0);
    CHECK(bound && bound->type.known);
    const XrXirTypeNode *bound_type = xr_xir_callable_signature(view->types, bound->type.type);
    CHECK(bound_type && bound_type->parameter_count == 1 && bound_type->parameters[0].type == XR_XIR_I64 && bound_type->result == XR_XIR_I64);
    unsigned bound_references = 0;
    for (uint32_t i = 0; i < view->reference_count; ++i)
        if (view->references[i].target == method->id && view->references[i].access == XR_XIR_SOURCE_FUNCTION_VALUE) ++bound_references;
    CHECK(bound_references == 1);
    xr_xir_source_result_free(&result);
    static const char *const invalid[] = {
        "struct S{x:i64}\nvar s=S{}", "struct S{x:i64}\nvar s=S{y:1}",
        "struct S{x:i64;y:i64}\nvar s=S{x:1,x:2}", "struct S{x:i64}\nvar s=S{x:\"bad\"}",
        "struct S{private x:i64}\nvar s=S{x:1}", "struct S{x:i64}\nconst s=S{x:1}\ns.x=2",
        "struct S{const x:i64}\nvar s=S{x:1}\ns.x=2", "struct S{x:S}",
        "struct S{x:i64=\"bad\"}", "struct S<T>{x:T}\nvar s=S{x:1}", "struct S{x:Unknown}",
        "struct S<T>{x:T}\nvar s=S<i64>{x:\"bad\"}",
        "struct S<T>{x:T}\nvar s:S<i64>", "struct S<T>{x:T=1}",
        "struct S<T>{x:T}\nvar s=S<i64,string>{x:1}",
        "struct S<T:Sendable>{x:T}\nfn bad<T>(x:T)->S<T>{return S<T>{x:x}}"
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

static XrXirSourceResult native_string_facts(XrXirSourceRequest *request) {
    write_source(request->entry_path,
        "const a=\"abc\".contains(\"b\")\nconst b=\"abc\".startsWith(\"a\")\nconst c=\"abc\".endsWith(\"c\")\nconst d=\"abc\".indexOf(\"b\",1)\nconst e=\"abc\".lastIndexOf(\"a\")\n");
    XrXirSourceResult result = {0};
    CHECK(xr_xir_source_check(request, &result, NULL) == XR_XIR_OK);
    const XrXirSourceView *view = xr_xir_source_snapshot_view(result.snapshot);
    const XrXirSourceDeclaration *string = declaration(view, "string", 0);
    CHECK(string && string->kind == XR_XIR_SOURCE_TYPE && string->native_identity == 2);
    CHECK(string->type.known && string->type.type == XR_XIR_STRING && !string->generic_parameter_count);
    CHECK(!strcmp(view->modules[string->range.module].identity, "xray-native:prelude/string"));
    const char *names[] = {"contains", "startsWith", "endsWith", "indexOf", "lastIndexOf"};
    const uint32_t identities[] = {5,15,16,6,7};
    for (unsigned i = 0; i < 5; ++i) {
        const XrXirSourceDeclaration *member = declaration(view,names[i],string->id);
        CHECK(member && member->kind == XR_XIR_SOURCE_MEMBER && member->native_identity == identities[i]);
        CHECK(member->exported && !member->mutable && member->type.known && member->type.type == (i<3 ? XR_XIR_BOOL : XR_XIR_I64));
        CHECK(member->parameter_count == (i==3 ? 2u : 1u) && member->parameters[0].known && member->parameters[0].type == XR_XIR_STRING);
        if (i==3) CHECK(member->parameters[1].known && member->parameters[1].type == XR_XIR_I64);
        CHECK(!strcmp(member->signature,i<3 ? "(search: string) -> bool" : i==3 ? "(search: string, start?: i64) -> i64" : "(search: string) -> i64"));
        CHECK(references(view,member->id,member->id)==1);
    }
    return result;
}
static void source_enum_facts(XrXirSourceRequest *request) {
    write_source(request->entry_path,
        "enum Choice<T> { Empty, Some { value:T }, Other { value:string } }\n"
        "const v=Choice<i64>.Some { value:42 }\nprint(v.ordinal)\n"
        "const got=match(v){Choice.Some{value:item}->item,Choice.Other{value:ignored}->0,Choice.Empty->-1}\n");
    XrXirSourceResult result={0};
    CHECK(xr_xir_source_check(request,&result,NULL)==XR_XIR_OK && result.checked && result.snapshot);
    xr_xir_artifact_free(result.checked); result.checked=NULL;
    write_source(request->entry_path,"print(0)\n");
    const XrXirSourceView *view=xr_xir_source_snapshot_view(result.snapshot);
    const XrXirSourceDeclaration *owner=declaration(view,"Choice",0); CHECK(owner);
    CHECK(view->types && view->types->nominals && view->types->nominals->count == 1);
    const XrXirNominalDeclaration *nominal = view->types->nominals->declarations;
    CHECK(nominal->kind == XR_XIR_NOMINAL_ENUM && nominal->variant_count == 3);
    const char *variant_names[] = {"Empty", "Some", "Other"};
    for (uint32_t i = 0; i < 3; ++i) {
        CHECK(nominal->variants[i].name.length == strlen(variant_names[i]));
        CHECK(!memcmp(nominal->variants[i].name.bytes, variant_names[i], strlen(variant_names[i])));
        CHECK(nominal->variants[i].field_count == (i ? 1u : 0u));
    }
    XrXirBudget budget = xr_xir_default_budget();
    CHECK(xr_xir_types_structure_verify(view->types, &budget) == XR_XIR_OK);
    const XrXirSourceDeclaration *some=declaration(view,"Some",owner->id);
    const XrXirSourceDeclaration *other=declaration(view,"Other",owner->id);
    const XrXirSourceDeclaration *ordinal=declaration(view,"ordinal",owner->id);
    CHECK(some && other && ordinal && some->id!=other->id);
    const XrXirSourceDeclaration *value=declaration(view,"value",some->id);
    const XrXirSourceDeclaration *text=declaration(view,"value",other->id);
    CHECK(value && text && value->id!=text->id);
    CHECK(value->type.known && value->type.type==XR_XIR_TYPE_PARAMETER_BASE && value->type.generic_owner==owner->id);
    CHECK(text->type.known && text->type.type==XR_XIR_STRING && !text->type.generic_owner);
    CHECK(ordinal->kind==XR_XIR_SOURCE_INTRINSIC && ordinal->type.known && ordinal->type.type==XR_XIR_I64);
    CHECK(!ordinal->range.line && !ordinal->range.column);
    const XrXirSourceDeclaration *item=declaration(view,"item",0), *ignored=declaration(view,"ignored",0);
    CHECK(item && ignored && item->type.known && item->type.type==XR_XIR_I64 && !item->mutable);
    CHECK(ignored->type.known && ignored->type.type==XR_XIR_STRING && !ignored->mutable);
    CHECK(references(view,some->id,some->id)==2 && references(view,value->id,value->id)==2);
    CHECK(references(view,ordinal->id,ordinal->id)==1 && references(view,text->id,text->id)==1);
    xr_xir_source_result_free(&result);
}
static void nested_pattern_facts(XrXirSourceRequest *request) {
    write_source(request->entry_path,
        "enum Inner<T>{A,B{value:T}}\nenum Outer<T>{Wrap{inner:Inner<T>,flag:bool}}\n"
        "fn pick(v:Outer<string>)->string{return match(v){Outer.Wrap{inner:Inner.A}->\"empty\","
        "Outer.Wrap{inner:Inner.B{value:item},flag:true}->item,"
        "Outer.Wrap{inner:Inner.B{value:other},flag:false}->other}}\n");
    XrXirSourceResult result={0};
    CHECK(xr_xir_source_check(request,&result,NULL)==XR_XIR_OK && result.checked && result.snapshot);
    xr_xir_artifact_free(result.checked); result.checked=NULL;
    write_source(request->entry_path,"print(0)\n");
    const XrXirSourceView *view=xr_xir_source_snapshot_view(result.snapshot);
    const XrXirSourceDeclaration *inner=declaration(view,"Inner",0), *outer=declaration(view,"Outer",0);
    const XrXirSourceDeclaration *pick=declaration(view,"pick",0); CHECK(inner && outer && pick);
    const XrXirSourceDeclaration *b=declaration(view,"B",inner->id), *wrap=declaration(view,"Wrap",outer->id);
    CHECK(b && wrap);
    const XrXirSourceDeclaration *field=declaration(view,"value",b->id), *flag=declaration(view,"flag",wrap->id);
    const XrXirSourceDeclaration *item=declaration(view,"item",pick->id), *other=declaration(view,"other",pick->id);
    CHECK(field && flag && item && other && item->id!=other->id);
    CHECK(item->type.known && item->type.type==XR_XIR_STRING && !item->mutable);
    CHECK(other->type.known && other->type.type==XR_XIR_STRING && !other->mutable);
    CHECK(references(view,b->id,b->id)==2 && references(view,field->id,field->id)==2);
    CHECK(references(view,wrap->id,wrap->id)==3 && references(view,flag->id,flag->id)==2);
    CHECK(references(view,item->id,item->id)==1 && references(view,other->id,other->id)==1);
    xr_xir_source_result_free(&result);
}
static void range_pattern_facts(XrXirSourceRequest *request) {
    write_source(request->entry_path,
        "enum E{A{x:i8}}\nfn pick(v:E)->i64{return match(v){\n"
        "E.A{x:-128..0}->1,\nE.A{x:0..=127}->2}}\n");
    XrXirSourceResult result={0};
    CHECK(xr_xir_source_check(request,&result,NULL)==XR_XIR_OK && result.checked && result.snapshot);
    xr_xir_artifact_free(result.checked); result.checked=NULL;
    write_source(request->entry_path,"print(0)\n");
    const XrXirSourceView *view=xr_xir_source_snapshot_view(result.snapshot);
    unsigned endpoints[2]={0};
    for (uint32_t i=0;i<view->expression_count;++i) {
        const XrXirSourceExpression *expr=&view->expressions[i];
        if (expr->range.line>=3 && expr->range.line<=4 && expr->type.known && expr->type.type==XR_XIR_I8) {
            CHECK(!expr->type.generic_owner); ++endpoints[expr->range.line-3];
        }
    }
    CHECK(endpoints[0]==2 && endpoints[1]==2);
    xr_xir_source_result_free(&result);
}
static void alternative_pattern_facts(XrXirSourceRequest *request) {
    write_source(request->entry_path,
        "enum E{A{x:string},B{x:string}}\nfn pick(v:E)->string{return match(v){\n"
        "E.A{x:item},\nE.B{x:item} if(item==\"yes\")->item,\n_->\"no\"}}\n");
    XrXirSourceResult result={0};
    CHECK(xr_xir_source_check(request,&result,NULL)==XR_XIR_OK && result.checked && result.snapshot);
    xr_xir_artifact_free(result.checked); result.checked=NULL;
    write_source(request->entry_path,"print(0)\n");
    const XrXirSourceView *view=xr_xir_source_snapshot_view(result.snapshot);
    const XrXirSourceDeclaration *pick=declaration(view,"pick",0); CHECK(pick);
    const XrXirSourceDeclaration *item=declaration(view,"item",pick->id); CHECK(item);
    CHECK(item->kind==XR_XIR_SOURCE_BINDING && item->type.known && item->type.type==XR_XIR_STRING && !item->mutable);
    unsigned declarations=0,reads=0,writes=0;
    for (uint32_t i=0;i<view->declaration_count;++i)
        if (view->declarations[i].parent==pick->id && !strcmp(view->declarations[i].name,"item")) ++declarations;
    for (uint32_t i=0;i<view->reference_count;++i) {
        const XrXirSourceReference *ref=&view->references[i];
        if (ref->target!=item->id) continue;
        CHECK(ref->declaration==item->id && ref->range.line==4);
        if (ref->access==XR_XIR_SOURCE_WRITE) ++writes;
        else { CHECK(ref->access==XR_XIR_SOURCE_READ); ++reads; }
    }
    CHECK(declarations==1 && reads==2 && writes==1 && item->range.line==3);
    xr_xir_source_result_free(&result);
}
static void unreachable_pattern_facts(XrXirSourceRequest *request) {
    write_source(request->entry_path,"fn f(v:f32)->i64{return match(v){\n_,\n16777216->1}}\n");
    XrXirSourceResult result={0};
    CHECK(xr_xir_source_check(request,&result,NULL)==XR_XIR_OK && result.checked && result.snapshot);
    xr_xir_artifact_free(result.checked); result.checked=NULL;
    write_source(request->entry_path,"print(0)\n");
    const XrXirSourceView *view=xr_xir_source_snapshot_view(result.snapshot); unsigned facts=0;
    for (uint32_t i=0;i<view->expression_count;++i) {
        const XrXirSourceExpression *expr=&view->expressions[i];
        if (expr->range.line==3 && expr->type.known && expr->type.type==XR_XIR_F32) ++facts;
    }
    CHECK(facts==1); xr_xir_source_result_free(&result);
}
#include "xir_source_interface_cases.h"
#include "xir_source_witness_cases.h"
#include "xir_source_witness_inheritance_cases.h"
#include "xir_source_enum_witness_cases.h"
#include "xir_witness_provenance_cases.h"
#include "xir_source_generic_requirement_cases.h"
#include "xir_source_enum_identity_cases.h"
#include "xir_source_requirement_value_cases.h"
#include "xir_source_result_inference_cases.h"
#include "xir_source_contextual_lambda_cases.h"
#include "xir_source_requirement_inference_cases.h"
#include "xir_source_ordinary_inference_cases.h"
#include "xir_own_where_source_access_cases.h"
#include "xir_source_inference_equivalence_cases.h"
#include "xir_source_method_where_cases.h"
#include "xir_generic_witness_provenance_cases.h"
#include "xir_source_requirement_value_boundaries.h"
#include "xir_source_requirement_value_provenance.h"
#include "xir_source_class_cases.h"
#include "xir_generic_storage_cases.h"
#include "xir_source_dependency_ready_cases.h"
#include "xir_source_iteration_cases.h"
#include "xir_source_unit_context_cases.h"
#include "xir_source_unit_local_cases.h"
int main(void) {
    generic_storage_cases();
    nominal_query_boundary();
    char directory[XR_TEST_PATH_MAX] = "xir-source-query-XXXXXX", absolute[XR_TEST_PATH_MAX];
    CHECK(xr_test_mkdtemp(directory) && xr_test_realpath_buf(directory, absolute, sizeof(absolute)));
    char root[XR_TEST_PATH_MAX], library[XR_TEST_PATH_MAX];
    CHECK(snprintf(root, sizeof(root), "%s/root.xr", absolute) > 0);
    CHECK(snprintf(library, sizeof(library), "%s/lib.xr", absolute) > 0);
    write_source(library, "export fn visible(value:i64)->i64 { return value }\nfn hidden(value:i64)->i64 { return value }\n");
    XrCompilerSession *session = xr_compiler_session_new(NULL); CHECK(session);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, absolute};
    XrXirSourceRequest request = {session, root, &authority, NULL, NULL, NULL};
    source_dependency_ready_cases(&request);
    source_unit_context_cases(&request);
    source_unit_local_cases(&request);
    source_class_definition_cases(&request);
    source_iteration_definition_cases(&request);
    source_generic_requirement_positive(&request);
    source_requirement_value_positive(&request);
    source_enum_identity_cases(&request);

    source_requirement_value_negative(&request);
    source_result_inference_cases(&request);
    source_generic_requirement_rejections(&request);
    source_requirement_inference_cases(&request);
    source_ordinary_inference_cases(&request);
    source_contextual_lambda_cases(&request);
    own_where_source_access_cases(&request);
    own_where_self_bound_cases(&request);
    source_inference_equivalence_cases(&request);
    source_inference_phantom_cases(&request);
    source_interface_method_where_positive(&request);
    source_interface_method_where_rejections(&request);
    generic_witness_provenance_cases(&request);
    source_requirement_value_boundaries(&request);
    source_requirement_value_provenance(&request);
    source_witness_cases(&request);
    source_witness_inheritance_cases(&request);
    source_enum_witness_cases(&request);
    witness_provenance_cases(&request);
    source_interface_facts(&request);
    source_struct_facts(&request);
    source_enum_facts(&request);
    nested_pattern_facts(&request);
    range_pattern_facts(&request);
    alternative_pattern_facts(&request);
    unreachable_pattern_facts(&request);
    source_constructor_facts(&request);
    source_default_argument_facts(&request);
    source_static_method_facts(&request);
    source_method_generic_facts(&request);
    failures(&request);
    publication_budgets(&request);
    multiline_declaration(&request);
    native_array_type_rejections(&request);
    XrXirSourceResult strings = native_string_facts(&request);
    XrXirSourceResult native = native_array_type_facts(&request);
    native_array_binding_decisions(&request, library);
    XrXirSourceResult result = accepted(&request);
    xr_xir_artifact_free(result.checked); result.checked = NULL;
    xr_compiler_session_delete(session);
    const XrXirSourceView *string_view = xr_xir_source_snapshot_view(strings.snapshot);
    const XrXirSourceDeclaration *string_decl = declaration(string_view, "string", 0);
    CHECK(string_decl && !strcmp(string_view->modules[string_decl->range.module].path, "stdlib/types/string.xr"));
    CHECK(declaration(string_view, "contains", string_decl->id)->type.type == XR_XIR_BOOL);
    CHECK(declaration(string_view, "indexOf", string_decl->id)->parameters[1].type == XR_XIR_I64);
    xr_xir_source_result_free(&strings);
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
