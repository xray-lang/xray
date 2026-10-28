/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_cleanup_source.c - Source body cleanup on real VM exits
 *
 * KEY CONCEPT:
 *   Real generated and interpreted bodies share the canonical exit driver.
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
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_source_fixture_owner.h"
#include "xir_generator_cleanup_cases.h"
static const char cleanup_source[]=
    "enum E { Bad }\n"
    "fn normal(first:string,second:string)->Iterator<string> { defer { print(301) }\n yield first\n Coro.yield()\n yield second\n }\n"
    "fn failing()->Iterator<i64> { defer { print(302) }\n yield 7\n throw E.Bad\n }\n"
    "fn units()->Iterator<()> { defer { print(303) }\n yield ()\n }\n"
    "fn main(){}\n";
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"generator-cleanup",NULL};
    XrXirSourceRequest request={session,NULL,&authority,&owner.context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,
        &(XrXirSourceText){NULL,cleanup_source,sizeof(cleanup_source)-1},&source,&diagnostic,NULL);
    if(status!=XR_XIR_OK) fprintf(stderr,"Generator cleanup Source status%u line%d: %s\n",status,diagnostic.line,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked);xr_compile_session_free(session);
    XrXirCheckedPacket packet={0};XrXirArtifact *decoded=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(decoded,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(decoded);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}, NULL,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    CHECK(module->function_count>3 && module->functions[1].name_length==6 && !memcmp(module->functions[1].name,"normal",6) &&
        module->functions[2].name_length==7 && !memcmp(module->functions[2].name,"failing",7) &&
        module->functions[3].name_length==5 && !memcmp(module->functions[3].name,"units",5));
    XrXirVmBinding *bindings=NULL;XrXirCallEntry *entries=NULL;
    CHECK(xr_xir_compile_vm_bind_table(lowered,&bindings,&entries)==XR_XIR_OK);
    XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL;
    CHECK(xr_xir_compile_type_arena_new(&owner.context,module->types,&arena)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    generator_cleanup_cases(entries,module->function_count,arena,domain);
    XrXirCSource c={0};CHECK(xr_xir_compile_emit_c(lowered,"generator_cleanup",1048576,&c)==XR_XIR_OK);
    CHECK(c.text && c.length && !strstr(c.text,"({"));
    if(argc==2) {FILE *file=fopen(argv[1],"wb");CHECK(file);CHECK(fwrite(c.text,1,c.length,file)==c.length);CHECK(!fclose(file));}
    xr_xir_compile_c_source_free(&c);xr_xir_compile_type_arena_drop(arena);xr_xir_domain_drop(domain);
    xr_compile_resources_free(entries);xr_compile_resources_free(bindings);xr_xir_compile_artifact_free(lowered);
    source_fixture_owner_free(&owner);puts("Real Generator VM cleanup once: unstarted, yielded, suspended, normal, thrown, Unit and independent activations PASS");return 0;
}
