/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_mcp_run_runtime.c - Physical release through the production MCP runner
 *
 * KEY CONCEPT:
 *   Compilation precedes runtime fault injection. Every actual runtime
 *   allocation ordinal is exercised and all observed blocks die before JSON
 *   observation, including child timer cancellation and panic outcomes.
 */
#include "app/mcp/xmcp_tools_internal.h"
#include "app/mcp/xmcp_server.h"
#include "base/xmalloc.h"
#include "base/xhash.h"
#include "xir/xxir_program.h"
#include "os/os_time.h"
#include "../test_win_compat.h"
#include <stdio.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
typedef struct RuntimeBlock { void *pointer; size_t bytes; } RuntimeBlock;
static RuntimeBlock blocks[8192];
static size_t live, physical_bytes, attempts, selected=SIZE_MAX, fail_at=SIZE_MAX;
static bool injected, entered, expire_before_owner;
static size_t channel_requests, channel_pending_attempts, host_resumes;
static bool observe_channel_pending;
static size_t slot_of(const void *pointer) { return xr_hash_bytes(&pointer,sizeof(pointer)) & 8191; }
static void record(void *pointer,size_t bytes) {
    if (!pointer) return;
    CHECK(live < 4096 && bytes <= SIZE_MAX-physical_bytes);
    size_t slot=slot_of(pointer);
    while (blocks[slot].pointer) slot=(slot+1)&8191;
    blocks[slot]=(RuntimeBlock){pointer,bytes}; ++live; physical_bytes+=bytes;
}
static bool forget(void *pointer) {
    if (!pointer) return false;
    size_t hole=slot_of(pointer);
    while (blocks[hole].pointer != pointer) {
        if (!blocks[hole].pointer) return false;
        hole=(hole+1)&8191;
    }
    CHECK(live && physical_bytes>=blocks[hole].bytes);
    --live; physical_bytes-=blocks[hole].bytes;
    for (size_t next=(hole+1)&8191;blocks[next].pointer;next=(next+1)&8191) {
        size_t home=slot_of(blocks[next].pointer);
        bool stays=hole<=next ? hole<home && home<=next : hole<home || home<=next;
        if (!stays) { blocks[hole]=blocks[next]; hole=next; }
    }
    blocks[hole]=(RuntimeBlock){0}; return true;
}
static bool fail(void) {
    if (attempts++==fail_at) { injected=true; return true; }
    return false;
}
XR_FUNC void *mcp_runtime_malloc(size_t bytes) {
    if (fail()) return NULL;
    void *pointer=xr_malloc(bytes); record(pointer,bytes); return pointer;
}
XR_FUNC void *mcp_runtime_calloc(size_t count,size_t bytes) {
    if (fail()) return NULL;
    CHECK(!bytes || count<=SIZE_MAX/bytes);
    void *pointer=xr_calloc(count,bytes); record(pointer,count*bytes); return pointer;
}
XR_FUNC void *mcp_runtime_realloc(void *pointer,size_t bytes) {
    if (fail()) return NULL;
    void *replacement=xr_realloc(pointer,bytes);
    if (replacement) { (void)forget(pointer); record(replacement,bytes); }
    return replacement;
}
XR_FUNC void mcp_runtime_free(void *pointer) { (void)forget(pointer); xr_free(pointer); }
XR_FUNC XrXirCallStatus mcp_runtime_instance_new(XrXirProgram *program,
    const XrXirInstanceConfig *config,XrXirInstance **output) {
    CHECK(!entered); entered=true;
    CHECK(config->metadata_limit == (UINT64_C(16) << 20) &&
        config->value_limit == (UINT64_C(16) << 20) && config->call_limit == (UINT64_C(16) << 20));
    CHECK(config->requested_value_limit == (UINT64_C(64) << 20) &&
        config->requested_call_limit == (UINT64_C(64) << 20));
    CHECK(config->work_limit == UINT64_C(128000000) &&
        config->poll_limit == UINT64_C(1000000) && config->depth_limit == 4096);
    if (expire_before_owner) {
        /* Real elapsed time fixes the pre-poll cancellation branch; no timer
         * is declared due and the request retains its original 50ms budget. */
        uint64_t before=xr_time_monotonic_ns(); CHECK(before);
        xr_time_sleep_ms(50);
        uint64_t after=xr_time_monotonic_ns();
        CHECK(after>=before && after-before>=UINT64_C(50000000));
    }
    attempts=0; fail_at=selected;
    return xr_xir_instance_new(program,config,output);
}
XR_FUNC XrXirCallStatus mcp_runtime_instance_wait_request(XrXirInstance *instance,
    uint64_t epoch,uint64_t wake,XrXirWaitRequest *output) {
    XrXirCallStatus status=xr_xir_instance_wait_request(instance,epoch,wake,output);
    if (status==XR_XIR_CALL_READY && output->kind==XR_XIR_WAIT_CHANNEL) {
        if (observe_channel_pending) {
            if (!channel_requests) channel_pending_attempts=attempts;
            CHECK(attempts==channel_pending_attempts);
        }
        ++channel_requests;
    }
    return status;
}
XR_FUNC XrXirCallStatus mcp_runtime_instance_resume(XrXirInstance *instance,
    uint64_t epoch,uint64_t wake) {
    ++host_resumes;return xr_xir_instance_resume(instance,epoch,wake);
}
static void physical_zero(const char *name,size_t ordinal) {
    if (live || physical_bytes) fprintf(stderr,"%s ordinal=%zu residual blocks=%zu bytes=%zu\n",
        name,ordinal,live,physical_bytes);
    CHECK(!live && !physical_bytes);
}
typedef struct RunCase { const char *name,*code,*output; int timeout; XrXirCallStatus status; bool timed_out; } RunCase;
static void exercise(const RunCase *test,size_t ordinal) {
    physical_zero(test->name,ordinal); selected=ordinal; fail_at=SIZE_MAX;
    injected=false; entered=false; expire_before_owner=!strcmp(test->name,"pre-poll-cancel");
    channel_requests=channel_pending_attempts=host_resumes=0;
    observe_channel_pending=!strcmp(test->name,"channel-park-cancel");
    XmcpServer server={0}; XmcpCallContext context={0};
    XrJsonValue *arguments=xjson_new_object(); CHECK(arguments);
    XJSON_SET_STRING(arguments,"code",test->code);
    XJSON_SET_STRING(arguments,"moduleId","mcp-run-runtime");
    if (test->timeout) XJSON_SET_INT(arguments,"timeoutMs",test->timeout);
    XrJsonValue *result=xmcp_tool_xray_run(&server,&context,arguments);
    xjson_free(arguments); CHECK(result && entered); physical_zero(test->name,ordinal);
    XrJsonValue *value=xjson_get_object(result,"structuredContent"); CHECK(value);
    CHECK(xjson_get_int_or(value,"sourceStatus",-1)==XR_XIR_OK);
    XrXirCallStatus status=(XrXirCallStatus)xjson_get_int_or(value,"runtimeStatus",-1);
    if (ordinal==SIZE_MAX) {
        CHECK(!injected && status==test->status);
        CHECK(xjson_get_bool(value,"timedOut")==test->timed_out);
        CHECK(xjson_get_bool(value,"ok")== (status==XR_XIR_CALL_RETURNED));
        if (observe_channel_pending) CHECK(channel_requests>1 && !host_resumes);
        XrJsonValue *output=xjson_get(value,"stdout"); CHECK(xjson_is_string(output));
        CHECK(output->string_len==strlen(test->output) && !memcmp(output->as.string,test->output,output->string_len));
        if (test->status==XR_XIR_CALL_ASSERTION)
            CHECK(strstr(xjson_get_string(value,"stderr"),"owned-panic"));
    } else {
        if (!injected || status!=XR_XIR_CALL_OOM) fprintf(stderr,"%s ordinal=%zu injected=%d status=%d attempts=%zu\n",
            test->name,ordinal,injected,status,attempts);
        CHECK(injected && xjson_get_bool(result,"isError") && status==XR_XIR_CALL_OOM);
        CHECK(xjson_get_int_or(value,"outputBytes",-1)==0);
    }
    xjson_free(result); fail_at=SIZE_MAX; physical_zero(test->name,ordinal);
}
#include "mcp_timed_channel_cases.h"
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--normal")));
    bool normal_only=argc==2; xr_test_suppress_dialogs();
    const RunCase cases[]={
        {"return","print(42)\n","42\n",0,XR_XIR_CALL_RETURNED,false},
        {"task-timer","import time\nfn worker()->i64 { time.sleep(5); return 42 }\nfn main()->i64 { return await go worker() }\nprint(main())\n","42\n",0,XR_XIR_CALL_RETURNED,false},
        {"pre-poll-cancel","import time\nfn worker()->i64 { time.sleep(2000); return 42 }\nfn main()->i64 { return await go worker() }\nprint(main())\n","",50,XR_XIR_CALL_CANCELLED,true},
        {"task-default-cancel","import time\nfn worker()->i64 { time.sleep(5000); return 42 }\nfn main()->i64 { return await go worker() }\nprint(main())\n","",0,XR_XIR_CALL_CANCELLED,true},
        {"panic","assert(false, \"owned-panic\")\n","",0,XR_XIR_CALL_ASSERTION,false},
        {"channel-buffered","const ch=Channel<i64>(1)\nch.send(42)\nprint(ch.recvOr(0))\n","42\n",0,XR_XIR_CALL_RETURNED,false},
        {"channel-rendezvous","fn send(ch:Channel<i64>)->() { ch.send(42) }\n"
            "fn main()->i64 { const ch=Channel<i64>(); const task=go send(ch); const n=ch.recvOr(0); await task; return n }\n"
            "print(main())\n","42\n",0,XR_XIR_CALL_RETURNED,false},
        {"channel-task-error","fn send(ch:Channel<i64>)->i64 { ch.send(42); return -1 }\n"
            "fn main()->i64 { const ch=Channel<i64>(); ch.close(); const task=go send(ch); "
            "try { return await task } catch(e:SendResult) { return match(e) { SendResult.Sent -> -1, "
            "SendResult.Full -> -2, SendResult.Timeout -> -3, SendResult.Closed -> 64 } } }\n"
            "print(main())\n","64\n",0,XR_XIR_CALL_RETURNED,false},
        {"channel-park-cancel","const ch=Channel<i64>()\nvar value=ch.recv()\n","",1000,XR_XIR_CALL_CANCELLED,true},
        {"channel-timed",mcp_timed_channel_code,"14177\n",0,XR_XIR_CALL_RETURNED,false}
    };
    for (size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        exercise(&cases[i],SIZE_MAX); size_t sites=attempts; CHECK(sites);
        if (normal_only) printf("MCP runtime %s: %zu normal allocations; physical0 before JSON observation\n",cases[i].name,sites);
        else {
            for (size_t ordinal=0;ordinal<sites;++ordinal) exercise(&cases[i],ordinal);
            printf("MCP runtime %s: %zu complete allocation faults; physical0 before JSON observation\n",cases[i].name,sites);
        }
    }
    exercise(&cases[0],SIZE_MAX);
    puts("MCP runtime normal/cancel/panic ownership and same-process recovery PASS"); return 0;
}
