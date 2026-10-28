/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_generation_resources.c - Full Source generation physical faults
 */
#include "xir/xxir_source_generation.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_generic.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
typedef struct Allocation { void *pointer; size_t bytes; } Allocation;
static Allocation allocations[20000];
static size_t attempts, fail_at = SIZE_MAX;
static uint64_t physical;
static void *observed_alloc(size_t bytes) {
    if (attempts++ == fail_at) return NULL;
    void *memory = xr_malloc(bytes); if (!memory) return NULL;
    size_t i = 0; while (i < 20000 && allocations[i].pointer) ++i;
    CHECK(i < 20000); allocations[i] = (Allocation){memory, bytes}; physical += bytes;
    return memory;
}
static void observed_free(void *memory) {
    if (!memory) return;
    size_t i = 0; while (i < 20000 && allocations[i].pointer != memory) ++i;
    CHECK(i < 20000); physical -= allocations[i].bytes; allocations[i] = (Allocation){0};
    xr_free(memory);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc(n) observed_alloc(n)
#define xr_free(p) observed_free(p)
#include "base/xcompile_resources.c"

static const XrCompileResourceLimits limits = {
    UINT64_C(64)*1024*1024, UINT64_C(8)*1024*1024, UINT64_C(128000000)};
static XrXirStatus bind_program(XrXirSourceGenerations *owner,uint64_t serial) {
    XrXirSourceGenerationView view={0};XrXirArtifact *closed=NULL,*lowered=NULL;XrXirProgram *program=NULL;
    XrXirStatus status=xr_xir_compile_source_generation_view(owner,serial,&view);
    if (status==XR_XIR_OK) status=xr_xir_compile_specialize(view.result->checked,&closed,NULL);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    if (status==XR_XIR_OK) status=xr_xir_compile_lower(closed,&target, NULL,&lowered,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_vm_program_take(&lowered,&program);
    if (status==XR_XIR_OK) status=xr_xir_compile_source_generation_bind_program(owner,serial,program, NULL);
    xr_xir_compile_program_drop(program);xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(closed);
    return status;
}
static XrXirStatus pipeline(const XrCompileResourceLimits *bounds,XrCompileResourceStats *stats,
    bool comptime,bool history,bool program_binding,unsigned program_storage) {
    XrCompileResources *resources=NULL;XrXirSourceGenerations *owner=NULL;
    XrCompileResourceStatus opened=xr_compile_resources_new(bounds,&resources);
    if (opened!=XR_COMPILE_RESOURCE_OK) {
        CHECK(!resources && !physical);
        return opened==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    }
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrXirSourceGenerationConfig config={context,"generation-fi",NULL,NULL,NULL};
    XrXirStatus status=xr_xir_compile_source_generations_new(&config,&owner);
    if (status!=XR_XIR_OK) goto done;
    const char *first=comptime ? "const value=comptime (6*7)\nexport fn answer()->i64{return value}" :
        "const value=5\nexport fn answer()->i64{return value}";
    if (history) first="const value=5\nfn old()->i64{return 41}\nfn identity<T>(value:T)->T{return value}\nstruct Item{value:i64}\nexport fn seed()->i64{return 0}";
    if (program_storage==1) first="const value=5\nvar cell:i64=40\nfn old()->i64{return cell+1}\nfn identity<T>(value:T)->T{return value}\nstruct Item{value:i64}\nexport fn seed()->i64{return 0}";
    if (program_storage==2) first="const value=5\nvar cell:Array<i64> = [40]\nfn old()->i64{cell[0]=cell[0]+1;return cell[0]}\nfn identity<T>(value:T)->T{return value}\nstruct Item{value:i64}\nexport fn seed()->i64{return 0}";
    if (program_storage==3) first="const value=5\nconst cell:Array<i64> = [40]\nfn old()->i64{return cell[0]+1}\nfn identity<T>(value:T)->T{return value}\nstruct Item{value:i64}\nexport fn seed()->i64{return 0}";
    XrXirSourceText text={NULL,first,strlen(first)};uint64_t serial=0;
    status=xr_xir_compile_source_generation_prepare(owner,&text,&serial,NULL);
    if (status!=XR_XIR_OK) { CHECK(!serial);goto done; }
    if (program_binding && (status=bind_program(owner,serial))!=XR_XIR_OK) goto done;
    CHECK(xr_xir_compile_source_generation_publish(owner,serial)==XR_XIR_OK);
    XrXirSourceGenerationDefinition definition={0};
    status=xr_xir_compile_source_generation_definition(owner,serial,"value",&definition);
    if (status!=XR_XIR_OK) goto done;
    const XrXirModule *module=xr_xir_compile_artifact_module(definition.checked);
    CHECK(definition.kind==XR_XIR_SOURCE_DEFINITION_BINDING && definition.stored_type==XR_XIR_I64 &&
        definition.syntax && definition.ordinal<module->declarations->slot_count &&
        module->declarations->slots[definition.ordinal].type==definition.stored_type);
    XrXirSourceGenerationBinding before={0},after={0};
    status=xr_xir_compile_source_generation_lookup(owner,"value",&before);
    if (status!=XR_XIR_OK) goto done;
    CHECK(before.generation==serial && before.declaration->type.type==XR_XIR_I64);
    const char *second=history ? "const value=\"next\"\nexport fn answer()->i64{const item=Item{value:identity<i64>(old()+1)};return item.value}" :
        "const value=\"next\"\nexport fn answer()->string{return value}";
    text=(XrXirSourceText){NULL,second,strlen(second)};uint64_t candidate=0;
    status=xr_xir_compile_source_generation_prepare(owner,&text,&candidate,NULL);
    if (status!=XR_XIR_OK) {
        CHECK(!candidate);
        if (status==XR_XIR_OUT_OF_MEMORY) {
            CHECK(xr_xir_compile_source_generation_lookup(owner,"value",&after)==XR_XIR_OK);
            CHECK(before.generation==after.generation && before.declaration==after.declaration);
        }
        goto done;
    }
    CHECK(candidate>serial && xr_xir_compile_source_generation_lookup(owner,"value",&after)==XR_XIR_OK);
    CHECK(before.generation==after.generation && before.declaration==after.declaration);
    if (program_binding && (status=bind_program(owner,candidate))!=XR_XIR_OK) goto done;
    CHECK(xr_xir_compile_source_generation_abort(owner,candidate)==XR_XIR_OK);
    candidate=0;status=xr_xir_compile_source_generation_prepare(owner,&text,&candidate,NULL);
    if (status!=XR_XIR_OK) { CHECK(!candidate);goto done; }
    if (program_binding && (status=bind_program(owner,candidate))!=XR_XIR_OK) goto done;
    CHECK(xr_xir_compile_source_generation_publish(owner,candidate)==XR_XIR_OK);
    status=xr_xir_compile_source_generation_definition(owner,candidate,"value",&definition);
    if (status!=XR_XIR_OK) goto done;
    CHECK(definition.kind==XR_XIR_SOURCE_DEFINITION_BINDING && definition.stored_type==XR_XIR_STRING);
    status=xr_xir_compile_source_generation_lookup(owner,"value",&after);
    if (status==XR_XIR_OK) CHECK(after.generation==candidate && after.declaration->type.type==XR_XIR_STRING);
done:
    xr_xir_compile_source_generations_free(owner);
    CHECK(xr_compile_resources_stats(resources,stats)==XR_COMPILE_RESOURCE_OK);
    xr_compile_resources_release(resources);CHECK(!physical);return status;
}
int main(int argc,char **argv) {
    bool comptime=false,baseline=false,history=false,program_binding=false;unsigned program_storage=0;
    for (int i=1;i<argc;++i) {
        if (!strcmp(argv[i],"--comptime")) {CHECK(!comptime);comptime=true;}
        else if (!strcmp(argv[i],"--history")) {CHECK(!history);history=true;}
        else if (!strcmp(argv[i],"--program")) {CHECK(!program_binding);program_binding=true;history=true;}
        else if (!strcmp(argv[i],"--program-cell")) {CHECK(!program_binding);program_binding=true;program_storage=1;history=true;}
        else if (!strcmp(argv[i],"--program-array")) {CHECK(!program_binding);program_binding=true;program_storage=2;history=true;}
        else if (!strcmp(argv[i],"--program-array-const")) {CHECK(!program_binding);program_binding=true;program_storage=3;history=true;}
        else {CHECK(!strcmp(argv[i],"--baseline") && !baseline);baseline=true;}
    }
    XrCompileResourceStats normal={0},observed={0};
    CHECK(!comptime || !history);
    CHECK(pipeline(&limits,&normal,comptime,history,program_binding,program_storage)==XR_XIR_OK);
    size_t sites=attempts;CHECK(sites && sites<20000);
    printf("source generation sites=%zu allocated=%llu peak=%llu work=%llu\n",sites,
        (unsigned long long)normal.allocated_bytes,(unsigned long long)normal.peak_bytes,(unsigned long long)normal.work);
    fflush(stdout);
    if (baseline) return 0;
    for (size_t i=0;i<sites;++i) {
        attempts=0;fail_at=i;XrXirStatus status=pipeline(&limits,&observed,comptime,history,program_binding,program_storage);
        if (status!=XR_XIR_OUT_OF_MEMORY) fprintf(stderr,"site=%zu status=%u\n",i,(unsigned)status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY);
    }
    fail_at=SIZE_MAX;attempts=0;
    XrCompileResourceLimits exact={normal.allocated_bytes,normal.peak_bytes,normal.work};
    CHECK(pipeline(&exact,&observed,comptime,history,program_binding,program_storage)==XR_XIR_OK);
    CHECK(observed.allocated_bytes==normal.allocated_bytes && observed.peak_bytes==normal.peak_bytes && observed.work==normal.work);
    for (unsigned axis=0;axis<3;++axis) {
        XrCompileResourceLimits short_limit=exact;
        if (!axis) --short_limit.allocated_bytes;
        else if (axis==1) --short_limit.live_bytes;
        else --short_limit.work;
        attempts=0;CHECK(pipeline(&short_limit,&observed,comptime,history,program_binding,program_storage)==XR_XIR_BUDGET);
    }
    CHECK(!physical);
    puts("Source generation full physical FI, staged old-binding preservation, exact/minus-one budgets and zero physical allocations passed");
    return 0;
}
