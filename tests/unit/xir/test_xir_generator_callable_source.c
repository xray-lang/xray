/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_callable_source.c - Callable arguments cross fixed generator factory boundaries
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_source_fixture_owner.h"
#include "xir/xxir_source.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include "xir_generator_callable_cases.h"
#include "xir_callable_argument_admission.h"
#include "../test_win_compat.h"
static const char callable_source[]=
    "import time\nvar completed:i64=0\n"
    "interface P {values(f:fn()->i64)->Iterator<i64>}\n"
    "struct Method implements P {values(f:fn()->i64)->Iterator<i64> {defer {completed=completed+1\n print(401)}\n"
    " time.sleep(5)\n yield f()}}\n"
    "fn deferred(f:fn()->i64)->Iterator<i64> {defer {completed=completed+1\n print(401)}\n"
    " time.sleep(5)\n yield f()}\n"
    "struct Factory implements P {values(f:fn()->i64)->Iterator<i64>{return deferred(f)}}\n"
    "fn gather(it:Iterator<i64>)->i64 {var sum=0\n for(value in it){sum=sum+value}\n return sum}\n"
    "fn use<S:P>(p:S)->i64 {return gather(p.values(fn()->i64{return 7}))}\n"
    "export fn witness_method()->i64 {return use(Method{})}\n"
    "export fn witness_factory()->i64 {return use(Factory{})}\n"
    "export fn direct_method()->i64 {return gather(Method{}.values(fn()->i64{return 7}))}\n"
    "export fn direct_factory()->i64 {return gather(Factory{}.values(fn()->i64{return 7}))}\n"
    "export fn stats()->i64 {return completed}\n"
    "fn emit_good(f:fn()->i64)->i64{return 0}\nfn emit_plain(f:fn()->i64)->i64{return 0}\n"
    "fn emit_wrong(f:fn(i64)->i64)->i64{return 0}\n"
    "export fn good_value()->i64{return emit_good(fn()->i64{return 7})}\n"
    "export fn plain_value()->i64{return emit_plain(fn()->i64{return 7})}\n"
    "export fn wrong_value()->i64{return emit_wrong(fn(value:i64)->i64{return value})}\n"
    "export fn takes(f:fn()->i64)->i64{return f()}\nexport fn dispatch()->i64{return 0}\nfn main(){}\n";
static void callable_source_check(SourceFixtureOwner *owner,XrXirSourceResult *source) {
    char directory[]="xir-callable-arguments-XXXXXX",absolute[4096],entry[8192],manifest[8192];
    CHECK(xr_test_mkdtemp(directory) && xr_test_realpath_buf(directory,absolute,sizeof(absolute)));
    CHECK(snprintf(entry,sizeof(entry),"%s/root.xr",absolute)>0);
    CHECK(snprintf(manifest,sizeof(manifest),"%s/xray.toml",absolute)>0);
    FILE *file=fopen(entry,"wb");CHECK(file);
    CHECK(fwrite(callable_source,1,sizeof(callable_source)-1,file)==sizeof(callable_source)-1 && !fclose(file));
    file=fopen(manifest,"wb");CHECK(file);
    CHECK(fputs("[declarations]\nversion=1\n",file)>=0);
    const char *owners[]={"P","Method","Factory",NULL};
    for(unsigned n=0;n<4;++n) {
        CHECK(fprintf(file,"[[declarations.function]]\nmodule=\"root.xr\"\nname=\"%s\"\n",n<3 ? "values" : "deferred")>0);
        if(n<3) CHECK(fprintf(file,"owner=\"%s\"\nno_suspend=true\n",owners[n])>0);
        CHECK(fputs("no_suspend_parameters=[\"f\"]\n",file)>=0);
    }
    CHECK(fputs("[[declarations.function]]\nmodule=\"root.xr\"\nname=\"emit_good\"\nno_suspend=true\nno_suspend_parameters=[\"f\"]\n"
        "[[declarations.function]]\nmodule=\"root.xr\"\nname=\"emit_wrong\"\nno_suspend=true\nno_suspend_parameters=[\"f\"]\n"
        "[[declarations.function]]\nmodule=\"root.xr\"\nname=\"takes\"\nno_suspend=true\nno_suspend_parameters=[\"f\"]\n",file)>=0);
    CHECK(!fclose(file));XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(owner->context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_PROJECT,"generator-callable-arguments",absolute};
    XrXirSourceRequest request={session,entry,&authority,&owner->context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,source,&diagnostic,NULL);
    if(status!=XR_XIR_OK) fprintf(stderr,"Callable Source status%u line%d: %s\n",status,diagnostic.line,diagnostic.message);
    CHECK(status==XR_XIR_OK && source->checked);xr_compile_session_free(session);
    CHECK(!xr_test_unlink(entry) && !xr_test_unlink(manifest) && !xr_test_rmdir(directory));
}
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrXirSourceResult source={0};callable_source_check(&owner,&source);
    XrXirCheckedPacket packet={0};XrXirArtifact *decoded=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_source_result_free(&source);
    CHECK(xr_xir_compile_specialize(decoded,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(decoded);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}, NULL,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    const char *names[]={"witness_method","witness_factory","direct_method","direct_factory","stats","good_value","plain_value","wrong_value","takes","dispatch"};uint32_t ids[10]={0};
    for(unsigned n=0;n<10;++n) for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==strlen(names[n]) && !memcmp(module->functions[f].name,names[n],strlen(names[n]))) ids[n]=f;
    for(unsigned n=0;n<10;++n) CHECK(ids[n]);
    XrXirCSource c={0};CHECK(xr_xir_compile_emit_c(lowered,"generator_callable",1048576,&c)==XR_XIR_OK);
    CHECK(c.text && c.length && !strstr(c.text,"({"));
    if(argc==2) {FILE *file=fopen(argv[1],"wb");CHECK(file);CHECK(fwrite(c.text,1,c.length,file)==c.length);
        CHECK(fprintf(file,"\nconst uint32_t generator_callable_ids[10]={%u,%u,%u,%u,%u,%u,%u,%u,%u,%u};\n",ids[0],ids[1],ids[2],ids[3],ids[4],ids[5],ids[6],ids[7],ids[8],ids[9])>0);CHECK(!fclose(file));}
    xr_xir_compile_c_source_free(&c);XrXirProgram *program=NULL;
    XrXirVmBinding *bindings=NULL;XrXirCallEntry *entries=NULL;
    CHECK(xr_xir_compile_vm_bind_table(lowered,&bindings,&entries)==XR_XIR_OK);
    callable_admission_entries(entries,module->function_count,module->types);entries[ids[9]].resume=callable_admission_resume;
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,*xr_xir_compile_artifact_target(lowered),entries,module->function_count,
        module->declarations,{0},module->types,xr_xir_compile_program_proof(lowered)};
    XrXirProgram *adapter=NULL,*foreign=NULL;
    CHECK(xr_xir_compile_program_seal(&owner.context,&spec,&adapter)==XR_XIR_OK);
    CHECK(xr_xir_compile_program_seal(&owner.context,&spec,&foreign)==XR_XIR_OK);
    callable_admission_cases(adapter,foreign,ids,module->types);xr_xir_compile_program_drop(foreign);xr_xir_compile_program_drop(adapter);
    xr_compile_resources_free(entries);xr_compile_resources_free(bindings);
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(owner.context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirStatus status=xr_xir_compile_vm_program_take(&lowered,&program);
    if(status!=XR_XIR_OK) {
        CHECK(xr_compile_resources_stats(owner.context.resources,&after)==XR_COMPILE_RESOURCE_OK);
        fprintf(stderr,"Callable Program take status%u before_work%llu work%llu allocated%llu live%llu\n",status,
            (unsigned long long)before.work,(unsigned long long)after.work,
            (unsigned long long)after.allocated_bytes,(unsigned long long)after.live_bytes);
    }
    CHECK(status==XR_XIR_OK && program && !lowered);
    generator_callable_cases(program,ids);xr_xir_compile_program_drop(program);
    source_fixture_owner_free(&owner);return 0;
}
