/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_factory_source.c - Authentic Source factories create independent frames
 *
 * KEY CONCEPT:
 *   Only the owning executor holds code authority for a live producer.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_source_fixture_owner.h"
#include "xir_construction_fixture.h"
#include "xir_generator_factory_cases.h"
/* The owning checker receives real fields; no serialized identity is asserted. */
typedef struct FactoryModuleFixture {
    XrXirTypeNode nodes[2];XrXirTypes types;XrXirType cell;
    XrXirInstruction init,producer[3],factory[4],entry[5];XrXirBlock blocks[4];
    uint32_t factory_actual,entry_actual;XrXirFunction functions[4];
    XrXirFunctionIdentity identities[4];XrXirSourceModule source;
    XrXirDeclarations declarations;XrXirModule module;
} FactoryModuleFixture;
static void factory_module_fixture(FactoryModuleFixture *f,unsigned variant) {
    memset(f,0,sizeof(*f));bool unit=variant==1,cell=variant>=8,borrowed=variant==9;
    const XrXirType iterator=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    f->cell=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1);
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ITERATOR,
        .element=unit?XR_XIR_UNIT:XR_XIR_I64,.result=XR_XIR_NEVER};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64};
    f->types=(XrXirTypes){f->nodes,cell?2u:1u,NULL,NULL};
    f->init=(XrXirInstruction){.op=XR_XIR_RETURN};
    unsigned producer_count=unit?2u:3u;
    f->producer[0]=unit?(XrXirInstruction){.op=XR_XIR_GENERATOR_YIELD}:
        cell?(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=XR_XIR_I64,.args={0}}:
        (XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=11};
    if(!unit)f->producer[1]=(XrXirInstruction){.op=XR_XIR_GENERATOR_YIELD,.args={cell?1u:0u}};
    f->producer[producer_count-1]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->factory[0]=(XrXirInstruction){.op=XR_XIR_GENERATOR_NEW,.type=iterator,.immediate=variant==3?2:1};
    f->factory[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0}};
    unsigned factory_count=2,entry_count=2;
    if(variant==4)f->factory[0].type=XR_XIR_I64;
    if(variant==5 || variant==7) {
        f->factory[2]=f->factory[1];f->factory[2].args[0]=1;
        f->factory[1]=f->factory[0];
        f->factory[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
        if(variant==5){f->factory[1].args[1]=1;f->functions[2].operands=&f->factory_actual;f->functions[2].operand_count=1;}
        else f->factory[1]=(XrXirInstruction){.op=XR_XIR_GENERATOR_YIELD,.args={0}};
        factory_count=3;
    }
    if(variant==6)f->factory[0].targets[0]=1;
    if(cell) {
        f->factory_actual=borrowed?0u:1u;
        if(borrowed) {
            f->factory[0].args[1]=1;f->factory[1].args[0]=1;
        } else {
            f->factory[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
            f->factory[1]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=f->cell,.args={0}};
            f->factory[2]=(XrXirInstruction){.op=XR_XIR_GENERATOR_NEW,.type=iterator,.args={0,1},.immediate=1};
            f->factory[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2}};factory_count=4;
        }
        f->functions[2].operands=&f->factory_actual;f->functions[2].operand_count=1;
    }
    f->entry[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64};
    f->entry[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0}};
    if(borrowed) {
        f->entry[0].immediate=7;
        f->entry[1]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=f->cell,.args={0}};
        f->entry[2]=(XrXirInstruction){.op=XR_XIR_CALL,.type=iterator,.args={0,1},.immediate=2};
        f->entry[3]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64};
        f->entry[4]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={3}};
        f->entry_actual=1;f->functions[3].operands=&f->entry_actual;f->functions[3].operand_count=1;entry_count=5;
    }
    const char *names[]={"init","numbers","make","entry"};
    XrXirInstruction *ops[]={&f->init,f->producer,f->factory,f->entry};
    const uint32_t counts[]={1,producer_count,factory_count,entry_count};
    for(unsigned n=0;n<4;++n) {
        f->blocks[n]=(XrXirBlock){.count=counts[n]};
        f->functions[n].name=names[n];f->functions[n].name_length=(uint32_t)strlen(names[n]);
        f->functions[n].result=n==0?XR_XIR_UNIT:n==3?XR_XIR_I64:iterator;
        f->functions[n].error_type=XR_XIR_NEVER;f->functions[n].blocks=&f->blocks[n];
        f->functions[n].block_count=1;f->functions[n].instructions=ops[n];f->functions[n].instruction_count=counts[n];
    }
    if(cell){f->functions[1].parameters=&f->cell;f->functions[1].parameter_count=1;}
    if(borrowed){f->functions[2].parameters=&f->cell;f->functions[2].parameter_count=1;}
    f->identities[1].promises=XR_XIR_FUNCTION_GENERATOR;f->identities[2].exported=1;
    if(variant==2)f->identities[2].promises=XR_XIR_FUNCTION_NO_SUSPEND;
    f->source=(XrXirSourceModule){"factory",7,NULL,0,0};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,.entry_function=3};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=4,
        .types=&f->types,.declarations=&f->declarations,.linkage_kind=XR_XIR_PROGRAM};
}
static void factory_module_models(const XrXirCompileContext *context) {
    static const struct {const char *name;XrXirStatus expected;} cases[]={
        {"i64",XR_XIR_OK},{"unit",XR_XIR_OK},{"no_suspend",XR_XIR_OK},
        {"ordinary_target",XR_XIR_BAD_TYPE},{"scalar_result",XR_XIR_BAD_TYPE},
        {"argument_count",XR_XIR_BAD_STRUCTURE},{"targets",XR_XIR_BAD_STRUCTURE},
        {"yield_in_factory",XR_XIR_BAD_TYPE},{"owned_cell",XR_XIR_OK},{"borrowed_cell",XR_XIR_BAD_VALUE}};
    for(unsigned n=0;n<sizeof(cases)/sizeof(*cases);++n) {
        FactoryModuleFixture fixture;factory_module_fixture(&fixture,n);
        XrXirArtifact *checked=NULL,*lowered=NULL;XrXirDiagnostic diagnostic={0};
        XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,&diagnostic);
        if(status!=cases[n].expected)fprintf(stderr,"Factory Module %s status%u expected%u f%u i%u\n",
            cases[n].name,status,cases[n].expected,diagnostic.function,diagnostic.instruction);
        CHECK(status==cases[n].expected);
        if(status==XR_XIR_OK) {
            CHECK(checked);memset(&fixture,0xa5,sizeof(fixture));
            CHECK(xr_xir_compile_lower(checked,&(XrXirTarget){XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},
                NULL,&lowered,&diagnostic)==XR_XIR_OK);
            CHECK(xr_xir_compile_artifact_verify(lowered,&diagnostic)==XR_XIR_OK);
        } else CHECK(!checked);
        xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(checked);
    }
}


static const char factory_source[]=
    "fn numbers(seed:i64)->Iterator<i64> { defer {print(seed)}\n yield seed\n }\n"
    "export fn make(seed:i64)->Iterator<i64> {return numbers(seed)}\n"
    "export fn discard(){numbers(9)}\n"
    "fn main(){}\n";
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"generator-factory",NULL};
    XrXirSourceRequest request={session,NULL,&authority,&owner.context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,
        &(XrXirSourceText){NULL,factory_source,sizeof(factory_source)-1},&source,&diagnostic,NULL);
    if(status!=XR_XIR_OK) fprintf(stderr,"Factory Source status%u line%d: %s\n",status,diagnostic.line,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked);xr_compile_session_free(session);
    XrXirCheckedPacket packet={0};XrXirArtifact *decoded=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_source_result_free(&source);
    XrXirDiagnostic detail={0};status=xr_xir_compile_specialize(decoded,&closed,&detail, NULL);
    if(status!=XR_XIR_OK) fprintf(stderr,"Factory specialize status%u function%u block%u instruction%u\n",status,detail.function,detail.block,detail.instruction);
    CHECK(status==XR_XIR_OK);xr_xir_compile_artifact_free(decoded);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION}, NULL,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    CHECK(module->function_count>3 && module->functions[2].name_length==4 && !memcmp(module->functions[2].name,"make",4) &&
        module->functions[3].name_length==7 && !memcmp(module->functions[3].name,"discard",7));
    uint32_t factories=0;for(uint32_t f=0;f<module->function_count;++f)
        for(uint32_t i=0;i<module->functions[f].instruction_count;++i)
            factories+=module->functions[f].instructions[i].op==XR_XIR_GENERATOR_NEW;
    CHECK(factories==2);
    XrXirCSource c={0};CHECK(xr_xir_compile_emit_c(lowered,"generator_factory",1048576,&c)==XR_XIR_OK);
    CHECK(c.text && c.length && !strstr(c.text,"({"));
    if(argc==2){FILE *file=fopen(argv[1],"wb");CHECK(file);CHECK(fwrite(c.text,1,c.length,file)==c.length);CHECK(!fclose(file));}
    xr_xir_compile_c_source_free(&c);XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && program && !lowered);
    generator_factory_cases(program);factory_module_models(&owner.context);source_fixture_owner_free(&owner);
    puts("Source canonical factory creates owned parked Iterator; drop and Instance close retire frames before final code PASS");return 0;
}
