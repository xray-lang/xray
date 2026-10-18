/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_source_borrow_cases.h - Known callback aliases and independent value owners
 */
#ifndef P09_SOURCE_BORROW_CASES_H
#define P09_SOURCE_BORROW_CASES_H
static XrXirStatus borrow_source_check(const XrXirCompileContext *context,const char *text,
    XrXirSourceResult *result,XrXirSourceDiagnostic *diagnostic) {
    char directory[XR_TEST_PATH_MAX]="source-borrow-XXXXXX",absolute[XR_TEST_PATH_MAX],path[XR_TEST_PATH_MAX];
    CHECK(xr_test_mkdtemp(directory) && xr_test_realpath_buf(directory,absolute,sizeof(absolute)));
    CHECK(snprintf(path,sizeof(path),"%s/root.xr",absolute)>0);
    FILE *file=fopen(path,"wb");CHECK(file);
    size_t length=strlen(text);CHECK(fwrite(text,1,length,file)==length && !fclose(file));
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,absolute};
    XrXirSourceRequest request={session,path,&authority,context,XR_P09_HASH_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirStatus status=xr_xir_compile_source_check(&request,result,diagnostic,NULL);
    xr_compile_session_free(session);CHECK(!remove(path) && !xr_test_rmdir(directory));return status;
}
static void borrow_source_run(XrXirArtifact *lowered,int64_t expected) {
    uint32_t entry=borrow_entry(xr_xir_compile_artifact_module(lowered));XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    XrXirInstanceConfig config={0};XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    for(unsigned tick=0;;++tick) {
        CHECK(tick<100000);XrXirInstanceResult poll=xr_xir_instance_poll_bounded(instance,1);
        if(poll.outcome.status==XR_XIR_CALL_READY)continue;
        if(poll.outcome.status==XR_XIR_CALL_SUSPENDED) {
            CHECK(poll.epoch && poll.outcome.wake);
            CHECK(xr_xir_instance_resume(instance,poll.epoch,poll.outcome.wake)==XR_XIR_CALL_READY);continue;
        }
        CHECK(poll.outcome.status==XR_XIR_CALL_RETURNED);break;
    }
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(value.type==XR_XIR_I64 && value.payload==expected);xr_xir_value_drop(&value);
}
static void source_borrow_cases(void) {
    const char *prefix="fn refVisit(xs:ref Array<i64>,visit:fn()){visit()}\n";
    const char *negative[]={
        "export fn entry(){var xs=[2,1]\nrefVisit(ref xs,fn(){xs[0]=9})}",
        "export fn entry(){var xs=[2,1]\nrefVisit(ref xs,fn(){const read=xs[0]})}",
        "export fn entry(){var xs=[2,1]\nconst f=fn(){xs[0]=9}\nconst g:fn()=f\nrefVisit(ref xs,g)}",
        "export fn entry(){var xs=[2,1]\nconst call=refVisit\ncall(ref xs,fn(){xs[0]=9})}",
        "fn reversed(visit:fn(),xs:ref Array<i64>){visit()}\nexport fn entry(){var xs=[2,1]\nreversed(fn(){xs[0]=9},ref xs)}",
        "fn bad(xs:ref Array<i64>){refVisit(ref xs,fn(){xs[0]=9})}\nexport fn entry(){}",
        "fn generic<T>(xs:ref Array<T>,visit:fn()){visit()}\nexport fn entry(){var xs=[2,1]\ngeneric(ref xs,fn(){xs[0]=9})}",
        "export fn entry(){var xs=[2,1]\ntry{refVisit(ref xs,fn(){xs[0]=9})}catch panic(p){}}",
        "export fn entry(){var xs=[2,1]\nfn write(){xs[0]=9}\nrefVisit(ref xs,write)}",
        "export fn entry(){var xs=[2,1]\nfn write(target:ref Array<i64>){xs[0]=9}\nwrite(ref xs)}",
        "fn visit(n:ref i64,f:fn()){f()}\nexport fn entry(){var n=2\nvisit(ref n,fn(){n+=1})}"};
    const char *positive[]={
        "export fn entry()->i64{var xs=[2,1]\nvar ys=[4,3]\nrefVisit(ref xs,fn(){ys[0]=9})\nreturn xs[0]*10+ys[0]}",
        "export fn entry()->i64{var xs=[2,1]\nconst snapshot=xs\nvar seen=0\nrefVisit(ref xs,fn(){seen=snapshot[0]})\nreturn xs[0]*10+seen}",
        "export fn entry()->i64{var xs=[2,1]\nvar seen=0\nrefVisit(ref xs,fn(){Coro.yield()\nseen+=1})\nreturn xs[0]*10+seen}",
        "fn edit(xs:ref Array<i64>){xs[0]=7}\nfn outer(xs:ref Array<i64>){edit(ref xs)}\nexport fn entry()->i64{var xs=[2,1]\nouter(ref xs)\nreturn xs[0]}",
        "export fn entry()->i64{var xs=[2,1]\nvar ys=xs\nrefVisit(ref xs,fn(){ys[0]=9})\nreturn xs[0]*10+ys[0]}",
        "fn outer(xs:ref Array<i64>){refVisit(ref xs,fn(){})}\nexport fn entry()->i64{var xs=[2,1]\nouter(ref xs)\nreturn xs[0]}"};
    const int64_t expected[]={29,22,21,7,29,2};
    for(unsigned role=0;role<2;++role) {
        unsigned count=role?sizeof(positive)/sizeof(positive[0]):sizeof(negative)/sizeof(negative[0]);
        for(unsigned i=0;i<count;++i) {
            char text[4096];CHECK(snprintf(text,sizeof(text),"%s%s",prefix,role?positive[i]:negative[i])>0);
            SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
            XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
            XrXirStatus status=borrow_source_check(&owner.context,text,&result,&diagnostic);
            printf("borrow role=%s case=%u status=%u at=%d:%d diagnostic=%s\n",
                role?"independent":(i==9?"existing-mode-reject":"conflict"),i,status,diagnostic.line,diagnostic.column,diagnostic.message);fflush(stdout);
            if(role) {
                CHECK(status==XR_XIR_OK && result.checked && result.snapshot);
                borrow_source_run(hash_source_replay(&owner.context,&result),expected[i]);
            } else CHECK(status==XR_XIR_BAD_TYPE && !result.checked && !result.snapshot &&
                diagnostic.line>0 && strstr(diagnostic.message,i==9 ?
                    "closure parameter requires an annotation or complete callable context" :
                    "ref argument conflicts with a callable capture"));
            source_fixture_owner_free(&owner);
        }
    }
    puts("Known callback/ref root conflicts reject in Source; independent value owners and reborrows PASS");
}
#endif // P09_SOURCE_BORROW_CASES_H
