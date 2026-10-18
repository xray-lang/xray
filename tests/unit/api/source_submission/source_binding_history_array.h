/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_binding_history_array.h - Actual Array owners across Source publication
 */
#ifndef SOURCE_BINDING_HISTORY_ARRAY_H
#define SOURCE_BINDING_HISTORY_ARRAY_H
static void reads_history_array_expect(const XrXirValue *array,XrXirDomain *domain) {
    XrXirValueAdmission admission={.arena=xr_xir_value_arena(array),.domain=domain,.work=10000,.scratch_bytes=4096};
    for (int64_t index=0;index<2;++index) {
        XrXirFaultDetail fault={0};XrXirValue value={0};
        CHECK(xr_xir_array_get(array,index,&admission,&value,&fault)==XR_XIR_VALUE_OK &&
            value.type==XR_XIR_I64 && value.payload==41+index);
        xr_xir_value_drop(&value);
    }
}
static void reads_history_array_receiving(void) {
    SubmissionOwners owned={0};CHECK(submission_open(&owned,&generous)==XR_SOURCE_OK);
    CHECK(reads_publish(&owned,"const values:Array<i64> = [41,42]\n"
        "export fn old()->Array<i64>{return values}\nprint(17)\n")==XR_SOURCE_OK);
    CHECK(reads_publish(&owned,"const values=\"shadow\"\nconst previous=old()\n"
        "const answer=previous[1]\n")==XR_SOURCE_OK);
    reads_scalar(&owned,"answer",XR_XIR_I64,42);
    XrXirValue first={0},last={0};
    CHECK(xr_source_submission_read(owned.owner,"previous",&first)==XR_SOURCE_OK);
    CHECK(submission_text(&owned,"const values=true\nconst candidate=old()\nassert(false)\n")==XR_SOURCE_OK);
    CHECK(submission_finish(&owned)==XR_XIR_CALL_ASSERTION);
    XrXirCallResult panic={0};CHECK(xr_source_submission_take(owned.owner,&panic)==XR_SOURCE_OK);
    CHECK(xr_source_submission_publish(owned.owner)==XR_SOURCE_BAD_STATE);
    XrXirCallStatus cleanup=XR_XIR_CALL_READY;
    CHECK(xr_source_submission_abort(owned.owner,&cleanup)==XR_SOURCE_OK);
    xr_xir_call_result_drop(&panic);
    XrXirSourceGenerationBinding binding={0};
    CHECK(xr_source_submission_lookup(owned.owner,"values",&binding)==XR_SOURCE_OK && binding.generation==2);
    CHECK(reads_publish(&owned,"const values:bool=true\nconst after=old()\nconst total=after[1]\n")==XR_SOURCE_OK);
    reads_scalar(&owned,"values",XR_XIR_BOOL,1);reads_scalar(&owned,"total",XR_XIR_I64,42);
    CHECK(xr_source_submission_lookup(owned.owner,"values",&binding)==XR_SOURCE_OK && binding.generation==4);
    CHECK(xr_source_submission_read(owned.owner,"after",&last)==XR_SOURCE_OK && owned.outputs==1);
    CHECK(xr_source_submission_drop(&owned.owner,&cleanup)==XR_SOURCE_OK && !owned.owner);
    xr_compile_state_release(owned.state);owned.state=NULL;
    xr_compile_resources_release(owned.resources);owned.resources=NULL;
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(4096,&domain)==XR_XIR_VALUE_OK);
    reads_history_array_expect(&first,domain);reads_history_array_expect(&last,domain);
    xr_xir_value_drop(&first);xr_xir_value_drop(&last);xr_xir_domain_drop(domain);
    submission_close(&owned);
    puts("Actual Source coordinator Array history through String/Bool shadows, failed candidate abort, output17 once, old41/42 after producer death; physical0");
}
#endif // SOURCE_BINDING_HISTORY_ARRAY_H
