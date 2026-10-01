/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_unit_local_cases.h - Zero-size logical local declarations
 *
 * KEY CONCEPT:
 *   Unit captures retain declaration identity without a physical parameter or cell.
 */
static void source_unit_local_run(XrXirSourceRequest *request, const char *source) {
    write_source(request->entry_path,source);
    XrXirSourceResult result={0}; XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_source_check(request,&result,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"generic requirement: %u %d:%d %s\n",status,
        diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && result.checked && result.snapshot);
    const XrXirSourceView *view=xr_xir_source_snapshot_view(result.snapshot);
    const XrXirSourceDeclaration *owner=declaration(view,"Mapper",0);
    if (owner) {
    const XrXirSourceDeclaration *method=declaration(view,"map",owner->id);
    CHECK(method && method->generic_parent==owner->id && method->generic_parent_count==1 &&
        method->generic_parameter_count==2 && method->parameter_count==2);
    CHECK(method->type.generic_owner==method->id && method->parameters[0].generic_owner==method->id &&
        method->parameters[1].generic_owner==method->id);
    CHECK(method->type.type==(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+1));
    }
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_checked_write(result.checked,NULL,&packet,NULL)==XR_XIR_OK);
    xr_xir_artifact_free(result.checked); result.checked=NULL;
    write_source(request->entry_path,"const replaced=0\n");
    if (owner) CHECK(declaration(view,"map",owner->id)->generic_constraints[1].markers==XR_XIR_CONSTRAINT_SENDABLE);
    xr_xir_source_result_free(&result);
    XrXirArtifact *checked=NULL,*specialized=NULL,*lowered=NULL;
    CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&checked,NULL)==XR_XIR_OK);
    xr_xir_checked_packet_free(&packet);
    CHECK(xr_xir_specialize(checked,NULL,&specialized,NULL)==XR_XIR_OK);
    xr_xir_artifact_free(checked);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(specialized,&target,NULL,&lowered,NULL)==XR_XIR_OK);
    xr_xir_artifact_free(specialized);
    const XrXirModule *module=xr_xir_artifact_module(lowered);
    uint32_t entries[2]={UINT32_MAX,UINT32_MAX};
    const char *names[]={"genericMethodNumber","genericMethodText"};
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *function=&module->functions[f];
        for (uint32_t e=0;e<2;++e)
            if (function->name_length==strlen(names[e]) && !memcmp(function->name,names[e],strlen(names[e]))) entries[e]=f;
        for (uint32_t i=0;i<function->instruction_count;++i) CHECK(function->instructions[i].op!=XR_XIR_CALL_REQUIREMENT);
    }
    CHECK(entries[0]!=UINT32_MAX && entries[1]!=UINT32_MAX);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_vm_program_take(&lowered,(XrXirProgramBudget){33554432,64000000},&program)==XR_XIR_OK);
    XrXirValue retained[2]={{0},{0}};
    for (uint32_t run=0;run<2;++run) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY); XrXirInstance *instance=NULL;
        CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        for (uint32_t e=0;e<2;++e) {
            CHECK(xr_xir_instance_start(instance,entries[e],NULL,0)==XR_XIR_CALL_READY);
            XrXirInstanceResult step=xr_xir_instance_poll(instance);
            if (!e) {
                CHECK(step.outcome.status==XR_XIR_CALL_SUSPENDED);
                CHECK(xr_xir_instance_resume(instance,step.epoch,step.outcome.wake)==XR_XIR_CALL_READY);
                step=xr_xir_instance_poll(instance);
            }
            CHECK(step.outcome.status==XR_XIR_CALL_RETURNED);
            XrXirValue value={0};
            CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
            if (!e) { CHECK(value.type==XR_XIR_I64 && value.payload==41); xr_xir_value_drop(&value); }
            else retained[run]=value;
        }
        CHECK(xr_xir_instance_stop(instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    xr_xir_program_drop(program);
    for (uint32_t run=0;run<2;++run) {
        const char *bytes=NULL; size_t length=0;
        CHECK(retained[run].type==XR_XIR_STRING && xr_xir_string_view(&retained[run],&bytes,&length));
        CHECK(length==6 && !memcmp(bytes,"mapped",6));
        xr_xir_value_drop(&retained[run]);
    }
}
static void source_unit_local_cases(XrXirSourceRequest *request) {
    source_unit_local_run(request,
        "var trace=0\nfn mark(n:i64){trace=trace*10+n}\n"
        "fn work(){var empty:();const fixed:()=mark(1);var mutable=mark(2);"
        "const label=\"ok\";const capture=fn(){const before=empty;const also=fixed;mutable=mark(3);Coro.yield();"
        "return label==\"ok\" ? mutable : also};"
        "defer{empty=mark(4);const after=fixed};capture();return (empty)}\n"
        "export fn genericMethodNumber()->i64{work();return trace==1234 ? 41 : 0}\n"
        "export fn genericMethodText()->string{return \"mapped\"}\n");
    const char *bad[]={
        "fn bad(){const u:()}\n",
        "fn nop(){}\nfn bad(){const u=nop();u=nop()}\n",
        "fn bad(){var u:()=41}\n",
        "fn bad(){var u:();u=41}\n",
        "fn bad(){var u=u}\n",
        "fn nop(){}\nfn bad(){if(true){const u=nop()};return u}\n"
    };
    const XrXirStatus statuses[]={XR_XIR_BAD_STRUCTURE,XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE,
        XR_XIR_BAD_TYPE,XR_XIR_BAD_VALUE,XR_XIR_BAD_VALUE};
    const char *messages[]={
        "binding needs an initializer or explicit mutable type and no attributes",
        "assignment requires a mutable binding",
        "expression cannot satisfy its declared type",
        "expression cannot satisfy its declared type",
        "name is not an initialized value",
        "name is not an initialized value"
    };
    for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        write_source(request->entry_path,bad[i]);XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_source_check(request,&result,&diagnostic);
        char parse_message[1024];
        snprintf(parse_message,sizeof(parse_message),"failed to parse module: %s",request->entry_path);
        char actual_message[1024];
        snprintf(actual_message,sizeof(actual_message),"%s",diagnostic.message);
        for(char *c=parse_message;*c;++c) if(*c=='\\') *c='/';
        for(char *c=actual_message;*c;++c) if(*c=='\\') *c='/';
        const char *expected=i ? messages[i] : parse_message;
        if(status!=statuses[i] || strcmp(actual_message,expected))
            fprintf(stderr,"Unit local rejection %u: %u %s\n",i,status,diagnostic.message);
        CHECK(status==statuses[i] && !result.checked);
        CHECK(!strcmp(actual_message,expected));xr_xir_source_result_free(&result);
    }
}
