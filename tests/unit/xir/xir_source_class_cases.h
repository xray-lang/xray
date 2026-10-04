/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_class_cases.h - Source class identity and constructor obligations
 *
 * KEY CONCEPT:
 *   Definition checks precede every executable specialization, even for unused bodies.
 */
#ifndef XIR_SOURCE_CLASS_CASES_H
#define XIR_SOURCE_CLASS_CASES_H
#include "xir/xxir_class.h"
static const XrXirNominalDeclaration *source_class_case_nominal(const XrXirSourceView *view, XrXirType type) {
    const XrXirTypeNode *node = xr_xir_type_node(view->types, type);
    CHECK(node && node->kind == XR_XIR_TYPE_NOMINAL && view->types->nominals &&
        view->types->nominals->declarations && node->nominal.declaration < view->types->nominals->count);
    return &view->types->nominals->declarations[node->nominal.declaration];
}
static unsigned source_class_case_ops(const XrXirFunction *function, XrXirOp op) {
    unsigned count = 0;
    for (uint32_t i = 0; i < function->instruction_count; ++i) count += function->instructions[i].op == op;
    return count;
}
static void source_class_supported_query_facts(const XrXirSourceResult *result, uint32_t index) {
    if (index != 0 && index != 4 && index != 10 && index != 18 && index != 20 &&
        index != 22 && index != 28 && index != 29 && index != 30) return;
    const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(result->snapshot);
    CHECK(view && view->complete && view->diagnostic.status == XR_XIR_OK);
    const char *name = index == 0 ? "Container" : index == 10 ? "Box" : "C";
    const XrXirSourceDeclaration *owner = declaration(view, name, 0); CHECK(owner && owner->type.known);
    CHECK(!owner->native_identity && source_class_case_nominal(view, owner->type.type)->kind == XR_XIR_NOMINAL_CLASS);
    const char *field_name = index == 0 ? "items" : index == 4 || index == 10 ? "value" : "x";
    const XrXirSourceDeclaration *field = declaration(view, field_name, owner->id);
    CHECK(field && field->kind == XR_XIR_SOURCE_MEMBER && field->type.known && field->mutable);
    if (index == 0) {
        CHECK(owner->generic_parameter_count == 1 && field->type.generic_owner == owner->id);
        CHECK(xr_xir_type_is_array(view->types, field->type.type));
        CHECK(xr_xir_array_element(view->types, field->type.type) == XR_XIR_TYPE_PARAMETER_BASE);
    } else if (index == 4) {
        CHECK(source_class_case_nominal(view, field->type.type)->kind == XR_XIR_NOMINAL_STRUCT);
        const XrXirSourceDeclaration *record = declaration(view, "R", 0), *inner = declaration(view, "Inner", 0);
        CHECK(record && inner);
        const XrXirSourceDeclaration *identity = declaration(view, "value", record->id);
        CHECK(identity && identity->type.known && identity->type.type == inner->type.type);
        CHECK(source_class_case_nominal(view, identity->type.type)->kind == XR_XIR_NOMINAL_CLASS);
    } else if (index == 10) {
        CHECK(field->type.type == XR_XIR_TYPE_PARAMETER_BASE && field->type.generic_owner == owner->id);
        const XrXirSourceDeclaration *unused = declaration(view, "unused", 0); CHECK(unused);
        const XrXirSourceDeclaration *parameter = declaration(view, "b", unused->id); CHECK(parameter && parameter->type.known);
        const XrXirTypeNode *application = xr_xir_type_node(view->types, parameter->type.type);
        CHECK(application && application->kind == XR_XIR_TYPE_NOMINAL && application->nominal.argument_count == 1);
        XrXirType array = application->nominal.arguments[0];
        CHECK(xr_xir_type_is_array(view->types, array) && xr_xir_array_element(view->types, array) == XR_XIR_BOOL);
    } else if (index == 22) {
        CHECK(xr_xir_type_is_array(view->types, field->type.type));
        XrXirType inner = xr_xir_array_element(view->types, field->type.type);
        CHECK(xr_xir_type_is_array(view->types, inner) && xr_xir_array_element(view->types, inner) == XR_XIR_I64);
    } else if (index == 30) {
        const XrXirSourceDeclaration *array = declaration(view, "Array", 0); CHECK(array && !array->native_identity);
        CHECK(array->generic_parameter_count == 1 && source_class_case_nominal(view, array->type.type)->kind == XR_XIR_NOMINAL_CLASS);
        CHECK(!xr_xir_type_is_array(view->types, field->type.type));
        const XrXirTypeNode *application = xr_xir_type_node(view->types, field->type.type);
        const XrXirTypeNode *generic = xr_xir_type_node(view->types, array->type.type);
        CHECK(application && generic && application->kind == XR_XIR_TYPE_NOMINAL &&
            application->nominal.declaration == generic->nominal.declaration && application->nominal.argument_count == 1 &&
            application->nominal.arguments[0] == XR_XIR_I64);
        CHECK(view->module_count == 1);
        for (uint32_t d = 0; d < view->declaration_count; ++d) CHECK(!view->declarations[d].native_identity);
    } else {
        const XrXirModule *module = xr_xir_compile_artifact_module(result->checked); CHECK(module && module->declarations);
        const XrXirTypeNode *type = xr_xir_type_node(view->types, owner->type.type); CHECK(type);
        uint32_t nominal_owner = type->nominal.declaration + 1;
        if (index == 18 || index == 20) {
            CHECK(field->type.type == XR_XIR_I64);
            unsigned constructors = 0, defaults = 0; uint32_t initializer = UINT32_MAX, constructor = UINT32_MAX;
            for (uint32_t f = 0; f < module->function_count; ++f) {
                const XrXirFunction *function = &module->functions[f];
                const XrXirFunctionIdentity *identity = &module->declarations->functions[f];
                if (identity->nominal_owner != nominal_owner) continue;
                if (identity->method_kind == XR_XIR_CONSTRUCTOR) {
                    ++constructors; constructor = f; CHECK(identity->promises & XR_XIR_FUNCTION_NO_SUSPEND);
                    if (index == 18) CHECK(function->name_length == 8 && !memcmp(function->name, "$default", 8));
                }
                if (function->name_length == 14 && !memcmp(function->name, "$field_default", 14)) {
                    ++defaults; initializer = f; CHECK(function->result == XR_XIR_I64);
                    unsigned ones = 0;
                    for (uint32_t i = 0; i < function->instruction_count; ++i)
                        ones += function->instructions[i].op == XR_XIR_CONST_INT && function->instructions[i].immediate == 1;
                    CHECK(ones == 1);
                }
            }
            CHECK(constructors == 1 && constructor != UINT32_MAX && source_class_case_ops(&module->functions[constructor], XR_XIR_CLASS_NEW) == 1);
            if (index == 18) CHECK(!defaults && !declaration(view, "constructor", owner->id));
            else {
                CHECK(defaults == 1 && initializer != UINT32_MAX && declaration(view, "constructor", owner->id));
                unsigned calls = 0;
                const XrXirFunction *function = &module->functions[constructor];
                for (uint32_t i = 0; i < function->instruction_count; ++i)
                    calls += function->instructions[i].op == XR_XIR_CALL && function->instructions[i].immediate == initializer;
                CHECK(calls == 1);
            }
        } else {
            CHECK(xr_xir_type_is_array(view->types, field->type.type) && xr_xir_array_element(view->types, field->type.type) == XR_XIR_I64);
            const XrXirSourceDeclaration *bad = declaration(view, "bad", owner->id); CHECK(bad && bad->kind == XR_XIR_SOURCE_FUNCTION);
            unsigned methods = 0, writes = 0;
            for (uint32_t f = 0; f < module->function_count; ++f) {
                const XrXirFunction *function = &module->functions[f];
                if (module->declarations->functions[f].nominal_owner != nominal_owner ||
                    function->name_length != 3 || memcmp(function->name, "bad", 3)) continue;
                ++methods; CHECK(source_class_case_ops(function, XR_XIR_OBJECT_PLACE) == 1);
                CHECK(source_class_case_ops(function, XR_XIR_FIELD_PLACE) == 1);
                CHECK(source_class_case_ops(function, index == 28 ? XR_XIR_ARRAY_SET : XR_XIR_ARRAY_PUSH) == 1);
            }
            for (uint32_t i = 0; i < view->reference_count; ++i)
                writes += view->references[i].target == field->id && view->references[i].access == XR_XIR_SOURCE_READ_WRITE;
            CHECK(methods == 1 && writes >= 1);
        }
    }
}
static void source_class_definition_cases(XrXirSourceRequest *request) {
    static const struct { const char *source; unsigned status; const char *message; } cases[]={
    {"struct R{n:i64;text:string}\nfinal class Container<T>{items:Array<T> constructor(v:Array<T>){this.items=v}}\nfn unused(c:Container<R>){}",0,""},
    {"struct R{n:i64;text:string;tail:i64}\nfinal class C{value:R constructor(value:R){this.value=value} get()->R{return this.value}}",0,""},
    {"enum E{None,Full{value:i64,text:string}}\nfinal class C{value:E constructor(value:E){this.value=value} get()->E{return this.value}}",0,""},
    {"struct R{callback:fn()->i64}\nfinal class C{value:R constructor(value:R){this.value=value}}",3,"class field carrier is not implemented"},
    {"final class Inner{constructor(){}}\nstruct R{value:Inner}\nfinal class C{value:R constructor(value:R){this.value=value}}",0,""},
    {"final class Box<T>{value:T constructor(v:T){this.value=v} get()->T{return this.value}}",0,""},
    {"final class Box<T>{value:T constructor(v:T){this.value=v}}\nfn unused<T>(v:T)->Box<T>{return Box<T>(v)}",0,""},
    {"final class Container<T>{items:Array<T> constructor(v:Array<T>){this.items=v} get()->Array<T>{return this.items}}",0,""},
    {"final class Box<T>{value:T constructor(v:T){this.value=v} bad()->i64{return this.value.missing()}}",3,"receiver has no declared interface requirements"},
    {"final class Box<T>{private value:T constructor(v:T){this.value=v}}\nfn unused(b:Box<i64>)->i64{return b.value}",3,"field access is not permitted"},
    {"final class Box<T>{value:T constructor(v:T){this.value=v}}\nfn unused(b:Box<Array<bool>>){}",0,""},
    {"struct Pair{value:i64}\nfinal class Box<T>{value:T constructor(v:T){this.value=v}}\nfn unused(b:Box<Pair>){}",0,""},
    {"final class Box<T>{const value:T constructor(v:T){this.value=v;this.value=v}}",4,"write may overwrite already initialized const storage"},
    {"final class Box<T>{value:T constructor(v:T){}}",4,"read requires storage initialized on every incoming path"},
    {"final class Box<T>{value:T constructor(v:T){this.value=v}}\nfn unused(b:Box<Slice<u8>>){}",3,"name does not resolve to an admitted nominal type"},
    {"final class Box<T>{value:T constructor(v:T){this.value=v}}\nfn unused(b:Box<MutSlice<u8>>){}",3,"name does not resolve to an admitted nominal type"},
    {"class C {x:i64 constructor(){this.x=1}}",0,""},
    {"class P {x:i64 constructor(){this.x=1}}\nclass C : P {constructor(){}}",XR_XIR_BAD_STRUCTURE,"module graph build failed"},
    {"final class C{x:i64}",0,""},
    {"final class C{x:Array<i64> constructor(){this.x=[1]}}",0,""},
    {"final class C{x:i64=1 constructor(){}}",0,""},
    {"final class C{x:Array<string> constructor(x:Array<string>){this.x=x} get()->Array<string>{return this.x}}",0,""},
    {"final class C{x:Array<Array<i64>> constructor(x:Array<Array<i64>>){this.x=x}}",0,""},
    {"final class C<T>{x:T constructor(x:T){this.x=x}}",0,""},
    {"final class C{x:Array<i64> constructor(){}}",4,"read requires storage initialized on every incoming path"},
    {"final class C{const x:Array<i64> constructor(){this.x=[1];this.x=[2]}}",4,"write may overwrite already initialized const storage"},
    {"final class C{private x:Array<i64> constructor(){this.x=[1]}}\nfn unused(c:C)->Array<i64>{return c.x}",3,"field access is not permitted"},
    {"final class C{const x:Array<i64> constructor(){this.x=[1]} bad(){this.x=[2]}}",3,"field access is not permitted"},
    {"final class C{x:Array<i64> constructor(){this.x=[1]} bad(){this.x[0]=2}}",0,""},
    {"final class C{x:Array<i64> constructor(){this.x=[1]} bad(){this.x.push(2)}}",0,""},
    {"final class Array<T>{constructor(){}};final class C{x:Array<i64> constructor(x:Array<i64>){this.x=x}}",0,""},
    {"final class C{private x:i64 constructor(){this.x=1}}\nfn unused(c:C)->i64{return c.x}",3,"field access is not permitted"},
    {"final class C{const x:i64 constructor(){this.x=1} bad(){this.x=2}}",3,"field access is not permitted"},
    {"final class C{x:i64 constructor(){}}",4,"read requires storage initialized on every incoming path"},
    {"final class C{const x:i64 constructor(b:bool){if(b){this.x=1}}}",4,"read requires storage initialized on every incoming path"},
    {"final class C{const x:i64 constructor(){this.x=1;this.x=2}}",4,"write may overwrite already initialized const storage"},
    {"final class C{x:i64 constructor(){const n=this.x;this.x=n}}",4,"read requires storage initialized on every incoming path"},
    {"final class C{x:i64 constructor(){this.x=1;const n=this}}",3,"class this cannot escape before constructor completion"},
    {"final class C{x:i64 constructor(){this.x=1;const cb=fn()->i64{return this.x}}}",3,"class this cannot be captured during construction"},
    {"final class C{x:i64 constructor(){this.x=1;defer {this.x=2}}}",3,"class constructor cleanup cannot capture unpublished this"},
    {"final class C{const x:i64 constructor(b:bool){if(b){this.x=1}else{this.x=2}}}",0,""},
    {"final class C{x:i64 constructor(){this.x=1} static bad()->i64{return this.x}}",4,"name is not an initialized value"},
    {"final class C{x:i64 private constructor(){this.x=1}}\nfn bad()->C{return C()}",3,"constructor arity or authority mismatch"},
    {"final class C<T>{x:i64 constructor(x:i64){this.x=x} get()->i64{return this.x}}\nfn read()->i64{return C<string>(41).get()}",0,""},
    {"final class C{x:i64 constructor(){this.x=1;Coro.yield()}}",3,"declared no_suspend function may suspend or call an unqualified callable"},
    {"final class C{constructor(){}}\nfn bad(c:C){c=C()}",3,"assignment requires a mutable binding"},
    {"final class C{constructor(){}}\nfn missing<T>(value:T)->i64{return value.get()}",3,"receiver has no declared interface requirements"},
    {"final class C{x:i64 constructor(){this.x=1}}\nfn send<T:Sendable>(x:T){}\nfn bad(){send<C>(C())}",3,"type argument does not prove the declared constraint"},
    {"final class Array<T>{constructor(){}}\nfn typed(x:Array<i64>){}",0,""},
    {"final class Array<T>{constructor(){}}\nfn wrong(x:Array<i64>)->i64{return x.len()}",3,"unknown struct field"},
    {"class P {x:i64 constructor(){this.x=1}}\nclass C extends P {constructor(){}}",XR_XIR_BAD_TYPE,"class execution currently requires a root declaration without inheritance"},
    };
    bool matched = true;
    for (uint32_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        write_source(request->entry_path,cases[i].source);
        XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_source_check(request, &result, &diagnostic, NULL);
        if ((unsigned)status!=cases[i].status || strcmp(diagnostic.message,cases[i].message)) {
            fprintf(stderr,"class case %u: %u %s\n",i,status,diagnostic.message);
            matched = false;
        }
        CHECK((result.checked!=NULL)==(status==XR_XIR_OK));
        if (status == XR_XIR_OK) CHECK(result.snapshot && xr_xir_compile_source_snapshot_view(result.snapshot)->complete);
        else CHECK(!result.snapshot);
        if (status == XR_XIR_OK) source_class_supported_query_facts(&result, i);
        xr_xir_compile_source_result_free(&result);
    }
    CHECK(matched);
    const char *root=request->authority->physical_root;
    size_t capacity=strlen(root)+32;char *library=malloc(capacity);CHECK(library);
    CHECK(snprintf(library,capacity,"%s/class_private.xr",root)>0);
    write_source(library,"export final class Hidden{private n:i64 private constructor(){this.n=41} static make()->Hidden{return Hidden()}}\n");
    const char *cross[]={
        "import {Hidden} from \"./class_private\"\nfn unused(x:Hidden)->i64{return x.n}\n",
        "import {Hidden} from \"./class_private\"\nfn unused()->Hidden{return Hidden()}\n"
    };
    const char *messages[]={"field access is not permitted","constructor arity or authority mismatch"};
    for (uint32_t i=0;i<2;++i) {
        write_source(request->entry_path,cross[i]);
        XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
        CHECK(xr_xir_compile_source_check(request, &result, &diagnostic, NULL)==XR_XIR_BAD_TYPE && !result.checked && !result.snapshot);
        CHECK(!strcmp(diagnostic.message,messages[i]));xr_xir_compile_source_result_free(&result);
    }
    CHECK(!remove(library));free(library);
}
#endif // XIR_SOURCE_CLASS_CASES_H
