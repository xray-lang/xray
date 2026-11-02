/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_class_move_shape.c - Common rejects copying a take into an ancestor
 *
 * KEY CONCEPT:
 *   Real class declarations and constructor facts survive the Source session.
 *   Raw body mutations cannot turn an explicit take into an ancestor copy.
 */
#include "base/xmalloc.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_construction.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_source.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr,"class move shape %d: %s\n",__LINE__,#c);exit(1); \
} } while (0)

static const char class_move_source[] =
    "export class Base {\n"
    " value:i64\n"
    " init(value:i64){this.value=value}\n"
    "}\n"
    "export class Dog extends Base {\n"
    " extra:i64\n"
    " init(value:i64,extra:i64){super(value);this.extra=extra}\n"
    "}\n"
    "export fn valueProbe(value:move Dog)->Dog{return move value}\n"
    "export fn cellProbe(value:Dog)->Dog{var owned=value;return move owned}\n"
    "export fn ancestorProbe(value:Dog)->Base{return value}\n"
    "fn main()->i64{return 42}\n";

typedef enum ClassMoveKind {
    CLASS_ORDINARY_COPY, CLASS_VALUE_TAKE_COPY, CLASS_CELL_TAKE_COPY
} ClassMoveKind;

typedef struct ClassMoveFixture {
    const XrXirCompileContext *context;
    XrXirArtifact *source;
    XrXirType base, dog, cell;
    uint32_t probes[3];
} ClassMoveFixture;

typedef struct ClassMoveCase {
    XrXirModule module;
    XrXirFunction *functions;
    XrXirInstruction instructions[4];
    XrXirBlock block;
    uint32_t function, copy;
} ClassMoveCase;

static uint64_t class_move_held(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);
    return stats.held_bytes;
}

static void class_move_status(const char *operation,XrXirStatus status,
    const XrXirDiagnostic *diagnostic) {
    fprintf(stderr,"class move %s status%u f%u b%u i%u\n",operation,status,
        diagnostic->function,diagnostic->block,diagnostic->instruction);
}

static bool class_move_name(XrXirLiteral actual,const char *expected) {
    size_t length=strlen(expected);
    return actual.length==length && !memcmp(actual.bytes,expected,length);
}

static XrXirType class_move_class(const XrXirModule *module,const char *name) {
    CHECK(module && module->types && module->types->nominals);
    const XrXirNominalTable *nominals=module->types->nominals;
    XrXirType found=XR_XIR_UNIT;
    for(uint32_t t=0;t<module->types->count;++t) {
        const XrXirTypeNode *node=module->types->nodes+t;
        if(node->kind!=XR_XIR_TYPE_NOMINAL)continue;
        CHECK(node->nominal.declaration<nominals->count);
        uint32_t declaration=node->nominal.declaration;
        XrXirLiteral actual;uint32_t kind,arity,exported;
        if(nominals->declarations) {
            const XrXirNominalDeclaration *item=nominals->declarations+declaration;
            actual=item->name;kind=item->kind;arity=item->parameter_count;exported=item->exported;
        } else {
            CHECK(nominals->identities);
            const XrXirNominalIdentity *item=nominals->identities+declaration;
            actual=item->name;kind=item->kind;arity=item->arity;exported=item->exported;
        }
        if(!class_move_name(actual,name))continue;
        CHECK(found==XR_XIR_UNIT && kind==XR_XIR_NOMINAL_CLASS && !arity && exported);
        CHECK(!node->nominal.argument_count && !node->parameter_span);
        found=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+t);
    }
    CHECK(found!=XR_XIR_UNIT);return found;
}

static uint32_t class_move_function(const XrXirModule *module,const char *name) {
    CHECK(module && module->declarations && module->declarations->functions);
    uint32_t found=UINT32_MAX;size_t length=strlen(name);
    for(uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *function=module->functions+f;
        if(function->name_length!=length || memcmp(function->name,name,length))continue;
        const XrXirFunctionIdentity *identity=module->declarations->functions+f;
        CHECK(found==UINT32_MAX && identity->exported && !identity->nominal_owner &&
            !identity->cleanup_owner && identity->method_kind==XR_XIR_NON_MEMBER);
        found=f;
    }
    CHECK(found!=UINT32_MAX);return found;
}

static void class_move_source_owner(ClassMoveFixture *fixture,const char *root) {
    char stdlib_path[1024];int length=snprintf(stdlib_path,sizeof(stdlib_path),"%s/stdlib",root);
    CHECK(length>0 && (size_t)length<sizeof(stdlib_path));
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(fixture->context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"class-move-shape",NULL};
    XrXirSourceRequest request={.session=session,.authority=&authority,.context=fixture->context,
        .stdlib_path=stdlib_path,.linkage_kind=XR_XIR_PROGRAM};
    XrXirSourceText text={.text=class_move_source,.length=sizeof(class_move_source)-1};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&text,&source,&diagnostic,NULL);
    fprintf(stderr,"class move Source status%u %d:%d %s\n",status,
        diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked && source.snapshot);
    CHECK(xr_xir_compile_source_snapshot_view(source.snapshot)->complete);
    fixture->source=source.checked;source.checked=NULL;
    xr_xir_compile_source_result_free(&source);xr_compile_session_free(session);
    memset(stdlib_path,0xa5,sizeof(stdlib_path));
    CHECK(xr_xir_compile_artifact_verify(fixture->source,NULL)==XR_XIR_OK);
}

static void class_move_fixture_types(ClassMoveFixture *fixture) {
    const XrXirModule *module=xr_xir_compile_artifact_module(fixture->source);
    CHECK(module && module->stage==XR_XIR_CHECKED && module->linkage_kind==XR_XIR_PROGRAM);
    fixture->base=class_move_class(module,"Base");fixture->dog=class_move_class(module,"Dog");
    const XrXirTypeNode *base=xr_xir_type_node(module->types,fixture->base);
    const XrXirTypeNode *dog=xr_xir_type_node(module->types,fixture->dog);
    const XrXirNominalTable *nominals=module->types->nominals;
    CHECK(nominals->declarations && !nominals->identities);
    CHECK(nominals->declarations[dog->nominal.declaration].parent==base->nominal.declaration+1);
    CHECK(base->nominal.field_count==1 && dog->nominal.field_count==2 &&
        base->nominal.fields[0]==XR_XIR_I64 && dog->nominal.fields[0]==XR_XIR_I64 &&
        dog->nominal.fields[1]==XR_XIR_I64);
    fixture->cell=XR_XIR_UNIT;
    for(uint32_t t=0;t<module->types->count;++t) {
        const XrXirTypeNode *node=module->types->nodes+t;
        if(node->kind!=XR_XIR_TYPE_CELL || node->element!=fixture->dog)continue;
        CHECK(fixture->cell==XR_XIR_UNIT);
        fixture->cell=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+t);
    }
    CHECK(fixture->cell!=XR_XIR_UNIT);
    const char *const names[]={"ancestorProbe","valueProbe","cellProbe"};
    for(uint32_t k=0;k<3;++k) {
        uint32_t f=class_move_function(module,names[k]);fixture->probes[k]=f;
        const XrXirFunction *function=module->functions+f;
        CHECK(function->parameter_count==1 && function->parameters[0]==fixture->dog &&
            function->error_type==XR_XIR_NEVER);
        CHECK(function->result==(k==CLASS_ORDINARY_COPY?fixture->base:fixture->dog));
        CHECK(function->consume_parameter_count==(k==CLASS_VALUE_TAKE_COPY?1u:0u));
        if(function->consume_parameter_count)
            CHECK(function->consume_parameters && function->consume_parameters[0]==0);
    }
}

static XrXirArtifact *class_move_check(const ClassMoveFixture *fixture,const XrXirModule *module,
    const char *operation) {
    XrXirArtifact *output=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_check_v2(fixture->context,module,
        xr_xir_compile_artifact_construction(fixture->source),&output,&diagnostic);
    class_move_status(operation,status,&diagnostic);
    CHECK(status==XR_XIR_OK && output);
    CHECK(xr_xir_compile_artifact_verify(output,NULL)==XR_XIR_OK);return output;
}

static void class_move_original_raw(const ClassMoveFixture *fixture) {
    uint64_t held=class_move_held(fixture->context);
    XrXirModule module=*xr_xir_compile_artifact_module(fixture->source);
    /* This local raw view makes no Source-correspondence or execution claim. */
    module.stage=XR_XIR_BUILT;module.provenance=NULL;
    XrXirArtifact *checked=class_move_check(fixture,&module,"unmodified raw module");
    xr_xir_compile_artifact_free(checked);
    CHECK(class_move_held(fixture->context)==held);
}

static void class_move_case_init(const ClassMoveFixture *fixture,ClassMoveKind kind,
    ClassMoveCase *output) {
    memset(output,0,sizeof(*output));
    output->module=*xr_xir_compile_artifact_module(fixture->source);
    size_t function_count=output->module.function_count;
    CHECK(function_count<=SIZE_MAX/sizeof(*output->functions));
    output->functions=xr_malloc(function_count*sizeof(*output->functions));
    CHECK(output->functions);
    memcpy(output->functions,output->module.functions,function_count*sizeof(*output->functions));
    output->module.functions=output->functions;
    output->module.stage=XR_XIR_BUILT;output->module.provenance=NULL;
    output->function=fixture->probes[kind];
    XrXirFunction *function=output->functions+output->function;
    uint32_t count=0,source=0;
    if(kind==CLASS_VALUE_TAKE_COPY) {
        output->instructions[count++]=(XrXirInstruction){.op=XR_XIR_VALUE_TAKE,
            .type=fixture->dog,.args={0}};
        source=1;
    } else if(kind==CLASS_CELL_TAKE_COPY) {
        output->instructions[count++]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,
            .type=fixture->cell,.args={0}};
        output->instructions[count++]=(XrXirInstruction){.op=XR_XIR_CELL_TAKE,
            .type=fixture->dog,.args={1}};
        source=2;
    }
    output->copy=count;
    XrXirType result=kind==CLASS_ORDINARY_COPY?fixture->base:fixture->dog;
    output->instructions[count++]=(XrXirInstruction){.op=XR_XIR_COPY,.type=result,.args={source}};
    output->instructions[count]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={count}};
    ++count;
    output->block=(XrXirBlock){.count=count};
    function->result=result;function->instructions=output->instructions;function->instruction_count=count;
    function->blocks=&output->block;function->block_count=1;function->operands=NULL;function->operand_count=0;
}

static void class_move_copy_refusal(const ClassMoveFixture *fixture,ClassMoveCase *test) {
    XrXirFunction *function=test->functions+test->function;
    XrXirType saved=function->result;CHECK(saved==fixture->dog);
    XrXirInstruction *copy=test->instructions+test->copy;
    CHECK(copy->op==XR_XIR_COPY && copy->type==saved);
    copy->type=fixture->base;function->result=fixture->base;
    XrXirArtifact *rejected=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_check_v2(fixture->context,&test->module,
        xr_xir_compile_artifact_construction(fixture->source),&rejected,&diagnostic);
    class_move_status("ancestor COPY refusal",status,&diagnostic);
    CHECK(status==XR_XIR_UNSUPPORTED && diagnostic.status==status && !rejected);
    CHECK(diagnostic.function==test->function && diagnostic.block==0 && diagnostic.instruction==test->copy);
    copy->type=saved;function->result=saved;
    XrXirArtifact *restored=class_move_check(fixture,&test->module,"restored same-type COPY");
    xr_xir_compile_artifact_free(restored);
}

static void class_move_retain_refusal(const ClassMoveFixture *fixture,XrXirArtifact *lowered,
    uint32_t function_id,uint32_t instruction) {
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    XrXirType base=class_move_class(module,"Base"),dog=class_move_class(module,"Dog");
    const XrXirTarget *target=xr_xir_compile_artifact_target(lowered);
    XrXirLayout base_layout={0},dog_layout={0},base_result={0},dog_result={0};
    CHECK(xr_xir_compile_layout(fixture->context,module->types,base,target,
        XR_XIR_LAYOUT_FRAME,&base_layout)==XR_XIR_OK);
    CHECK(xr_xir_compile_layout(fixture->context,module->types,dog,target,
        XR_XIR_LAYOUT_FRAME,&dog_layout)==XR_XIR_OK);
    CHECK(base_layout.size==8 && dog_layout.size==8 && base_layout.alignment==dog_layout.alignment);
    CHECK(xr_xir_compile_layout(fixture->context,module->types,base,target,
        XR_XIR_LAYOUT_RESULT,&base_result)==XR_XIR_OK);
    CHECK(xr_xir_compile_layout(fixture->context,module->types,dog,target,
        XR_XIR_LAYOUT_RESULT,&dog_result)==XR_XIR_OK);
    CHECK(base_result.size==sizeof(XrXirValue) && base_result.size==dog_result.size &&
        base_result.alignment==dog_result.alignment);
    XrXirFunction *function=(XrXirFunction *)(uintptr_t)(module->functions+function_id);
    CHECK(instruction<function->instruction_count && function->result==dog);
    XrXirInstruction *retain=(XrXirInstruction *)(uintptr_t)(function->instructions+instruction);
    CHECK(retain->op==XR_XIR_OWNED_RETAIN && retain->type==dog);
    const XrXirFunctionLayout *layout=xr_xir_compile_artifact_layout(lowered,function_id);
    CHECK(layout && layout->result.size==dog_result.size &&
        layout->result.alignment==dog_result.alignment);
    CHECK(function->parameter_count+instruction<layout->slot_count);
    CHECK(layout->offsets[function->parameter_count+instruction]<=layout->frame_bytes &&
        layout->frame_bytes-layout->offsets[function->parameter_count+instruction]>=8);
    /* Only this independently checked raw descendant is mutable. The original
     * Source artifact and every constructor fact remain untouched. */
    retain->type=base;function->result=base;
    XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_artifact_verify(lowered,&diagnostic);
    class_move_status("ancestor OWNED_RETAIN refusal",status,&diagnostic);
    CHECK(status==XR_XIR_UNSUPPORTED && diagnostic.status==status);
    CHECK(diagnostic.function==function_id && diagnostic.block==0 && diagnostic.instruction==instruction);
    retain->type=dog;function->result=dog;
    CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);
}

static void class_move_lowered(const ClassMoveFixture *fixture,const ClassMoveCase *test,
    const XrXirArtifact *checked,ClassMoveKind kind) {
    XrXirArtifact *specialized=NULL,*lowered=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_specialize(checked,&specialized,&diagnostic,NULL);
    class_move_status("raw specialization",status,&diagnostic);
    CHECK(status==XR_XIR_OK && specialized);
    const XrXirModule *closed=xr_xir_compile_artifact_module(specialized);
    const char *const names[]={"ancestorProbe","valueProbe","cellProbe"};
    uint32_t selected=class_move_function(closed,names[kind]),origin=UINT32_MAX;
    CHECK(xr_xir_compile_specialization_origin(specialized,selected,&origin)==XR_XIR_OK &&
        origin==test->function);
    CHECK(closed->functions[selected].instruction_count==test->block.count);
    const XrXirTarget target={XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION};
    status=xr_xir_compile_lower(specialized,&target,NULL,&lowered,&diagnostic);
    class_move_status("raw Lowered positive",status,&diagnostic);
    CHECK(status==XR_XIR_OK && lowered);
    CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);
    const XrXirModule *physical=xr_xir_compile_artifact_module(lowered);
    const XrXirModule *source=xr_xir_compile_artifact_module(fixture->source);
    CHECK(physical!=source && physical->functions!=source->functions &&
        physical->functions[selected].instructions!=source->functions[test->function].instructions);
    CHECK(physical->functions[selected].instructions[test->copy].op==XR_XIR_OWNED_RETAIN);
    if(kind!=CLASS_ORDINARY_COPY)class_move_retain_refusal(fixture,lowered,selected,test->copy);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(specialized);
}

static void class_move_case(const ClassMoveFixture *fixture,ClassMoveKind kind) {
    uint64_t held=class_move_held(fixture->context);ClassMoveCase test;
    class_move_case_init(fixture,kind,&test);
    XrXirArtifact *checked=class_move_check(fixture,&test.module,"raw COPY positive");
    if(kind!=CLASS_ORDINARY_COPY)class_move_copy_refusal(fixture,&test);
    class_move_lowered(fixture,&test,checked,kind);
    xr_xir_compile_artifact_free(checked);xr_free(test.functions);
    CHECK(class_move_held(fixture->context)==held);
    CHECK(xr_xir_compile_artifact_verify(fixture->source,NULL)==XR_XIR_OK);
}

int main(int argc,char **argv) {
    CHECK(argc==2);XrXirCompileContext context={0};
    const XrCompileResourceLimits limits={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();uint64_t held=class_move_held(&context);
    ClassMoveFixture fixture={.context=&context};
    class_move_source_owner(&fixture,argv[1]);class_move_fixture_types(&fixture);
    class_move_original_raw(&fixture);
    for(uint32_t k=0;k<3;++k)class_move_case(&fixture,(ClassMoveKind)k);
    xr_xir_compile_artifact_free(fixture.source);
    CHECK(class_move_held(&context)==held);xr_compile_resources_release(context.resources);
    puts("class Common COPY/OWNED_RETAIN: ordinary ancestor and same-type takes pass; take upcasts reject");
    return 0;
}
