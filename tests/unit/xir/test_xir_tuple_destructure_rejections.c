/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_tuple_destructure_rejections.c - Complete declaration and definition checks
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_source_program_compile_owner.h"
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
int main(void) {
    const struct {const char *name;XrXirStatus expected;bool parser_failure;} cases[]={
        {"arity",XR_XIR_BAD_TYPE,false},{"non_tuple",XR_XIR_BAD_TYPE,false},{"non_unit",XR_XIR_BAD_TYPE,false},
        {"duplicate_group",XR_XIR_BAD_STRUCTURE,false},{"duplicate_scope",XR_XIR_BAD_STRUCTURE,false},
        {"singleton_comma",XR_XIR_BAD_STRUCTURE,true},{"const_write",XR_XIR_BAD_TYPE,false},
        {"unused_generic",XR_XIR_BAD_TYPE,false},{"nested_pending",XR_XIR_OK,false},
        {"rest_pending",XR_XIR_BAD_STRUCTURE,true},{"library_mutable",XR_XIR_OK,false}};
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        const XrXirCompileContext *context=source_program_owner(UINT64_C(64)*1024*1024,128000000);
        XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
        char entry[1024];int length=snprintf(entry,sizeof(entry),"%s/%s/root.xr",XR_TUPLE_DESTRUCTURE_REJECTED,cases[i].name);
        CHECK(length>0 && (size_t)length<sizeof(entry));
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_TUPLE_DESTRUCTURE_REJECTED};
        XrXirSourceProductRequest request={{session,entry,&authority,context,XR_TUPLE_DESTRUCTURE_STDLIB,NULL,
            XR_XIR_PROGRAM,NULL},{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
        XrXirSourceProduct *product=NULL;XrXirSourceProductDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_source_product_build(&request,&product,&diagnostic);
        if(status!=cases[i].expected)fprintf(stderr,"%s got=%u expected=%u stage=%u line=%d: %s\n",
            cases[i].name,status,cases[i].expected,diagnostic.stage,diagnostic.source.line,diagnostic.source.message);
        if (cases[i].expected==XR_XIR_OK && !strcmp(cases[i].name,"nested_pending")) {
            CHECK(status==XR_XIR_OK && product);
            CHECK(xr_xir_compile_source_product_verify(product,1048576,NULL)==XR_XIR_OK);
            XrXirSourceProductPacketView packet={0};XrXirArtifact *checked=NULL;
            CHECK(xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet)==XR_XIR_OK);
            CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
            const XrXirModule *module=xr_xir_compile_artifact_module(checked);
            const XrXirDeclarations *declarations=module->declarations;
            CHECK(declarations && declarations->slot_count==3);
            for (uint32_t s=0;s<3;++s)
                CHECK(declarations->slots[s].module==declarations->root_module && declarations->slots[s].mutable);
            uint32_t groups=0;
            for (uint32_t f=0;f<module->function_count;++f)
                for (uint32_t op=0;op<module->functions[f].instruction_count;++op) {
                    const XrXirInstruction *instruction=&module->functions[f].instructions[op];
                    if (instruction->op==XR_XIR_SLOT_GROUP_INIT) {
                        CHECK((uint64_t)instruction->immediate==3);++groups;
                    }
                }
            CHECK(groups==1);xr_xir_compile_artifact_free(checked);
            xr_xir_compile_source_product_free(product);xr_xir_compile_source_product_diagnostic_free(&diagnostic);
            xr_compile_session_free(session);source_program_owners_free();
            puts("Nested Tuple declaration preserves three mutable root slots and one atomic group");continue;
        }
        if (cases[i].expected==XR_XIR_OK) {
            CHECK(status==XR_XIR_OK && product);
            CHECK(xr_xir_compile_source_product_verify(product,1048576,NULL)==XR_XIR_OK);
            XrXirSourceProductPacketView packet={0};XrXirArtifact *checked=NULL;
            CHECK(xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_SOURCE,&packet)==XR_XIR_OK);
            CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
            const XrXirDeclarations *d=xr_xir_compile_artifact_module(checked)->declarations;
            CHECK(d && d->slot_count==2 && d->slots[0].module!=d->root_module &&
                d->slots[1].module==d->slots[0].module && d->slots[0].mutable && d->slots[1].mutable);
            xr_xir_compile_artifact_free(checked);xr_xir_compile_source_product_free(product);
            xr_xir_compile_source_product_diagnostic_free(&diagnostic);xr_compile_session_free(session);
            source_program_owners_free();puts("Private library Tuple state admitted through complete SourceProduct checking");
            continue;
        }
        CHECK(status==cases[i].expected && !product && diagnostic.stage==XR_XIR_SOURCE_PRODUCT_CHECK);
        /* Parser failures precede a typed Source diagnostic; their exact
         * lexer spans are printed by the existing frontend diagnostic owner. */
        CHECK(cases[i].parser_failure ? diagnostic.source.line==0 : diagnostic.source.line>0);
        xr_xir_compile_source_product_diagnostic_free(&diagnostic);xr_compile_session_free(session);
        printf("Tuple declaration %s rejected at definition status=%u, output empty\n",cases[i].name,status);
        source_program_owners_free();
    }
    puts("Nine rejected declarations, nested Tuple and private library state, physical0 PASS");return 0;
}
