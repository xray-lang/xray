/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_signal_source.c - Cold Source bindings and real isolated VM delivery
 */
#include "root_signal_runtime.h"
#include "xir_genuine_sys_path.h"
#include "xir/xxir_effects.c"
#include "xir/xxir_effects.h"
#include "xir/xxir_ctfe.h"
static uint32_t root_signal_named(const XrXirModule *module,const char *name) {
    for (uint32_t i=0;i<module->function_count;++i)
        if (module->functions[i].name_length==strlen(name) && !memcmp(module->functions[i].name,name,strlen(name))) return i;
    CHECK(false);return UINT32_MAX;
}
static XrXirStatus root_signal_check(const XrXirCompileContext *context,const char *input,bool genuine,
    XrXirSourceResult *source,XrXirSourceDiagnostic *diagnostic) {
    const char *root=XR_ROOT_SIGNAL_STDLIB;char path[2048];
    CHECK(snprintf(path,sizeof(path),"%s/sys/sys.xr",root)>0);
    XrModuleIdentityAuthority authority=genuine?(XrModuleIdentityAuthority){XR_MODULE_IDENTITY_STDLIB,"sys",root}:
        (XrModuleIdentityAuthority){XR_MODULE_IDENTITY_MEMORY,"untrusted",NULL};
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrXirSourceRequest request={session,genuine?path:NULL,&authority,context,root,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText text={genuine?"sys/sys.xr":NULL,input,strlen(input)};
    XrXirStatus status=genuine?xir_genuine_sys_path_check(&request,input,strlen(input),source,diagnostic):
        xr_xir_compile_source_check_text(&request,&text,source,diagnostic,NULL);
    xr_compile_session_free(session);return status;
}
static void root_signal_canonical_owner(const XrXirCompileContext *context,const XrXirArtifact *code,XrXirEffects *effects) {
    const XrXirModule *source=xr_xir_compile_artifact_module(code);
    (void)effects;
    EffectInputFacts *facts=NULL;EffectBodyMaterial *initial=NULL;EffectOrdinaryContexts *ordinary=NULL;
    CHECK(effect_input_facts_new(context,(XirEffectInput){source,code->construction,NULL},&facts)==XR_XIR_OK);
    CHECK(effect_body_material_generation(context,source,NULL,facts,&initial)==XR_XIR_OK);
    CHECK(effect_ordinary_build(context,source,initial,NULL,facts,&ordinary)==XR_XIR_OK);
    const XrXirCompileContext *saved=ordinary->terms.remaining;ordinary->terms.remaining=context;
    EffectOrdinaryView view={0};CHECK(effect_ordinary_view(ordinary,source,&view)==XR_XIR_OK);
    /* A real complete copy has no borrowed backbone or optional sharing prior. */
    view.input.initial=NULL;view.input.basis=NULL;view.input.record_source=NULL;
    EffectBodyMaterial *full=NULL;
    CHECK(effect_body_material_generation_input(context,&view.module,NULL,&view.input,ordinary->input_facts,&full)==XR_XIR_OK);
    ordinary->terms.remaining=saved;
    CHECK(full && !full->backbone && full->canonical_source && effect_body_material_shape(full));
    uint32_t selected=UINT32_MAX;
    for(uint32_t f=0;f<effect_body_count(full);++f)
        if(full->declarations->functions[f].native_callable==XR_NATIVE_CALLABLE_SYS_ON_SIGNAL){selected=f;break;}
    CHECK(selected!=UINT32_MAX);bool native=false;
    CHECK(effect_invocation_native_root(full,selected,&native)==XR_XIR_OK && native);
    EffectBodyMaterial invalid=*full;invalid.canonical_source=NULL;native=true;
    CHECK(effect_invocation_native_root(&invalid,selected,&native)==XR_XIR_BAD_STRUCTURE && !native);
    EffectBodyOrdinaryIdentity identities[256];CHECK(effect_body_count(full)<256);
    memcpy(identities,full->ordinary_identities,effect_body_count(full)*sizeof(*identities));
    invalid=*full;invalid.ordinary_identities=identities;
    identities[selected].declaration=source->declarations->entry_function;native=true;
    CHECK(effect_invocation_native_root(&invalid,selected,&native)==XR_XIR_BAD_STRUCTURE && !native);
    EffectBodyMaterial changed=*full->canonical_source;EffectBodyFunction heads[256];
    CHECK(effect_body_count(&changed)<256);memcpy(heads,changed.functions,effect_body_count(&changed)*sizeof(*heads));
    changed.functions=heads;heads[full->ordinary_identities[selected].declaration].parameter_count=1;
    invalid=*full;invalid.canonical_source=&changed;native=true;
    CHECK(effect_invocation_native_root(&invalid,selected,&native)==XR_XIR_BAD_TYPE && !native);
    effect_body_material_free(full);effect_ordinary_free(ordinary);effect_body_material_free(initial);effect_input_facts_free(facts);
}
static void root_signal_contracts(const XrXirCompileContext *context,const XrXirArtifact *code,const uint32_t ids[6]) {
    const XrXirModule *module=xr_xir_compile_artifact_module(code);uint32_t native=ids[4];
    CHECK(xir_native_callable_verify(module,native)==XR_XIR_OK);
    XrXirEffects *effects=NULL;CHECK(xr_xir_compile_effects_analyze(code,&effects)==XR_XIR_OK);
    XrXirEffectFacts facts=xr_xir_effects_read(effects,native);
    CHECK(facts.status==XR_XIR_OK && facts.root.requires_root && !facts.root.unresolved &&
        facts.control.suspend==XR_XIR_EFFECT_NONE && facts.root_witness.cause==XR_XIR_ROOT_CAUSE_NATIVE &&
        facts.root_witness.instruction==UINT32_MAX && facts.root_witness.slot==UINT32_MAX &&
        facts.root_witness.callee==UINT32_MAX && !facts.root_witness.distance);
    CHECK(!xr_xir_callable_root_accepts(facts.root.requires_root,facts.root.unresolved,XR_XIR_CALLABLE_ROOT_NONE));
    XrXirRootCauseTrace *trace=NULL;CHECK(xr_xir_compile_root_cause_trace_copy(context,effects,ids[3],&trace)==XR_XIR_OK);
    uint32_t count=0;const XrXirRootCauseStep *steps=xr_xir_root_cause_trace_steps(trace,false,&count);
    CHECK(steps && count && steps[count-1].function==native && steps[count-1].cause==XR_XIR_ROOT_CAUSE_NATIVE &&
        steps[count-1].instruction==UINT32_MAX && !steps[count-1].distance);
    xr_xir_compile_root_cause_trace_free(trace);
    xr_xir_compile_effects_free(effects);
    XrXirCtfeCallRequest request={.evaluation={.artifact=code,.function=root_signal_named(module,"ctfeRegister"),
        .target={XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES}},
        .call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
    memcpy(request.evaluation.checked_identity,code->checked_identity,32);XrXirValue constant={0};
    XrXirCtfeStatus evaluated=xr_xir_compile_ctfe_calls(&request,&constant);
    if(evaluated!=XR_XIR_CTFE_NOT_SUPPORTED)fprintf(stderr,"signal CTFE status%u value%u/%u/%lld\n",evaluated,constant.type,constant.reserved,(long long)constant.payload);
    CHECK(evaluated==XR_XIR_CTFE_NOT_SUPPORTED && !constant.type && !constant.reserved && !constant.payload);
    request.evaluation.function=root_signal_named(module,"ctfePlain");
    CHECK(xr_xir_compile_ctfe_calls(&request,&constant)==XR_XIR_CTFE_OK &&
        constant.type==XR_XIR_BOOL && !constant.reserved && constant.payload==1);
    xr_xir_value_drop(&constant);
    /* These local metadata views never become Source artifacts or Programs. */
    XrXirTypeNode nodes[256];CHECK(module->types->count<256);
    memcpy(nodes,module->types->nodes,module->types->count*sizeof(*nodes));
    XrXirTypes types=*module->types;types.nodes=nodes;XrXirModule changed=*module;changed.types=&types;
    XrXirType handler=module->functions[native].parameters[1];
    CHECK((uint32_t)handler>=XR_XIR_CONSTRUCTED_TYPE_BASE && (uint32_t)handler-XR_XIR_CONSTRUCTED_TYPE_BASE<types.count);
    uint32_t h=(uint32_t)(handler-XR_XIR_CONSTRUCTED_TYPE_BASE);XrXirTypeNode original=nodes[h];
    XrXirCallableParameter parameter={XR_XIR_I64,XR_PARAM_READ};
    for (unsigned mutation=0;mutation<8;++mutation) {
        nodes[h]=original;
        if(mutation==0)nodes[h].flags=XR_XIR_CALLABLE_ROOT_NONE;
        else if(mutation==1)nodes[h].flags|=XR_XIR_CALLABLE_NO_SUSPEND;
        else if(mutation==2)nodes[h].flags|=XR_XIR_CALLABLE_REST;
        else if(mutation==3)nodes[h].result=XR_XIR_I64;
        else if(mutation==4)nodes[h].element=XR_XIR_ERROR;
        else if(mutation==5){nodes[h].parameters=&parameter;nodes[h].parameter_count=1;}
        else if(mutation==6)nodes[h].parameter_span=1;
        else nodes[h].kind=XR_XIR_TYPE_ARRAY;
        CHECK(xir_native_callable_verify(&changed,native)==XR_XIR_BAD_TYPE);
    }
    nodes[h]=original;CHECK(xir_native_callable_verify(&changed,native)==XR_XIR_OK);
    static const char *const denied[]={
        "export fn bad(){const saved=__onSignal}\n",
        "export fn bad()->bool{return __onSignal(2,fn(){return 42})}\n",
        "fn handler(){}\nfn worker()->bool{return __onSignal(2,handler)}\nconst task=go worker()\n"
    };
    for (unsigned i=0;i<3;++i) {
        XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};
        CHECK(root_signal_check(context,denied[i],true,&output,&diagnostic)==XR_XIR_BAD_TYPE && !output.checked && !output.snapshot);
        if(i==0)CHECK(strstr(diagnostic.message,"signal registration requires a direct call"));
        if(i==2)CHECK(strstr(diagnostic.message,"GO target requires the current instance root execution"));
        xr_xir_compile_source_result_free(&output);
    }
    XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(root_signal_check(context,"export fn bad()->bool{return __onSignal(2,fn(){})}\n",false,&output,&diagnostic)!=XR_XIR_OK &&
        !output.checked && !output.snapshot);xr_xir_compile_source_result_free(&output);
    CHECK(root_signal_check(context,"fn __onSignal(signal:i64,handler:fn())->bool{return false}\nexport fn shadow()->bool{return __onSignal(2,fn(){})}\n",true,&output,&diagnostic)==XR_XIR_OK);
    const XrXirModule *shadow=xr_xir_compile_artifact_module(output.checked);
    CHECK(!shadow->declarations->functions[root_signal_named(shadow,"__onSignal")].native_callable);
    xr_xir_compile_source_result_free(&output);
}
static XrXirArtifact *root_signal_code(const XrXirCompileContext *context,uint32_t ids[6]) {
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=root_signal_check(context,root_signal_text,true,&source,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"signal Source%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK);
    root_signal_canonical_owner(context,source.checked,NULL);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);XrXirArtifact *cold=NULL,*closed=NULL,*code=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&cold,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    XrXirDiagnostic position={0};status=xr_xir_compile_specialize(cold,&closed,&position,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"signal Specialize%u f%u b%u i%u reason%u\n",status,position.function,position.block,position.instruction,position.reason);
    CHECK(status==XR_XIR_OK);xr_xir_compile_artifact_free(cold);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},NULL,&code,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    const char *names[]={"parent","guarded","owned","withHandler","__onSignal","read"};
    for (unsigned i=0;i<6;++i) ids[i]=root_signal_named(xr_xir_compile_artifact_module(code),names[i]);
    return code;
}
static void root_signal_emit(const XrXirArtifact *code,const uint32_t ids[6],const char *directory,bool write) {
    XrXirCSource c={0};CHECK(xr_xir_compile_emit_c(code,"root_signal",16777216,&c)==XR_XIR_OK);
    char file[2048],tail[256];CHECK(snprintf(file,sizeof(file),"%s/root_signal.c",directory)>0);
    int count=snprintf(tail,sizeof(tail),"\nconst uint32_t root_signal_ids[6]={%u,%u,%u,%u,%u,%u};\n",ids[0],ids[1],ids[2],ids[3],ids[4],ids[5]);
    CHECK(count>0 && (size_t)count<sizeof(tail));FILE *out=fopen(file,write?"wb":"rb");CHECK(out);
    if (write) CHECK(fwrite(c.text,1,c.length,out)==c.length && fwrite(tail,1,(size_t)count,out)==(size_t)count);
    else {for(size_t i=0;i<c.length;++i)CHECK(fgetc(out)==(unsigned char)c.text[i]);for(int i=0;i<count;++i)CHECK(fgetc(out)==(unsigned char)tail[i]);CHECK(fgetc(out)==EOF&&!ferror(out));}
    CHECK(!fclose(out));xr_xir_compile_c_source_free(&c);
}
int main(int argc,char **argv) {
    (void)sizeof(root_signal_text);
    bool emit=argc==3 && !strcmp(argv[1],"--emit");
    bool console=argc==2 && !strcmp(argv[1],"--private-console"),detached=argc==2 && !strcmp(argv[1],"--detached");
    CHECK(emit || console || detached || argc==1);
#if defined(XR_OS_WINDOWS)
    if (!emit && !console && !detached) {
        root_signal_isolate(L"--detached",DETACHED_PROCESS);root_signal_isolate(L"--private-console",CREATE_NEW_CONSOLE);
        puts("42");return 0;
    }
    if(console)CHECK(SetConsoleCtrlHandler(root_signal_previous,TRUE));
#endif
    XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;limits.profile_allocations=true;
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    uint32_t ids[6];XrXirArtifact *code=root_signal_code(&context,ids);root_signal_emit(code,ids,emit?argv[2]:XR_ROOT_SIGNAL_GENERATED,emit);
    if (emit) xr_xir_compile_artifact_free(code);
    else {
        root_signal_contracts(&context,code,ids);
        XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&code,&program)==XR_XIR_OK && !code);
        if(detached)root_signal_detached(program,ids);
        else {root_signal_cases(program,ids);root_signal_owner_cases(program,ids,&context);root_signal_stop_cases(program,ids);root_signal_admission_cases(program,ids);}
        xr_xir_compile_program_drop(program);
    }
    CHECK(!runtime_live && !runtime_bytes && xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && before.held_bytes==after.held_bytes);
    xr_compile_resources_release(context.resources);
#if defined(XR_OS_WINDOWS)
    if(console)CHECK(SetConsoleCtrlHandler(root_signal_previous,FALSE));
#endif
    return 0;
}
