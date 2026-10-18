/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_generations.c - Exact Source records over real Instance transactions
 */
#include "xir/xxir_source_generation.h"
#include "xir/xxir_instance_generation.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_program_internal.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_nominal.h"
#include "xir/xxir_types.h"
#include "module/xmodule_fingerprint.h"
#include "frontend/parser/xast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "../xir_source_fixture_owner.h"
#include "source_history_cell_fixture.h"
#include "source_history_function_fixture.h"
#if defined(XR_SOURCE_HISTORY_NATIVE)
#include "source_history_native_generated.h"
static bool history_native_case;
static unsigned history_native_kind;
#endif

static XrXirOutputStatus generation_output(void *context,const XrXirOutputGroup *group) {
    unsigned *calls=context;
    CHECK(group && group->count==1 && group->values[0].type==XR_XIR_I64 && group->values[0].payload==17);
    CHECK(!*calls);++*calls;return XR_XIR_OUTPUT_OK;
}
static uint64_t prepare(XrXirSourceGenerations *owner,const char *source) {
    const XrXirSourceText text={NULL,source,strlen(source)};
    uint64_t serial=0;XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_generation_prepare(owner,&text,&serial,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"prepare=%u line=%d %s\n",(unsigned)status,diagnostic.line,diagnostic.message);
    CHECK(status==XR_XIR_OK && serial && diagnostic.status==XR_XIR_OK);return serial;
}
static XrXirProgram *program(const XrXirSourceGenerationView *view,const char *name,uint32_t *entry) {
    XrXirArtifact *closed=NULL,*lowered=NULL;XrXirProgram *output=NULL;
    CHECK(xr_xir_compile_specialize(view->result->checked,&closed,NULL)==XR_XIR_OK);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);bool found=false;
    for (uint32_t i=0;i<module->function_count;++i)
        if (module->declarations->functions[i].module==module->declarations->root_module &&
            module->functions[i].name_length==strlen(name) && !memcmp(module->functions[i].name,name,strlen(name))) {
            CHECK(!found);*entry=i;found=true;
        }
    CHECK(found);
#if defined(XR_SOURCE_HISTORY_NATIVE)
    bool native=history_native_case && !(view->serial==1 ? XR_SOURCE_HISTORY_OLD_VM : XR_SOURCE_HISTORY_NEW_VM);
    if (native) {
        CHECK(view->serial>=1 && view->serial<=4);
        unsigned fixture=history_native_kind>=9 ? history_native_kind-5 :
            history_native_kind>=4 ? history_native_kind+2 : history_native_kind;
        CHECK(fixture<10);
        const XrXirProgramSpec *spec=source_history_native_programs[fixture][view->serial-1];
        CHECK(xr_xir_compile_program_seal(xr_xir_compile_artifact_context(lowered),spec,&output)==XR_XIR_OK);
        xr_xir_compile_artifact_free(lowered);return output;
    }
#endif
    CHECK(xr_xir_compile_vm_program_take(&lowered,&output)==XR_XIR_OK && !lowered);return output;
}
static XrXirCallStatus generation_poll(XrXirInstance *instance) {
    for (unsigned i=0;i<1024;++i) {
        XrXirCallStatus status=xr_xir_instance_poll_bounded(instance,256).outcome.status;
        if (status!=XR_XIR_CALL_READY) return status;
    }
    CHECK(false);return XR_XIR_CALL_LIMIT;
}
static void bind_program(SourceFixtureOwner *compiler,XrXirSourceGenerations *owner,
    uint64_t serial,XrXirProgram *input) {
    XrXirStatus status=xr_xir_compile_source_generation_bind_program(owner,serial,input);
    if (status!=XR_XIR_OK) fprintf(stderr,"Source Program binding serial%llu status%u\n",
        (unsigned long long)serial,(unsigned)status);
    CHECK(status==XR_XIR_OK);
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(compiler->context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_xir_compile_source_generation_bind_program(owner,serial,input)==XR_XIR_OK);
    CHECK(xr_compile_resources_stats(compiler->context.resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(before.allocation_count==after.allocation_count && before.allocated_bytes==after.allocated_bytes &&
        before.live_bytes==after.live_bytes && before.work==after.work);
}
static void old_function(XrXirInstance *instance,const XrXirValue *function) {
    CHECK(xr_xir_instance_start_function(instance,function,NULL,0)==XR_XIR_CALL_READY);
    CHECK(generation_poll(instance)==XR_XIR_CALL_RETURNED);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(result.type==XR_XIR_I64 && result.payload==5);xr_xir_value_drop(&result);
}
static void source_publish(SourceFixtureOwner *compiler,XrXirSourceGenerations *owner,uint64_t serial) {
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(compiler->context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_xir_compile_source_generation_publish(owner,serial)==XR_XIR_OK);
    CHECK(xr_compile_resources_stats(compiler->context.resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(before.work==after.work && before.allocated_bytes==after.allocated_bytes && after.live_bytes<=before.live_bytes);
}
#include "source_history_function_value.h"
static void compile_failure(XrXirSourceGenerations *owner,uint64_t first) {
    const char source[]="const value=missing";
    const XrXirSourceText text={NULL,source,sizeof(source)-1};
    uint64_t serial=0;XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_generation_prepare(owner,&text,&serial,&diagnostic);
    CHECK(status==XR_XIR_BAD_VALUE);
    CHECK(!serial && diagnostic.status==XR_XIR_BAD_VALUE && diagnostic.line==1 &&
        strstr(diagnostic.message,"name is not an initialized value"));
    XrXirSourceGenerationBinding binding={0};
    CHECK(xr_xir_compile_source_generation_lookup(owner,"value",&binding)==XR_XIR_OK);
    CHECK(binding.generation==first && binding.declaration->type.type==XR_XIR_I64);
}
static void aborted_string(XrXirSourceGenerations *owner,XrXirInstance *instance,uint64_t first) {
    uint64_t serial=prepare(owner,"const value=\"new\"\nexport fn answer()->string{return value}");
    XrXirSourceGenerationView view={0};CHECK(xr_xir_compile_source_generation_view(owner,serial,&view)==XR_XIR_OK);
    CHECK(!view.published && view.parent==first);
    XrXirSourceGenerationBinding binding={0};
    CHECK(xr_xir_compile_source_generation_lookup(owner,"value",&binding)==XR_XIR_OK && binding.generation==first);
    uint32_t entry=0;XrXirProgram *candidate=program(&view,"answer",&entry);uint64_t runtime=0;
    CHECK(xr_xir_compile_source_generation_bind_program(owner,serial,candidate)==XR_XIR_OK);
    CHECK(xr_xir_source_generation_prepare_instance(owner,serial,instance,&runtime)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY && generation_poll(instance)==XR_XIR_CALL_RETURNED);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&result,&bytes,&length) && length==3 && !memcmp(bytes,"new",3));
    CHECK(xr_xir_instance_generation_abort(instance,runtime)==XR_XIR_CALL_READY);
    CHECK(xr_xir_compile_source_generation_abort(owner,serial)==XR_XIR_OK);
    CHECK(xr_xir_compile_source_generation_view(owner,serial,&view)==XR_XIR_BAD_STAGE);
    CHECK(xr_xir_compile_source_generation_lookup(owner,"value",&binding)==XR_XIR_OK && binding.generation==first);
    xr_xir_compile_program_drop(candidate);xr_xir_value_drop(&result);
}
static void failed_runtime(XrXirSourceGenerations *owner,XrXirInstance *instance,uint64_t first,XrXirCallResult *failure) {
    uint64_t serial=prepare(owner,"const value=9\nassert(false,\"new-fault\")\nexport fn answer()->i64{return value}");
    XrXirSourceGenerationView view={0};CHECK(xr_xir_compile_source_generation_view(owner,serial,&view)==XR_XIR_OK);
    uint32_t entry=0;XrXirProgram *candidate=program(&view,"answer",&entry);uint64_t runtime=0;
    CHECK(xr_xir_compile_source_generation_bind_program(owner,serial,candidate)==XR_XIR_OK);
    CHECK(xr_xir_source_generation_prepare_instance(owner,serial,instance,&runtime)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY && generation_poll(instance)==XR_XIR_CALL_ASSERTION);
    CHECK(xr_xir_instance_generation_publish(instance,runtime)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_copy_failure(instance,failure)==XR_XIR_CALL_ASSERTION);
    CHECK(xr_xir_instance_generation_abort(instance,runtime)==XR_XIR_CALL_READY);
    CHECK(xr_xir_compile_source_generation_abort(owner,serial)==XR_XIR_OK);
    XrXirSourceGenerationBinding binding={0};
    CHECK(xr_xir_compile_source_generation_lookup(owner,"value",&binding)==XR_XIR_OK && binding.generation==first);
    xr_xir_compile_program_drop(candidate);
}
static void definition_checks(XrXirSourceGenerations *owner,uint64_t serial,
    const XrXirSourceGenerationView *view,bool has_value,bool mutable_value,unsigned value_kind) {
    const XrXirModule *module=xr_xir_compile_artifact_module(view->result->checked);
    XrXirSourceGenerationDefinition definition={0},sentinel;
    bool imported=view->ast->as.program.statements[0]->type==AST_IMPORT_STMT;
    uint32_t shift=has_value || imported ? 1 : 0;
    CHECK(xr_xir_compile_source_generation_definition(owner,serial,"old",&definition)==XR_XIR_OK);
    CHECK(definition.checked==view->result->checked && definition.generation==serial &&
        definition.kind==XR_XIR_SOURCE_DEFINITION_FUNCTION && definition.ordinal<module->function_count &&
        definition.syntax==view->ast->as.program.statements[shift] && !strcmp(definition.name,"old"));
    const XrXirFunction *function=&module->functions[definition.ordinal];
    CHECK(function->name_length==3 && !memcmp(function->name,"old",3) && function->result==XR_XIR_I64);
    CHECK(xr_xir_compile_source_generation_definition(owner,serial,"identity",&definition)==XR_XIR_OK);
    CHECK(definition.kind==XR_XIR_SOURCE_DEFINITION_FUNCTION && definition.ordinal<module->function_count &&
        definition.syntax==view->ast->as.program.statements[shift+1] && module->generics &&
        module->generics[definition.ordinal].parameter_count==1);
    CHECK(xr_xir_compile_source_generation_definition(owner,serial,"Item",&definition)==XR_XIR_OK);
    CHECK(definition.kind==XR_XIR_SOURCE_DEFINITION_NOMINAL && module->types && module->types->nominals &&
        definition.ordinal<module->types->nominals->count && definition.syntax==view->ast->as.program.statements[shift+2]);
    const XrXirNominalDeclaration *item=&module->types->nominals->declarations[definition.ordinal];
    CHECK(item->field_count==1 && (value_kind==5 ? xr_xir_type_is_array(module->types,item->fields[0].type) &&
        xr_xir_array_element(module->types,item->fields[0].type)==XR_XIR_I64 :
        value_kind==7 ? xr_xir_type_is_struct(module->types,item->fields[0].type) :
        item->fields[0].type==(value_kind==6 ? XR_XIR_STRING : XR_XIR_I64)));
    if (has_value) {
        CHECK(xr_xir_compile_source_generation_definition(owner,serial,"value",&definition)==XR_XIR_OK);
        XrXirType element=mutable_value ? xr_xir_cell_element(module->types,definition.stored_type) : definition.stored_type;
        CHECK(value_kind==4 || value_kind==5 || value_kind==6 || value_kind==7 ? xr_xir_type_is_class(module->types,element) :
            value_kind==2 || value_kind==3 ? xr_xir_type_is_array(module->types,element) &&
            xr_xir_array_element(module->types,element)==XR_XIR_I64 : element==XR_XIR_I64);
        CHECK(definition.kind==XR_XIR_SOURCE_DEFINITION_BINDING && definition.mutable==mutable_value &&
            definition.ordinal<module->declarations->slot_count &&
            module->declarations->slots[definition.ordinal].type==definition.stored_type &&
            definition.syntax==view->ast->as.program.statements[0]);
    }
    if (imported) {
        CHECK(xr_xir_compile_source_generation_definition(owner,serial,"library",&definition)==XR_XIR_OK);
        CHECK(definition.kind==XR_XIR_SOURCE_DEFINITION_MODULE && definition.ordinal==UINT32_MAX &&
            definition.dependency<module->declarations->module_count && definition.syntax==view->ast->as.program.statements[0] &&
            definition.dependency_syntax && definition.dependency_syntax->type==AST_PROGRAM &&
            definition.dependency_syntax->as.program.count==1 &&
            !strcmp(definition.dependency_syntax->as.program.statements[0]->as.function_decl.name,"answer"));
        const XrXirSourceModule *scope=&module->declarations->modules[definition.module];bool linked=false;
        for (uint32_t i=0;i<scope->dependency_count;++i) linked|=scope->dependencies[i]==definition.dependency;
        CHECK(linked);
    }
    memset(&sentinel,0xa5,sizeof(sentinel));definition=sentinel;
    CHECK(xr_xir_compile_source_generation_definition(owner,serial,"absent",&definition)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&definition,&sentinel,sizeof(definition)));
    CHECK(xr_xir_compile_source_generation_definition(owner,serial+100,"old",&definition)==XR_XIR_BAD_STAGE &&
        !memcmp(&definition,&sentinel,sizeof(definition)));
}
static void history_ref_string(const XrXirValue *value,unsigned calls) {
    CHECK(calls==1 || calls==2 || calls==4);
    static const char *const expected[]={"ax1","ax1x1","ax1x1x1x1"};
    const char *text=expected[calls==1 ? 0 : calls==2 ? 1 : 2],*bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(value,&bytes,&length) && length==strlen(text) && !memcmp(bytes,text,length));
}

static void history_answer(XrXirValue *value,bool string,unsigned calls,int64_t expected) {
    if (string) history_ref_string(value,calls);
    else CHECK(value->type==XR_XIR_I64 && value->payload==expected);
    xr_xir_value_drop(value);
}
static void class_ref_coordinates(XrXirSourceGenerations *owner,uint64_t first,
    const XrXirSourceGenerationView *current,bool inline_leaf) {
    XrXirSourceGenerationView birth={0};
    CHECK(xr_xir_compile_source_generation_view(owner,first,&birth)==XR_XIR_OK);
    const XrXirModule *modules[]={xr_xir_compile_artifact_module(birth.result->checked),
        xr_xir_compile_artifact_module(current->result->checked)};
    XrXirType classes[]={XR_XIR_UNIT,XR_XIR_UNIT};bool string_cells[]={false,false};
    for (unsigned m=0;m<2;++m) {
        const XrXirTypes *types=modules[m]->types;CHECK(types);
        for (uint32_t s=0;s<modules[m]->declarations->slot_count;++s) {
            const XrXirSlot *slot=&modules[m]->declarations->slots[s];
            XrXirType element=xr_xir_cell_element(types,slot->type);
            if (slot->mutable && xr_xir_type_is_class(types,element)) classes[m]=element;
        }
        const XrXirTypeNode *object=xr_xir_type_node(types,classes[m]);
        CHECK(object && object->nominal.field_count==1);
        XrXirType leaf=inline_leaf ? object->nominal.fields[0] : XR_XIR_STRING;
        if (inline_leaf) {
            const XrXirTypeNode *record=xr_xir_type_node(types,leaf);
            CHECK(xr_xir_type_is_struct(types,leaf) && record && record->nominal.field_count==1 &&
                record->nominal.fields[0]==XR_XIR_I64);
        }
        for (uint32_t t=0;t<types->count;++t)
            string_cells[m]|=types->nodes[t].kind==XR_XIR_TYPE_CELL && types->nodes[t].element==leaf;
    }
    printf(inline_leaf ? "CLASS_INLINE_REF_COORDINATES birth=%u current=%u birth_leaf_cell=%u current_leaf_cell=%u\n" :
        "CLASS_REF_COORDINATES birth=%u current=%u birth_string_cell=%u current_string_cell=%u\n",
        (unsigned)classes[0],(unsigned)classes[1],(unsigned)string_cells[0],(unsigned)string_cells[1]);
    CHECK(classes[0]!=XR_XIR_UNIT && classes[1]!=XR_XIR_UNIT && classes[0]!=classes[1] && !string_cells[0] && string_cells[1]);
}
static int history_check(const char *mode) {
    bool definition_cell=!strcmp(mode,"--definitions-cell");
    bool class_ref=!strcmp(mode,"--history-class-ref");
    bool class_inline_ref=!strcmp(mode,"--history-class-inline-ref");
    bool class_array=!strcmp(mode,"--history-class-array");
    bool class_value=class_inline_ref || class_ref || class_array || !strcmp(mode,"--history-class");
    bool array_const=!strcmp(mode,"--history-array-const");
    bool array_value=array_const || !strcmp(mode,"--history-array");
    bool mutating=class_value || (array_value && !array_const) || !strcmp(mode,"--history-cell-write");
    unsigned storage_kind=class_inline_ref ? 7u : class_ref ? 6u : class_array ? 5u : class_value ? 4u : array_const ? 3u : array_value ? 2u : mutating ? 1u : 0u;
    bool definitions=definition_cell || !strcmp(mode,"--definitions") || !strcmp(mode,"--definitions-comptime") || !strcmp(mode,"--definitions-import");
    bool folded=!strcmp(mode,"--definitions-comptime");
    bool imported=!strcmp(mode,"--definitions-import");
    bool cell=definition_cell || mutating || !strcmp(mode,"--history-cell");
    bool active_birth=!strcmp(mode,"--history-active-birth");
#if defined(XR_SOURCE_HISTORY_NATIVE)
    history_native_case=(cell || array_value) && !definition_cell;history_native_kind=storage_kind;
#endif
    bool stateful=!strcmp(mode,"--history-stateful") || cell || array_value || active_birth;
    const char *next=class_ref || class_inline_ref ? source_history_cell_input(2,storage_kind) : definition_cell ? NULL : !strcmp(mode,"--history-function") ?
        "export fn answer()->i64{return old()+1}" : !strcmp(mode,"--history-generic") ?
        "export fn answer()->i64{return identity<i64>(42)}" : !strcmp(mode,"--history-type") ?
        "export fn answer()->i64{const item=Item{value:42};return item.value}" : stateful ?
        "const value=\"shadow\"\nexport fn answer()->i64{return old()}" : NULL;
    CHECK(next || definitions);
    SourceFixtureOwner compiler={0};source_fixture_owner_new(&compiler);
    XrXirSourceGenerationConfig config={compiler.context,"history-check-test",imported ? XR_GENERATION_STDLIB : NULL,NULL,NULL};
    XrXirSourceGenerations *owner=NULL;CHECK(xr_xir_compile_source_generations_new(&config,&owner)==XR_XIR_OK);
    const char *source=active_birth ? "const value=41\nfn old()->i64{return value+1}\nfn identity<T>(value:T)->T{return value}\nstruct Item{value:i64}\nprint(17)\nexport fn seed()->fn()->i64{return old}" :
        cell || array_value ? source_history_cell_input(1,storage_kind) :
        stateful ? "const value=41\nfn old()->i64{return value+1}\nfn identity<T>(value:T)->T{return value}\nstruct Item{value:i64}\nprint(17)\nexport fn seed()->i64{return 0}" :
        imported ? "import _probe as library\nfn old()->i64{return 41}\nfn identity<T>(value:T)->T{return value}\nstruct Item{value:i64}\nexport fn seed()->i64{return library.answer()}" :
        folded ? "const value=comptime (6*7)\nfn old()->i64{return 41}\nfn identity<T>(value:T)->T{return value}\nstruct Item{value:i64}\nexport fn seed()->i64{return value}" :
        "fn old()->i64{return 41}\nfn identity<T>(value:T)->T{return value}\nstruct Item{value:i64}\nexport fn seed()->i64{return 0}";
    uint64_t first=prepare(owner,source);
    XrXirSourceGenerationView view={0};CHECK(xr_xir_compile_source_generation_view(owner,first,&view)==XR_XIR_OK);
    const struct AstNode *original=view.ast;
    definition_checks(owner,first,&view,folded || stateful,cell,storage_kind);
    uint32_t entry=0;XrXirProgram *initial=program(&view,"seed",&entry);
    bind_program(&compiler,owner,first,initial);
    XrXirInstanceConfig runtime_config;CHECK(xr_xir_instance_config_init(&runtime_config,sizeof(runtime_config))==XR_XIR_CALL_READY);
    unsigned output_calls=0;
    if (stateful) runtime_config.output=(XrXirOutputProvider){.abi_version=XR_XIR_CALL_ABI_VERSION,
        .write=generation_output,.context=&output_calls};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_generation_new(&runtime_config,&instance)==XR_XIR_CALL_READY);
    uint64_t runtime=0;CHECK(xr_xir_source_generation_prepare_instance(owner,first,instance,&runtime)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY && generation_poll(instance)==XR_XIR_CALL_RETURNED);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    XrXirValue birth_function={0};
    if (active_birth) {
        CHECK(xr_xir_value_valid(&result) && xr_xir_value_arena(&result));
        birth_function=result;result=(XrXirValue){0};
    } else CHECK(result.type==XR_XIR_I64 && result.payload==(folded || imported ? 42u : 0u));
    xr_xir_value_drop(&result);
    CHECK(xr_xir_instance_generation_publish(instance,runtime)==XR_XIR_CALL_READY);source_publish(&compiler,owner,first);
    XrXirStatus status=XR_XIR_OK;uint64_t serial=0;
    if (next) {
        XrXirSourceDiagnostic diagnostic={0};const XrXirSourceText text={NULL,next,strlen(next)};
        status=xr_xir_compile_source_generation_prepare(owner,&text,&serial,&diagnostic);
        printf("history mode=%s source=%u line=%d serial=%llu message=%s\n",mode,(unsigned)status,
            diagnostic.line,(unsigned long long)serial,diagnostic.message);
    }
    XrXirSourceGenerationBinding binding={0};
    CHECK(xr_xir_compile_source_generation_lookup(owner,"old",&binding)==XR_XIR_OK && binding.generation==first);
    CHECK(xr_xir_compile_source_generation_view(owner,first,&view)==XR_XIR_OK && view.published && view.ast==original);
    definition_checks(owner,first,&view,folded || stateful,cell,storage_kind);
    if (next && status==XR_XIR_OK) {
        CHECK(serial>first && xr_xir_compile_source_generation_view(owner,serial,&view)==XR_XIR_OK && view.parent==first);
        if (class_ref || class_inline_ref) class_ref_coordinates(owner,first,&view,class_inline_ref);
        const XrXirDeclarations *declarations=xr_xir_compile_artifact_module(view.result->checked)->declarations;
        uint32_t root=declarations->root_module,scope=declarations->modules[root].lexical_scope;
        CHECK(scope && scope-1!=root && xr_xir_module_same_scope(declarations,root,scope-1));
        uint32_t next_entry=0;XrXirProgram *received=program(&view,"answer",&next_entry);uint64_t candidate=0;
        CHECK(xr_xir_compile_source_generation_bind_program(owner,serial,initial)==XR_XIR_BAD_STRUCTURE);
        if (cell || array_value) {
            XrXirInstance *in_use=NULL;
            CHECK(xr_xir_instance_new(received,&runtime_config,&in_use)==XR_XIR_CALL_BAD_STATE && !in_use);
            CHECK(xr_xir_compile_program_retain(received));
            CHECK(xr_xir_compile_source_generation_bind_program(owner,serial,received)==XR_XIR_BAD_STAGE);
            xr_xir_compile_program_drop(received);
        }
        bind_program(&compiler,owner,serial,received);
        CHECK(xr_xir_compile_source_generation_bind_program(owner,serial,initial)==XR_XIR_BAD_STAGE);
        XrXirInstance *foreign=NULL;
        CHECK(xr_xir_instance_new(initial,&runtime_config,&foreign)==XR_XIR_CALL_READY);
        CHECK(xr_xir_source_generation_prepare_instance(owner,serial,foreign,&candidate)==XR_XIR_CALL_BAD_ARGUMENT && !candidate);
        CHECK(xr_xir_instance_free(foreign)==XR_XIR_CALL_READY);
        CHECK(xr_xir_source_generation_prepare_instance(owner,serial,instance,&candidate)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,next_entry,NULL,0)==XR_XIR_CALL_READY);
        XrXirCallStatus outcome=generation_poll(instance);
        if (outcome!=XR_XIR_CALL_RETURNED) fprintf(stderr,"history runtime mode=%s outcome=%u output_calls=%u\n",
            mode,(unsigned)outcome,output_calls);
        CHECK(outcome==XR_XIR_CALL_RETURNED);
        XrXirValue answer={0};CHECK(xr_xir_instance_take_result(instance,&answer)==XR_XIR_CALL_RETURNED);
        history_answer(&answer,class_ref,1,42);
        if (stateful) {
            CHECK(xr_xir_instance_generation_publish(instance,candidate)==XR_XIR_CALL_READY);
            source_publish(&compiler,owner,serial);
            if (active_birth) {
                CHECK(xr_xir_instance_start_function(instance,&birth_function,NULL,0)==XR_XIR_CALL_READY);
            }
            uint64_t third=prepare(owner,source_history_cell_input(3,storage_kind));
            CHECK(xr_xir_compile_source_generation_view(owner,third,&view)==XR_XIR_OK && view.parent==serial);
            uint32_t third_entry=0;XrXirProgram *latest=program(&view,"answer",&third_entry);
            bind_program(&compiler,owner,third,latest);
            candidate=0;
            if (active_birth) {
                CHECK(xr_xir_source_generation_prepare_instance(owner,third,instance,&candidate)==XR_XIR_CALL_BUSY && !candidate);
                CHECK(generation_poll(instance)==XR_XIR_CALL_RETURNED);
                CHECK(xr_xir_instance_take_result(instance,&answer)==XR_XIR_CALL_RETURNED);
                CHECK(answer.type==XR_XIR_I64 && answer.payload==42);xr_xir_value_drop(&answer);
            }
            CHECK(xr_xir_source_generation_prepare_instance(owner,third,instance,&candidate)==XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_start(instance,third_entry,NULL,0)==XR_XIR_CALL_READY && generation_poll(instance)==XR_XIR_CALL_RETURNED);
            CHECK(xr_xir_instance_take_result(instance,&answer)==XR_XIR_CALL_RETURNED);
            history_answer(&answer,class_ref,2,mutating ? 43 : 42);
            CHECK(xr_xir_instance_generation_publish(instance,candidate)==XR_XIR_CALL_READY);
            source_publish(&compiler,owner,third);
            uint64_t failed=prepare(owner,source_history_cell_input(4,storage_kind));
            CHECK(xr_xir_compile_source_generation_view(owner,failed,&view)==XR_XIR_OK);
            uint32_t failed_entry=0;XrXirProgram *broken=program(&view,"fail",&failed_entry);
            bind_program(&compiler,owner,failed,broken);candidate=0;
            CHECK(xr_xir_source_generation_prepare_instance(owner,failed,instance,&candidate)==XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_start(instance,failed_entry,NULL,0)==XR_XIR_CALL_READY && generation_poll(instance)==XR_XIR_CALL_ASSERTION);
            CHECK(xr_xir_instance_generation_publish(instance,candidate)==XR_XIR_CALL_BAD_STATE);
            CHECK(xr_xir_instance_generation_abort(instance,candidate)==XR_XIR_CALL_READY);
            CHECK(xr_xir_compile_source_generation_abort(owner,failed)==XR_XIR_OK);
            CHECK(xr_xir_instance_start(instance,third_entry,NULL,0)==XR_XIR_CALL_READY && generation_poll(instance)==XR_XIR_CALL_RETURNED);
            CHECK(xr_xir_instance_take_result(instance,&answer)==XR_XIR_CALL_RETURNED);
            history_answer(&answer,class_ref,4,mutating ? 45 : 42);
            xr_xir_compile_program_drop(broken);xr_xir_compile_program_drop(latest);
        } else {
            CHECK(xr_xir_instance_generation_abort(instance,candidate)==XR_XIR_CALL_READY);
            CHECK(xr_xir_compile_source_generation_abort(owner,serial)==XR_XIR_OK);
        }
        xr_xir_compile_program_drop(received);
    } else CHECK(!serial);
    xr_xir_value_drop(&birth_function);
    xr_xir_compile_source_generations_free(owner);xr_xir_compile_program_drop(initial);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);source_fixture_owner_free(&compiler);
    CHECK(status==XR_XIR_OK);
    if (stateful) CHECK(output_calls==1);
    puts(definitions ? "Owned actual semantic ordinals, generic/type/slot owners and original AST survived Source construction; no history receiving qualification" : stateful ?
        "Three Source/Instance generations returned42 from birth storage; original initialization ran once and foreign Domain was rejected" :
        "Published historical declaration entered ordinary checking and VM returned42; paired candidate aborted");
    return 0;
}
static void mutable_definitions(void) {
    SourceFixtureOwner compiler={0};source_fixture_owner_new(&compiler);
    XrXirSourceGenerationConfig config={compiler.context,"mutable-definition-test",NULL,NULL,NULL};
    XrXirSourceGenerations *owner=NULL;CHECK(xr_xir_compile_source_generations_new(&config,&owner)==XR_XIR_OK);
    uint64_t serial=prepare(owner,"var value:i64=11\nvar empty=()\nconst unit=()\n");
    XrXirSourceGenerationView view={0};CHECK(xr_xir_compile_source_generation_view(owner,serial,&view)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(view.result->checked);
    const char *names[]={"value","empty"};const XrXirType types[]={XR_XIR_I64,XR_XIR_UNIT};
    for(unsigned i=0;i<2;++i) {
        XrXirSourceGenerationDefinition definition={0};
        CHECK(xr_xir_compile_source_generation_definition(owner,serial,names[i],&definition)==XR_XIR_OK);
        CHECK(definition.kind==XR_XIR_SOURCE_DEFINITION_BINDING && definition.mutable &&
            definition.logical_type==types[i] && definition.ordinal<module->declarations->slot_count &&
            definition.stored_type==module->declarations->slots[definition.ordinal].type &&
            xr_xir_type_is_cell(module->types,definition.stored_type) &&
            xr_xir_cell_element(module->types,definition.stored_type)==types[i]);
    }
    XrXirSourceGenerationDefinition unit={0};
    CHECK(xr_xir_compile_source_generation_definition(owner,serial,"unit",&unit)==XR_XIR_OK &&
        unit.kind==XR_XIR_SOURCE_DEFINITION_UNIT && !unit.mutable && unit.ordinal==UINT32_MAX);
    CHECK(xr_xir_compile_source_generation_abort(owner,serial)==XR_XIR_OK);
    xr_xir_compile_source_generations_free(owner);source_fixture_owner_free(&compiler);
}
static void inspection_definitions(void) {
    SourceFixtureOwner compiler={0};source_fixture_owner_new(&compiler);
    XrXirSourceGenerationConfig config={compiler.context,"checked-reader-test",NULL,NULL,NULL};
    XrXirSourceGenerations *owner=NULL;CHECK(xr_xir_compile_source_generations_new(&config,&owner)==XR_XIR_OK);
    const char source[]="var root:i64=42\nvar empty=()\nconst unit=()\nconst text=\"owned\"\n"
        "const values=[11,42]\nconst callback=fn()->i64{return root}\n";
    const char *names[]={"root","empty","unit","text","values","callback"};
    uint64_t ordinary=prepare(owner,source);
    for(unsigned i=0;i<sizeof(names)/sizeof(*names);++i) {
        XrXirSourceGenerationDefinition definition={0};
        CHECK(xr_xir_compile_source_generation_definition(owner,ordinary,names[i],&definition)==XR_XIR_OK && definition.reader==UINT32_MAX);
    }
    CHECK(xr_xir_compile_source_generation_abort(owner,ordinary)==XR_XIR_OK);
    uint64_t serial=0;XrXirSourceText text={NULL,source,sizeof(source)-1};
    CHECK(xr_xir_compile_source_generation_prepare_bindings(owner,&text,&serial,NULL)==XR_XIR_OK && serial);
    XrXirSourceGenerationView view={0};CHECK(xr_xir_compile_source_generation_view(owner,serial,&view)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(view.result->checked);
    for(unsigned i=0;i<sizeof(names)/sizeof(*names);++i) {
        XrXirSourceGenerationDefinition definition={0};
        XrXirStatus status=xr_xir_compile_source_generation_definition(owner,serial,names[i],&definition);
        if(status!=XR_XIR_OK)fprintf(stderr,"actual reader definition %s status=%u\n",names[i],status);
        CHECK(status==XR_XIR_OK && definition.reader<module->function_count);
        const XrXirFunction *reader=&module->functions[definition.reader];
        CHECK(!reader->parameter_count && reader->result==definition.logical_type &&
            module->declarations->functions[definition.reader].module==definition.module &&
            module->declarations->functions[definition.reader].exported &&
            (module->declarations->functions[definition.reader].promises&XR_XIR_FUNCTION_NO_SUSPEND));
        CHECK(reader->instructions[reader->instruction_count-1].op==XR_XIR_RETURN);
    }
    const XrXirSourceGenerationBinding *bindings=NULL;size_t count=0;
    CHECK(xr_xir_compile_source_generation_candidate_bindings(owner,serial,&bindings,&count)==XR_XIR_OK && count==6);
    for(size_t i=0;i<count;++i) {
        XrXirSourceGenerationDefinition definition={0};
        CHECK(xr_xir_compile_source_generation_definition(owner,serial,bindings[i].declaration->name,&definition)==XR_XIR_OK);
    }
    CHECK(xr_xir_compile_source_generation_abort(owner,serial)==XR_XIR_OK);
    xr_xir_compile_source_generations_free(owner);source_fixture_owner_free(&compiler);
}
int main(int argc,char **argv) {
    if (argc==2 && (!strcmp(argv[1],"--history-function-call") || !strcmp(argv[1],"--history-function-value"))) {
        bool escaped=!strcmp(argv[1],"--history-function-value");
#if defined(XR_SOURCE_HISTORY_NATIVE)
        history_native_case=true;history_native_kind=escaped?10u:9u;
#endif
        return escaped?history_function_value():history_function_call();
    }
    if (argc==2) return history_check(argv[1]);
    CHECK(argc==1);
    mutable_definitions();
    inspection_definitions();
    SourceFixtureOwner compiler={0};source_fixture_owner_new(&compiler);
    char namespace_id[]="source-generation-test";
    XrXirSourceGenerationConfig config={compiler.context,namespace_id,NULL,NULL,NULL};
    XrXirSourceGenerations *owner=NULL;CHECK(xr_xir_compile_source_generations_new(&config,&owner)==XR_XIR_OK);
    memset(namespace_id,'!',sizeof(namespace_id)-1);
    char source[]="const value=5\nprint(17)\nfn read()->i64{const local=1;return value}\nexport fn make()->fn()->i64{return read}";
    uint64_t first=prepare(owner,source);memset(source,'?',sizeof(source)-1);
    XrXirSourceGenerationView view={0};CHECK(xr_xir_compile_source_generation_view(owner,first,&view)==XR_XIR_OK);
    CHECK(!view.published && !view.parent && view.ast && view.ast->type==AST_PROGRAM);
    CHECK(view.ast->as.program.count==4 && !strcmp(view.ast->as.program.statements[0]->as.var_decl.name,"value") &&
        !strcmp(view.ast->as.program.statements[2]->as.function_decl.name,"read"));
    CHECK(strstr(view.source,"const value=5") && strstr(view.identity,"gen-22-source-generation-test-1"));
    XrFingerprint fingerprint={0};xr_module_source_fingerprint(view.source,&fingerprint);
    CHECK(!memcmp(&fingerprint,&view.fingerprint,sizeof(fingerprint)));
    XrXirSourceGenerationBinding sentinel={99,NULL},binding=sentinel;
    CHECK(xr_xir_compile_source_generation_lookup(owner,"value",&binding)==XR_XIR_BAD_STRUCTURE && !memcmp(&binding,&sentinel,sizeof(binding)));
    uint64_t occupied=0;const XrXirSourceText empty={NULL,"",0};
    CHECK(xr_xir_compile_source_generation_prepare(owner,&empty,&occupied,NULL)==XR_XIR_BAD_STAGE && !occupied);
    CHECK(xr_xir_compile_source_generation_publish(owner,first+1)==XR_XIR_BAD_STAGE);
    uint32_t entry=0;XrXirProgram *initial=program(&view,"make",&entry);
    XrXirInstanceConfig runtime_config;CHECK(xr_xir_instance_config_init(&runtime_config,sizeof(runtime_config))==XR_XIR_CALL_READY);
    unsigned output_calls=0;runtime_config.output=(XrXirOutputProvider){.abi_version=XR_XIR_CALL_ABI_VERSION,.write=generation_output,.context=&output_calls};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_generation_new(&runtime_config,&instance)==XR_XIR_CALL_READY);
    bind_program(&compiler,owner,first,initial);
    uint64_t runtime=0;CHECK(xr_xir_source_generation_prepare_instance(owner,first,instance,&runtime)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY && generation_poll(instance)==XR_XIR_CALL_RETURNED);
    XrXirValue function={0};CHECK(xr_xir_instance_take_result(instance,&function)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_generation_publish(instance,runtime)==XR_XIR_CALL_READY);source_publish(&compiler,owner,first);
    CHECK(xr_xir_compile_source_generation_lookup(owner,"value",&binding)==XR_XIR_OK && binding.generation==first && binding.declaration->type.type==XR_XIR_I64);
    binding=sentinel;CHECK(xr_xir_compile_source_generation_lookup(owner,"local",&binding)==XR_XIR_BAD_STRUCTURE && !memcmp(&binding,&sentinel,sizeof(binding)));
    old_function(instance,&function);aborted_string(owner,instance,first);old_function(instance,&function);
    compile_failure(owner,first);old_function(instance,&function);
    XrXirCallResult failure={0};failed_runtime(owner,instance,first,&failure);old_function(instance,&function);
    uint64_t next=prepare(owner,"const value=\"published\"\nexport fn answer()->string{return value}");
    CHECK(next>first+2 && xr_xir_compile_source_generation_view(owner,next,&view)==XR_XIR_OK);
    uint32_t next_entry=0;XrXirProgram *newer=program(&view,"answer",&next_entry);uint64_t next_runtime=0;
    bind_program(&compiler,owner,next,newer);
    CHECK(xr_xir_source_generation_prepare_instance(owner,next,instance,&next_runtime)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,next_entry,NULL,0)==XR_XIR_CALL_READY && generation_poll(instance)==XR_XIR_CALL_RETURNED);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_generation_publish(instance,next_runtime)==XR_XIR_CALL_READY);source_publish(&compiler,owner,next);
    CHECK(xr_xir_compile_source_generation_lookup(owner,"value",&binding)==XR_XIR_OK && binding.generation==next && binding.declaration->type.type==XR_XIR_STRING);
    CHECK(xr_xir_compile_source_generation_view(owner,first,&view)==XR_XIR_OK && view.published && strstr(view.source,"const value=5"));
    xr_xir_compile_source_generations_free(owner);xr_xir_compile_program_drop(initial);xr_xir_compile_program_drop(newer);
    old_function(instance,&function);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&result,&bytes,&length) && length==9 && !memcmp(bytes,"published",9));
    CHECK(xr_xir_string_view(&failure.panic.message,&bytes,&length) && length==9 && !memcmp(bytes,"new-fault",9));
    xr_xir_value_drop(&result);xr_xir_call_result_drop(&failure);xr_xir_value_drop(&function);
    CHECK(output_calls==1);source_fixture_owner_free(&compiler);
    puts("Owned Source generations: exact bytes/AST/identity, staged names, compile/runtime abort, no-allocation publish and old function5 after producer death passed");
    return 0;
}
