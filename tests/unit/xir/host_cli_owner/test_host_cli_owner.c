/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_host_cli_owner.c - Exact borrowed rendering and physical release
 */
#include "execution/xr_xir_host_cli.h"
#include "xir/xxir_enum.h"
#include "xir/xxir_struct.h"
#include "xir/xxir_nominal.h"
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "../xir_runtime_allocations.h"
#include "base/xcompile_resources.c"
#include "xir/xxir_format.c"
#include "execution/xr_xir_host_execution.c"
#include "execution/xr_xir_host_cli.c"

typedef struct Capture { char bytes[16384]; size_t size, calls, fail_at; } Capture;
static int capture(void *context, const void *bytes, size_t size) {
    Capture *out = context;
    if (out->calls++ == out->fail_at) return 0;
    CHECK(out->size + size <= sizeof(out->bytes));
    if (size) memcpy(out->bytes + out->size, bytes, size);
    out->size += size;
    return 1;
}
static void expect(Capture *out, const void *bytes, size_t length) {
    CHECK(out->size == length && !memcmp(out->bytes, bytes, length));
}
static XrXirTypeArena *arena_new(XrCompileResources **resources) {
    XrCompileResourceLimits limits = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
    CHECK(xr_compile_resources_new(&limits, resources) == XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context = {*resources, xr_xir_compile_default_limits()};
    XrXirNominalVariant detail[] = {{{"Message",7},0,1},{{"Empty",5},1,0}};
    XrXirNominalVariant failed[] = {{{"Failed",6},0,4}};
    XrXirNominalVariant unsupported[] = {{{"Failed",6},0,1}};
    XrXirNominalFieldIdentity text[] = {{{"text",4},0}};
    XrXirNominalFieldIdentity outer[] = {{{"detail",6},0},{{"code",4},0},{{"ok",2},0},{{"real",4},0}};
    XrXirNominalFieldIdentity inner[] = {{{"item",4},0}};
    XrXirNominalIdentity identities[] = {
        {{"alpha",5},{"Detail",6},1,0,text,1,XR_XIR_NOMINAL_ENUM,detail,2,0},
        {{"alpha",5},{"Failure",7},1,0,outer,4,XR_XIR_NOMINAL_ENUM,failed,1,0},
        {{"alpha",5},{"Record",6},1,0,inner,1,XR_XIR_NOMINAL_STRUCT,NULL,0,0},
        {{"alpha",5},{"Unsupported",11},1,0,inner,1,XR_XIR_NOMINAL_ENUM,unsupported,1,0}};
    XrXirNominalTable table = {NULL,4,identities};
    XrXirType detail_types[] = {XR_XIR_STRING};
    XrXirType outer_types[] = {(XrXirType)256,XR_XIR_I64,XR_XIR_BOOL,XR_XIR_F64};
    XrXirType inner_types[] = {XR_XIR_I64}, unsupported_types[] = {(XrXirType)258};
    XrXirTypeNode nodes[4] = {{0}};
    for (uint32_t i = 0; i < 4; ++i) nodes[i].kind = XR_XIR_TYPE_NOMINAL;
    nodes[0].nominal = (XrXirNominalType){0,NULL,0,detail_types,1};
    nodes[1].nominal = (XrXirNominalType){1,NULL,0,outer_types,4};
    nodes[2].nominal = (XrXirNominalType){2,NULL,0,inner_types,1};
    nodes[3].nominal = (XrXirNominalType){3,NULL,0,unsupported_types,1};
    XrXirTypes types = {nodes,4,&table,NULL};
    XrXirTypeArena *arena = NULL;
    CHECK(xr_xir_compile_type_arena_new(&context, &types, &arena) == XR_XIR_VALUE_OK);
    return arena;
}

static void error_lifetime(void) {
    XrCompileResources *resources = NULL;
    XrXirTypeArena *arena = arena_new(&resources);
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(UINT64_MAX, &domain) == XR_XIR_VALUE_OK);
    XrXirValueAdmission admission = {arena,domain,NULL,NULL,UINT64_MAX,UINT64_MAX};
    char text[5003]; memset(text, 'x', sizeof(text)); text[13] = '\0';
    XrXirValue message = {0}, detail = {0}, empty = {0}, erased = {0};
    CHECK(xr_xir_string_new(domain, text, sizeof(text), &message) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_enum_new((XrXirType)256,0,&message,1,&admission,&detail) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_enum_new((XrXirType)256,1,NULL,0,&admission,&empty) == XR_XIR_VALUE_OK);
    XrXirValue fields[] = {detail,{XR_XIR_I64,0,-7},{XR_XIR_BOOL,0,1},{XR_XIR_F64,0,INT64_C(0x3ff8000000000000)}};
    XrXirCallResult result = {XR_XIR_CALL_THROWN,{0},0,{0}};
    CHECK(xr_xir_enum_new((XrXirType)257,0,fields,4,&admission,&result.value) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_error_erase(&result.value, &admission, &erased) == XR_XIR_VALUE_OK);
    XrXirEnumBorrow borrowed, before;
    memset(&borrowed, 0x5a, sizeof(borrowed)); before = borrowed;
    CHECK(xr_xir_enum_borrow(NULL, &borrowed) == XR_XIR_VALUE_BAD_ARGUMENT && !memcmp(&borrowed,&before,sizeof(before)));
    CHECK(xr_xir_enum_borrow(&fields[1], &borrowed) == XR_XIR_VALUE_BAD_ARGUMENT && !memcmp(&borrowed,&before,sizeof(before)));
    CHECK(xr_xir_enum_borrow(&erased, &borrowed) == XR_XIR_VALUE_BAD_ARGUMENT && !memcmp(&borrowed,&before,sizeof(before)));
    XirNominalValue forged = *(const XirNominalValue *)(intptr_t)empty.payload;
    XrXirValue fake = {256,0,(int64_t)(intptr_t)&forged};
    CHECK(xr_xir_enum_borrow(&fake,&borrowed) == XR_XIR_VALUE_BAD_ARGUMENT && !memcmp(&borrowed,&before,sizeof(before)));
    xr_xir_value_drop(&detail); xr_xir_value_drop(&message);
    xr_xir_compile_type_arena_drop(arena); xr_compile_resources_release(resources); xr_xir_domain_drop(domain);
    size_t attempts = runtime_attempts, live = runtime_live, bytes = runtime_bytes;
    CHECK(xr_xir_enum_borrow(&result.value, &borrowed) == XR_XIR_VALUE_OK && borrowed.field_count == 4);
    CHECK(borrowed.name.length == 7 && !memcmp(borrowed.name.bytes,"Failure",7));
    CHECK(borrowed.fields[1].payload == -7 && runtime_attempts == attempts);
    Capture output = {.fail_at = SIZE_MAX}; XrValueFormatSink sink = {&output,capture};
    CHECK(xr_xir_host_report_result(&result,sink,false) == XR_XIR_HOST_REPORT_OK);
    const char prefix[] = "[Uncaught Error] Failure.Failed(Detail.Message(\"";
    const char suffix[] = "\"), -7, true, 1.5)\n";
    CHECK(output.size == sizeof(prefix)-1 + sizeof(text) + sizeof(suffix)-1);
    CHECK(!memcmp(output.bytes,prefix,sizeof(prefix)-1));
    CHECK(!memcmp(output.bytes+sizeof(prefix)-1,text,sizeof(text)));
    CHECK(!memcmp(output.bytes+sizeof(prefix)-1+sizeof(text),suffix,sizeof(suffix)-1));
    size_t calls = output.calls;
    XrXirCallResult saved = result;
    for (size_t i=0;i<calls;++i) {
        output = (Capture){.fail_at=i};
        CHECK(xr_xir_host_report_result(&result,sink,false) == XR_XIR_HOST_REPORT_IO);
        CHECK(output.calls == i+1 && !memcmp(&result,&saved,sizeof(saved)));
    }
    result.value = erased;
    output = (Capture){.fail_at=SIZE_MAX};
    CHECK(xr_xir_host_report_result(&result,sink,true) == XR_XIR_HOST_REPORT_OK);
    CHECK(!memcmp(output.bytes,"\033[1;31m[Uncaught Error]\033[0m ",27));
    result.value = empty; output = (Capture){.fail_at=SIZE_MAX};
    CHECK(xr_xir_host_report_result(&result,sink,false) == XR_XIR_HOST_REPORT_OK);
    const char expected[]="[Uncaught Error] Detail.Empty\n";
    expect(&output,expected,sizeof(expected)-1);
    CHECK(runtime_attempts==attempts && runtime_live==live && runtime_bytes==bytes);
    result.value = saved.value;
    xr_xir_value_drop(&empty); xr_xir_value_drop(&erased); xr_xir_call_result_drop(&result);
    CHECK(!runtime_live && !runtime_bytes);
}

static void panic_lifetime(void) {
    const uint32_t codes[] = {420,421,422,430,442,444,445};
    const XrXirCallStatus statuses[] = {XR_XIR_CALL_DIVIDE_BY_ZERO,XR_XIR_CALL_DIVIDE_BY_ZERO,
        XR_XIR_CALL_NUMERIC_RANGE,XR_XIR_CALL_BOUNDS,XR_XIR_CALL_MATCH_FAILURE,
        XR_XIR_CALL_DEFER_ASYNC,XR_XIR_CALL_ASSERTION};
    const char *messages[] = {"division by zero","modulo by zero","numeric conversion is out of range",
        "array index out of range: 9 (length 3)","non-exhaustive match",
        "defer cleanup cannot suspend or create tasks"};
    for (size_t i=0;i<sizeof(codes)/sizeof(*codes);++i) {
        XrXirCallResult result = {statuses[i],{0},0,{{codes[i],0,0,0},{0}}};
        char long_message[5001]; memset(long_message,'m',sizeof(long_message));long_message[100]='\0';
        if (codes[i]==430) {result.panic.detail.index=9; result.panic.detail.length=3;}
        if (codes[i]==445) {
            XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
            CHECK(xr_xir_string_new(domain,long_message,sizeof(long_message),&result.panic.message)==XR_XIR_VALUE_OK);
            xr_xir_domain_drop(domain);
        }
        Capture output={.fail_at=SIZE_MAX}; size_t allocations=runtime_attempts;
        CHECK(xr_xir_host_report_result(&result,(XrValueFormatSink){&output,capture},false)==XR_XIR_HOST_REPORT_OK);
        char expected[6000]; int prefix=snprintf(expected,sizeof(expected),"[Uncaught Panic] E%04u: ",codes[i]);
        CHECK(prefix>0); size_t length=codes[i]==445 ? sizeof(long_message) : strlen(messages[i]);
        memcpy(expected+(size_t)prefix,codes[i]==445 ? long_message : messages[i],length);
        expected[(size_t)prefix+length]='\n';expect(&output,expected,(size_t)prefix+length+1);
        CHECK(runtime_attempts==allocations);
        xr_xir_call_result_drop(&result);CHECK(!runtime_live && !runtime_bytes);
    }
}

static void unsupported_and_invalid(void) {
    XrCompileResources *resources=NULL;XrXirTypeArena *arena=arena_new(&resources);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(UINT64_MAX,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={arena,domain,NULL,NULL,UINT64_MAX,UINT64_MAX};
    XrXirValue field={XR_XIR_I64,0,42},record={0};
    CHECK(xr_xir_struct_new((XrXirType)258,&field,1,&admission,&record)==XR_XIR_VALUE_OK);
    XrXirCallResult result={XR_XIR_CALL_THROWN,{0},0,{0}};
    CHECK(xr_xir_enum_new((XrXirType)259,0,&record,1,&admission,&result.value)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&record);xr_xir_compile_type_arena_drop(arena);
    xr_compile_resources_release(resources);xr_xir_domain_drop(domain);
    Capture output={.fail_at=SIZE_MAX};
    CHECK(xr_xir_call_result_valid(&result));
    CHECK(xr_xir_host_report_result(&result,(XrValueFormatSink){&output,capture},false)==XR_XIR_HOST_REPORT_BAD_ARGUMENT);
    CHECK(host_terminal(result.status,&result)==1);
    result.value.reserved=1;
    CHECK(host_terminal(result.status,&result)==4);
    result.value.reserved=0;xr_xir_call_result_drop(&result);
    CHECK(!runtime_live && !runtime_bytes);
    XrXirCallResult returned={XR_XIR_CALL_RETURNED,{XR_XIR_I64,0,0},0,{0}};
    CHECK(host_terminal(returned.status,&returned)==0);
    returned.value.payload=1;CHECK(host_terminal(returned.status,&returned)==4);
    returned.value=(XrXirValue){0};CHECK(host_terminal(returned.status,&returned)==4);
    const XrXirCallStatus failed[]={XR_XIR_CALL_LIMIT,XR_XIR_CALL_OUTPUT_ERROR,XR_XIR_CALL_OOM,XR_XIR_CALL_BAD_ABI};
    for (size_t i=0;i<sizeof(failed)/sizeof(*failed);++i) {
        returned=(XrXirCallResult){failed[i],{0},0,{0}};
        CHECK(host_terminal(failed[i],&returned)==(i<2 ? 1 : 4));
    }
}

int main(void) {
    error_lifetime();panic_lifetime();unsupported_and_invalid();
    CHECK(!runtime_live && !runtime_bytes);
    puts("Borrowed Error/Panic exact bytes, sink failures, typed status and physical zero PASS");
    return 0;
}
