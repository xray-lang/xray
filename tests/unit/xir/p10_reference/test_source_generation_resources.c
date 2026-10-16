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
#include "xir/xxir_types.h"
#include "source_history_function_fixture.h"
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
typedef struct ProgramTrial { unsigned storage;size_t after_publish,after_abort; } ProgramTrial;
static XrXirStatus bind_program(XrXirSourceGenerations *owner,uint64_t serial) {
    XrXirSourceGenerationView view={0};XrXirArtifact *closed=NULL,*lowered=NULL;XrXirProgram *program=NULL;
    XrXirStatus status=xr_xir_compile_source_generation_view(owner,serial,&view);
    if (status==XR_XIR_OK) status=xr_xir_compile_specialize(view.result->checked,&closed,NULL);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    if (status==XR_XIR_OK) status=xr_xir_compile_lower(closed,&target,&lowered,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_vm_program_take(&lowered,&program);
    if (status==XR_XIR_OK) status=xr_xir_compile_source_generation_bind_program(owner,serial,program);
    xr_xir_compile_program_drop(program);xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(closed);
    return status;
}
static XrXirStatus pipeline(const XrCompileResourceLimits *bounds,XrCompileResourceStats *stats,
    bool comptime,bool history,bool program_binding,ProgramTrial *trial) {
    unsigned program_storage=trial->storage;
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
    if (program_storage==4) first="const value=5\nvar cell:Box=Box(40)\nfn old()->i64{cell.value+=1;return cell.value}\nfn identity<T>(value:T)->T{return value}\nstruct Item{value:i64}\nclass Box{value:i64 init(first:i64){this.value=first}}\nexport fn seed()->i64{return 0}";
    if (program_storage==5) first="const value=5\nvar cell:Box=Box(40)\nfn old()->i64{cell.value.set(0,cell.value[0]+1);return cell.value[0]}\nfn identity<T>(value:T)->T{return value}\nstruct Item{value:i64}\nclass Box{value:Array<i64> init(first:i64){this.value=[first]}}\nexport fn seed()->i64{return 0}";
    if (program_storage==6) first="const value=5\nvar cell:Array<i64> = [40]\nfn old()->i64{if(cell==[40]){return 41}else{return 0}}\nfn identity<T>(value:T)->T{return value}\nstruct Item{value:i64}\nexport fn seed()->i64{return 0}";
    if (program_storage==7) first="const value=5\nvar cell:Box=Box(\"a\")\nfn old()->i64{return counter+1}\nfn identity<T>(value:T)->T{return value}\nstruct Item{value:i64}\nclass Box{value:string init(first:string){this.value=first}}\nfn fetch()->Box{return cell}\nvar counter:i64=40\nexport fn seed()->i64{return 0}";
    if (program_storage==8) first="const value=5\nvar cell:Box=Box(40)\nfn old()->i64{return counter+1}\nfn identity<T>(value:T)->T{return value}\nstruct Item{value:i64}\nclass Box{value:Leaf init(first:i64){this.value=Leaf{value:first}}}\nstruct Leaf{value:i64}\nfn fetch()->Box{return cell}\nvar counter:i64=40\nexport fn seed()->i64{return 0}";
    if (program_storage>=9) first=source_history_function_input(1,program_storage==10);
    XrXirSourceText text={NULL,first,strlen(first)};uint64_t serial=0;
    status=xr_xir_compile_source_generation_prepare(owner,&text,&serial,NULL);
    if (status!=XR_XIR_OK) { CHECK(!serial);goto done; }
    if (program_binding && (status=bind_program(owner,serial))!=XR_XIR_OK) goto done;
    CHECK(xr_xir_compile_source_generation_publish(owner,serial)==XR_XIR_OK);
    XrXirSourceGenerationDefinition definition={0};
    status=xr_xir_compile_source_generation_definition(owner,serial,"value",&definition);
    if (status!=XR_XIR_OK) goto done;
    const XrXirModule *module=xr_xir_compile_artifact_module(definition.checked);
    CHECK(definition.kind==XR_XIR_SOURCE_DEFINITION_BINDING &&
        (program_storage>=9 ? definition.mutable && xr_xir_type_is_cell(module->types,definition.stored_type) :
            definition.stored_type==XR_XIR_I64) &&
        definition.syntax && definition.ordinal<module->declarations->slot_count &&
        module->declarations->slots[definition.ordinal].type==definition.stored_type);
    XrXirSourceGenerationBinding before={0},after={0};
    status=xr_xir_compile_source_generation_lookup(owner,"value",&before);
    if (status!=XR_XIR_OK) goto done;
    CHECK(before.generation==serial);
    if (program_storage>=9) {
        const XrXirTypeNode *signature=xr_xir_callable_signature(module->types,before.declaration->type.type);
        CHECK(signature && !signature->parameter_count && signature->result==XR_XIR_I64 &&
            xr_xir_cell_element(module->types,definition.stored_type)==before.declaration->type.type);
    } else CHECK(before.declaration->type.type==XR_XIR_I64);
    trial->after_publish=attempts;
    const char *second=history ? "const value=\"next\"\nexport fn answer()->i64{const item=Item{value:identity<i64>(old()+1)};return item.value}" :
        "const value=\"next\"\nexport fn answer()->string{return value}";
    if (program_storage==7) second="struct Padding{flag:bool}\nconst value=\"next\"\nfn append(slot:ref string){slot+=\"x\";slot+=\"${1}\"}\nexport fn answer()->i64{var object=fetch();append(ref object.value);const item=Item{value:identity<i64>(old()+1)};return item.value}";
    if (program_storage==8) second="struct Padding{flag:bool}\nconst value=\"next\"\nfn bump(slot:ref Leaf){slot.value+=1}\nexport fn answer()->i64{var object=fetch();bump(ref object.value);const item=Item{value:identity<i64>(old()+1)};return item.value+object.value.value}";
    if (program_storage>=9) second=source_history_function_input(2,program_storage==10);
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
    trial->after_abort=attempts;
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
static bool program_phase(const char *argument,ProgramTrial *trial,unsigned *phase) {
    static const char *const names[]={"--program-class-first","--program-class-abort","--program-class-publish",
        "--program-class-array-first","--program-class-array-abort","--program-class-array-publish",
        "--program-class-ref-first","--program-class-ref-abort","--program-class-ref-publish",
        "--program-class-inline-ref-first","--program-class-inline-ref-abort","--program-class-inline-ref-publish",
        "--program-function-call-first","--program-function-call-abort","--program-function-call-publish",
        "--program-function-value-first","--program-function-value-abort","--program-function-value-publish"};
    static const unsigned storage[]={4,5,7,8,9,10};
    for (unsigned i=0;i<18;++i) if (!strcmp(argument,names[i])) {
        trial->storage=storage[i/3];*phase=1+i%3;return true;
    }
    return false;
}
int main(int argc,char **argv) {
    bool comptime=false,baseline=false,history=false,program_binding=false,focused=false;
    ProgramTrial trial={0};unsigned phase=0;
    for (int i=1;i<argc;++i) {
        if (!strcmp(argv[i],"--comptime")) {CHECK(!comptime);comptime=true;}
        else if (!strcmp(argv[i],"--history")) {CHECK(!history);history=true;}
        else if (!strcmp(argv[i],"--program")) {CHECK(!program_binding);program_binding=true;history=true;}
        else if (!strcmp(argv[i],"--program-cell")) {CHECK(!program_binding);program_binding=true;trial.storage=1;history=true;}
        else if (!strcmp(argv[i],"--program-array")) {CHECK(!program_binding);program_binding=true;trial.storage=2;history=true;}
        else if (!strcmp(argv[i],"--program-array-const")) {CHECK(!program_binding);program_binding=true;trial.storage=3;history=true;}
        else if (!strcmp(argv[i],"--program-array-equal")) {CHECK(!program_binding);program_binding=true;trial.storage=6;history=true;}
        else if (program_phase(argv[i],&trial,&phase)) {CHECK(!program_binding);program_binding=true;history=true;}
        else if (!strcmp(argv[i],"--focused-fault")) {CHECK(!focused);focused=true;}
        else {CHECK(!strcmp(argv[i],"--baseline") && !baseline);baseline=true;}
    }
    XrCompileResourceStats normal={0},observed={0};
    CHECK(!comptime || !history);
    CHECK(!focused || (phase && !baseline));
    CHECK(pipeline(&limits,&normal,comptime,history,program_binding,&trial)==XR_XIR_OK);
    size_t total=attempts,first=0,last=total;
    if (phase) {
        CHECK(trial.after_publish && trial.after_publish<trial.after_abort && trial.after_abort<total);
        first=phase==1 ? 0 : phase==2 ? trial.after_publish : trial.after_abort;
        last=phase==1 ? trial.after_publish : phase==2 ? trial.after_abort : total;
        printf("SOURCE_GENERATION_PHASE storage=%u phase=%u total=%zu first=%zu last=%zu physical=0\n",
            trial.storage,phase,total,first,last);
    }
    size_t sites=last-first;
    printf("source generation sites=%zu allocated=%llu peak=%llu work=%llu\n",sites,
        (unsigned long long)normal.allocated_bytes,(unsigned long long)normal.peak_bytes,(unsigned long long)normal.work);
    fflush(stdout);
    CHECK(sites && sites<20000);
    if (baseline) return 0;
    for (size_t i=first;i<last;++i) {
        if (focused && i!=first && i!=last-1) continue;
        attempts=0;fail_at=i;XrXirStatus status=pipeline(&limits,&observed,comptime,history,program_binding,&trial);
        if (status!=XR_XIR_OUT_OF_MEMORY) fprintf(stderr,"site=%zu status=%u\n",i,(unsigned)status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY);
    }
    fail_at=SIZE_MAX;attempts=0;
    XrCompileResourceLimits exact={normal.allocated_bytes,normal.peak_bytes,normal.work};
    CHECK(pipeline(&exact,&observed,comptime,history,program_binding,&trial)==XR_XIR_OK);
    CHECK(observed.allocated_bytes==normal.allocated_bytes && observed.peak_bytes==normal.peak_bytes && observed.work==normal.work);
    for (unsigned axis=0;axis<3;++axis) {
        XrCompileResourceLimits short_limit=exact;
        if (!axis) --short_limit.allocated_bytes;
        else if (axis==1) --short_limit.live_bytes;
        else --short_limit.work;
        attempts=0;CHECK(pipeline(&short_limit,&observed,comptime,history,program_binding,&trial)==XR_XIR_BUDGET);
    }
    CHECK(!physical);
    if (focused) printf("Source generation phase boundary faults at %zu/%zu, exact/minus-one budgets and zero physical allocations passed\n",first,last-1);
    else puts("Source generation full physical FI, staged old-binding preservation, exact/minus-one budgets and zero physical allocations passed");
    return 0;
}
