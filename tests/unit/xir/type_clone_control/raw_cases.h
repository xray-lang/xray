/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * raw_cases.h - Verified metadata owns nested vectors independently of its request.
 */
#ifndef TYPE_CLONE_RAW_CASES_H
#define TYPE_CLONE_RAW_CASES_H
enum {TC_ARITY=1025,TC_NODE_COUNT=96};
typedef struct TcFixture {
    XrXirTypes types;XrXirTypeNode nodes[TC_NODE_COUNT];
    XrXirCallableParameter parameters[600];
    XrXirType arguments[TC_ARITY],fields[TC_ARITY],parent_arguments[TC_ARITY],method_arguments[TC_ARITY];
    XrXirConstraint nominal_constraints[TC_ARITY],base_constraints[TC_ARITY],child_constraint,method_constraint;
    XrXirNominalField nominal_fields[TC_ARITY];char names[TC_ARITY][8];
    XrXirNominalDeclaration declaration;XrXirNominalTable nominals;
    XrXirInterfaceApplication parent,method_application;XrXirInterfaceMethod method;
    XrXirInterfaceDeclaration declarations[2];XrXirInterfaceTable interfaces;
} TcFixture;
static TcFixture *raw_fixture(void) {
    TcFixture *f=xr_calloc(1,sizeof(*f));CHECK(f);
    for(unsigned i=0;i<600;++i)f->parameters[i]=(XrXirCallableParameter){XR_XIR_I64,XR_PARAM_READ};
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,
        .parameters=f->parameters,.parameter_count=600,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_NONE};
    for(unsigned i=0;i<TC_ARITY;++i) {
        f->arguments[i]=f->fields[i]=f->parent_arguments[i]=f->method_arguments[i]=XR_XIR_I64;
        int n=snprintf(f->names[i],sizeof(f->names[i]),"f%04u",i);CHECK(n==5);
        f->nominal_fields[i]=(XrXirNominalField){{f->names[i],5},XR_XIR_I64,0};
    }
    f->declaration=(XrXirNominalDeclaration){.module={"types",5},.name={"Record",6},.exported=1,
        .constraints=f->nominal_constraints,.parameter_count=TC_ARITY,.fields=f->nominal_fields,.field_count=TC_ARITY};
    f->nominals=(XrXirNominalTable){&f->declaration,1,NULL};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,f->arguments,TC_ARITY,f->fields,TC_ARITY}};
    for(unsigned i=2;i<TC_NODE_COUNT;++i)f->nodes[i]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,
        .element=i==2?XR_XIR_I64:(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i-1)};
    f->parent=(XrXirInterfaceApplication){0,f->parent_arguments,TC_ARITY};
    f->method_application=(XrXirInterfaceApplication){0,f->method_arguments,TC_ARITY};
    f->method_constraint=(XrXirConstraint){0,&f->method_application,1};
    f->method=(XrXirInterfaceMethod){{"read",4},(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE,0,1,&f->method_constraint};
    f->declarations[0]=(XrXirInterfaceDeclaration){.module={"types",5},.name={"Base",4},.exported=1,
        .constraints=f->base_constraints,.parameter_count=TC_ARITY};
    f->declarations[1]=(XrXirInterfaceDeclaration){.module={"types",5},.name={"Child",5},.exported=1,
        .constraints=&f->child_constraint,.parameter_count=1,.parents=&f->parent,.parent_count=1,.methods=&f->method,.method_count=1};
    f->interfaces=(XrXirInterfaceTable){f->declarations,2};
    f->types=(XrXirTypes){f->nodes,TC_NODE_COUNT,&f->nominals,&f->interfaces};return f;
}
static void raw_owned(const XrXirTypes *copy) {
    CHECK(copy&&copy->count==TC_NODE_COUNT&&copy->nodes[0].parameter_count==600);
    CHECK(copy->nodes[0].parameters[599].type==XR_XIR_I64);
    CHECK(copy->nodes[1].nominal.arguments[TC_ARITY-1]==XR_XIR_I64&&copy->nodes[1].nominal.fields[TC_ARITY-1]==XR_XIR_I64);
    CHECK(copy->nominals&&copy->nominals->count==1);
    if(copy->nominals->declarations) {
        const XrXirNominalDeclaration *d=&copy->nominals->declarations[0];CHECK(d->parameter_count==TC_ARITY&&d->field_count==TC_ARITY);
        CHECK(d->fields[TC_ARITY-1].name.length==5&&!memcmp(d->fields[TC_ARITY-1].name.bytes,"f1024",5));
        CHECK(copy->interfaces&&copy->interfaces->declarations[1].parents[0].arguments[TC_ARITY-1]==XR_XIR_I64);
        const XrXirInterfaceMethod *method=&copy->interfaces->declarations[1].methods[0];
        CHECK(method->own_parameter_count==1&&method->constraints[0].interfaces[0].arguments[TC_ARITY-1]==XR_XIR_I64);
    } else {CHECK(copy->nominals->identities[0].fields[TC_ARITY-1].name.length==5);}
}
static bool raw_cancel(const void *opaque) {return tc_cancel(opaque)!=0;}
static void raw_request(TcPhase phase,TcMode mode,unsigned target) {
    tc_memory_reset();TcFixture *f=raw_fixture();XrCompileResourceLimits limits={.profile_allocations=true};XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrXirStatus verified=xr_xir_compile_types_structure_verify(&context,&f->types);
    if(verified!=XR_XIR_OK)fprintf(stderr,"raw structural admission %u\n",(unsigned)verified);
    CHECK(verified==XR_XIR_OK);
    TcProbe p={.phase=phase,.mode=mode};
    if(target) {p.target=target==1?f->parent_arguments:f->method_arguments;p.bytes=sizeof(f->parent_arguments);}
    XrCompileRequestOptions policy={0};policy.cancel_poll=raw_cancel;policy.cancel_context=&p;
    if(mode==TC_DEADLINE){policy.deadline_mode=XR_COMPILE_DEADLINE_AFTER;policy.timeout_ns=UINT64_C(20000000);}
    XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&policy,&control)==XR_COMPILE_RESOURCE_OK);
    XrXirTypes sentinel={0},*copy=&sentinel;tc_probe_begin(&p);
    XrXirStatus status=xr_xir_compile_types_clone(&context,&f->types,&control,&copy);
    XrXirStatus expected=mode==TC_SUCCESS?XR_XIR_OK:mode==TC_CANCEL?XR_XIR_CANCELLED:mode==TC_DEADLINE?XR_XIR_DEADLINE:XR_XIR_OUT_OF_MEMORY;
    CHECK(status==expected&&p.completed&&p.calls==1);
    CHECK(mode==TC_OOM?(p.oom_seen&&p.attempts==1):p.witnesses==1);
    p.retired=true;tc_probe_end(&p);
    if(mode!=TC_SUCCESS) {
        CHECK(copy==&sentinel&&p.held_before==p.held_after);
        XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(resources,&stats)==XR_COMPILE_RESOURCE_OK&&stats.held_bytes==p.held_initial);
        /* A separate request reuses the same memory owner after failure. */
        XrCompileRequestOptions fresh_policy={0};XrCompileRequestControl fresh={0};
        CHECK(xr_compile_request_control_init(&fresh_policy,&fresh)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_xir_compile_types_clone(&context,&f->types,&fresh,&copy)==XR_XIR_OK);
    }
    CHECK(copy!=&sentinel&&copy->nodes!=f->nodes&&copy->nodes[0].parameters!=f->parameters);
    CHECK(copy->nodes[1].nominal.arguments!=f->arguments&&copy->nodes[1].nominal.fields!=f->fields);
    memset(f,0xa5,sizeof(*f));xr_free(f);size_t polls=p.polls;memset(&control,0xa5,sizeof(control));
    raw_owned(copy);CHECK(xr_xir_compile_types_structure_verify(&context,copy)==XR_XIR_OK&&p.polls==polls);
    xr_compile_resources_release(resources);xr_xir_compile_types_free(copy);release_probe(&p);
}
static void raw_identities(void) {
    tc_memory_reset();TcFixture *f=raw_fixture();XrCompileResourceLimits limits={0};XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    CHECK(xr_xir_compile_types_structure_verify(&context,&f->types)==XR_XIR_OK);
    XrXirNominalTable *projection=NULL;CHECK(xr_xir_compile_nominal_project(&context,&f->nominals,NULL,&projection)==XR_XIR_OK);
    XrXirTypes projected=f->types;projected.nominals=projection;projected.interfaces=NULL;
    CHECK(xr_xir_compile_types_structure_verify(&context,&projected)==XR_XIR_OK);
    XrCompileRequestOptions policy={0};XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&policy,&control)==XR_COMPILE_RESOURCE_OK);
    XrXirTypes *copy=NULL;CHECK(xr_xir_compile_types_clone(&context,&projected,&control,&copy)==XR_XIR_OK);
    CHECK(copy->nominals->identities&&copy->nominals->identities!=projection->identities);
    xr_xir_compile_nominal_free(projection);memset(f,0xa5,sizeof(*f));xr_free(f);memset(&control,0xa5,sizeof(control));
    raw_owned(copy);CHECK(xr_xir_compile_types_structure_verify(&context,copy)==XR_XIR_OK);
    xr_compile_resources_release(resources);xr_xir_compile_types_free(copy);CHECK(!tc_memory_live()&&!tc_memory_bytes());
}
static void raw_finite(void) {
    tc_memory_reset();TcFixture *f=raw_fixture();XrCompileResourceLimits limits={.profile_allocations=true};XrCompileResources *owner=NULL;
    CHECK(xr_compile_resources_new(&limits,&owner)==XR_COMPILE_RESOURCE_OK);XrXirCompileContext context={owner,xr_xir_compile_default_limits()};
    CHECK(xr_xir_compile_types_structure_verify(&context,&f->types)==XR_XIR_OK);
    XrXirTypes *copy=NULL;CHECK(xr_xir_compile_types_clone(&context,&f->types,NULL,&copy)==XR_XIR_OK);
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(owner,&stats)==XR_COMPILE_RESOURCE_OK);
    uint64_t exact=stats.peak_bytes;xr_xir_compile_types_free(copy);xr_compile_resources_release(owner);CHECK(!tc_memory_live());
    for(unsigned below=0;below<2;++below) {
        tc_memory_reset();owner=NULL;limits.memory_mode=XR_COMPILE_MEMORY_FINITE;limits.memory_limit=exact-below;
        CHECK(xr_compile_resources_new(&limits,&owner)==XR_COMPILE_RESOURCE_OK);context.resources=owner;
        uint64_t initial=tc_memory_initial(owner);XrXirTypes sentinel={0};copy=&sentinel;
        XrCompileRequestOptions policy={0};XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&policy,&control)==XR_COMPILE_RESOURCE_OK);
        XrXirStatus status=xr_xir_compile_types_clone(&context,&f->types,&control,&copy);
        CHECK(status==(below?XR_XIR_MEMORY_LIMIT:XR_XIR_OK));
        if(below) {
            CHECK(copy==&sentinel&&xr_compile_resources_stats(owner,&stats)==XR_COMPILE_RESOURCE_OK&&stats.held_bytes==initial);
            /* The memory refusal does not poison this request or owner. */
            CHECK(xr_compile_request_control_poll(&control)==XR_COMPILE_RESOURCE_OK);
            XrXirInterfaceApplication *app=NULL;
            CHECK(xr_xir_compile_interface_applications_copy_verified(&context,&f->parent,1,&control,&app)==XR_XIR_OK);
            CHECK(app->arguments[TC_ARITY-1]==XR_XIR_I64);xr_xir_compile_interface_applications_free(app,1);
        } else {raw_owned(copy);xr_xir_compile_types_free(copy);}
        CHECK(xr_compile_resources_stats(owner,&stats)==XR_COMPILE_RESOURCE_OK&&stats.held_bytes==initial);
        xr_compile_resources_release(owner);CHECK(!tc_memory_live()&&!tc_memory_bytes());
    }
    xr_free(f);
}
static void raw_cases(void) {
    static const TcPhase phases[]={TC_NODES_ZERO,TC_PARAMETERS,TC_ARGUMENTS,TC_FIELDS};
    for(unsigned i=0;i<sizeof(phases)/sizeof(*phases);++i)for(unsigned mode=0;mode<4;++mode)raw_request(phases[i],(TcMode)mode,0);
    for(unsigned target=1;target<=2;++target)for(unsigned mode=0;mode<4;++mode)raw_request(TC_NAME,(TcMode)mode,target);
    raw_identities();raw_finite();puts("Verified nested types, declarations, identities, parents and own constraints: interruption, rollback and retirement PASS");
}
#endif // TYPE_CLONE_RAW_CASES_H
