/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_sdk_consumer.c - Installed public API ownership and Checked consumers
 */
#include "xray_source.h"
#include "xray_source_entry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
_Static_assert(sizeof(XrSourceText) == 32, "text ABI");
_Static_assert(sizeof(XrSourceDiagnosticReportInfo) == 8, "diagnostic report ABI");
_Static_assert(sizeof(XrSourceEvent) == 24, "event ABI");
_Static_assert(sizeof(XrSourceCallOptions) == 80, "call options ABI");
_Static_assert(sizeof(XrSourceWait) == 40, "wait ABI");
_Static_assert(offsetof(XrSourceWait, subject) == 16, "wait subject ABI");
_Static_assert(offsetof(XrSourceWait, generation) == 24, "wait generation ABI");
_Static_assert(offsetof(XrSourceWait, ticket) == 32, "wait ticket ABI");
_Static_assert(offsetof(XrSourceCallOptions, write) == 64, "callback ABI");
typedef struct Output { char bytes[256]; size_t length; } Output;
static XrSourceWriteStatus capture(void *context, XrSourceOutputStream stream, const char *bytes, size_t length) {
    Output *out = context;
    CHECK(stream == XR_SOURCE_STDOUT && length <= sizeof(out->bytes) - out->length);
    memcpy(out->bytes + out->length, bytes, length); out->length += length; return XR_SOURCE_WRITE_OK;
}
static XrSourceCompileOptions compile_options(const char *stdlib) {
    XrSourceCompileOptions options=xray_source_compile_options_default();
    CHECK(options.target==XR_SOURCE_TARGET_VM && options.memory_mode==XR_SOURCE_MEMORY_ORDINARY);
    options.stdlib_path=stdlib;return options;
}
static XrSourceStatus execute_owned(XrSourceArtifact *artifact,const XrSourceCallOptions *options,
    XrSourceResult **result,XrSourceEvent *failure) {
    XrSourceExecutionOptions policy=xray_source_execution_options_default();
    XrSourceExecution *execution=NULL;
    XrSourceStatus status=xray_source_execution_new(&policy,&execution);
    if(status==XR_SOURCE_OK)status=xray_source_artifact_execute(execution,artifact,options,result,failure);
    xray_source_execution_free(execution);return status;
}
static XrSourceStatus begin_owned(XrSourceArtifact *artifact,const XrSourceCallOptions *options,
    XrSourceCall **call) {
    XrSourceExecutionOptions policy=xray_source_execution_options_default();
    XrSourceExecution *execution=NULL;
    XrSourceStatus status=xray_source_execution_new(&policy,&execution);
    if(status==XR_SOURCE_OK)status=xray_source_artifact_begin(execution,artifact,options,call);
    xray_source_execution_free(execution);return status;
}
static XrSourceArtifact *compile(const XrSourceCompileOptions *compiler, char *bytes, const char *case_name) {
    XrSourceText text = {NULL, "installed-embedding", bytes, strlen(bytes)};
    XrSourceArtifact *program = NULL; XrSourceDiagnostic *diagnostic = NULL;
    XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,.value.text=text};
    XrSourceFailure failure={0};
    XrSourceStatus status = xray_source_build(&input, compiler, &program, &failure, &diagnostic);
    if (status != XR_SOURCE_OK || !program || diagnostic)
        fprintf(stderr, "[sdk-compile-failure] case=%s status=%u\n", case_name, (unsigned)status);
    if (diagnostic) { XrSourceDiagnosticRoot info={0};XrSourceDiagnosticReportInfo report_info={0}; CHECK((xray_source_diagnostic_report(diagnostic,&report_info)==XR_SOURCE_OK && report_info.roots==1 ? xray_source_diagnostic_root(diagnostic,0,&info) : XR_SOURCE_BAD_STATE) == XR_SOURCE_OK); fprintf(stderr, "%s\n", info.message); }
    CHECK(status == XR_SOURCE_OK && program && !diagnostic); memset(bytes, '!', strlen(bytes)); return program;
}
static void checked_program(const char *stdlib) {
    XrSourceCompileOptions compiler = compile_options(stdlib);
    char source[] = "import time as Clock\nimport \"std/io/output\" as Output\n"
        "Output.writeStdout(\"A\\0B\")\nconst begin=Clock.nanos()\nClock.sleep(1)\n"
        "print(Clock.nanos()-begin>=1000000)\n";
    XrSourceArtifact *program = compile(&compiler, source, __func__);
    Output out = {0}; XrSourceCallOptions options;
    CHECK(xray_source_call_options_init(&options, sizeof(options)) == XR_SOURCE_OK);
    options.write = capture; options.context = &out;
    XrSourceResult *result = NULL; XrSourceEvent failure = {0};
    CHECK(execute_owned(program, &options, &result, &failure) == XR_SOURCE_OK && result);
    xray_source_artifact_free(program); XrSourceEvent event;
    CHECK(xray_source_result_event(result, &event) == XR_SOURCE_OK && event.state == XR_SOURCE_RUN_RETURNED);
    int64_t value = -1; CHECK(xray_source_result_i64(result, &value) == XR_SOURCE_OK && value == 0);
    CHECK(out.length == 8 && !memcmp(out.bytes, "A\0Btrue\n", 8)); xray_source_result_free(result);
}
static void detached_cancellation(const char *stdlib) {
    XrSourceCompileOptions compiler = compile_options(stdlib);
    char source[] = "var n:i64=0\nwhile(true){ n+=1 }\n";
    XrSourceArtifact *program = compile(&compiler, source, __func__);
    XrSourceCallOptions options; CHECK(xray_source_call_options_init(&options, sizeof(options)) == XR_SOURCE_OK);
    XrSourceCall *call = NULL; CHECK(begin_owned(program, &options, &call) == XR_SOURCE_OK);
    xray_source_artifact_free(program); XrSourceEvent event = {0};
    CHECK(xray_source_call_poll(call, 1, &event) == XR_SOURCE_OK && event.state == XR_SOURCE_RUN_READY);
    CHECK(xray_source_call_cancel(call) == XR_SOURCE_OK);
    for (size_t steps = 0; event.state == XR_SOURCE_RUN_READY; ++steps) {
        CHECK(steps < 4096 && xray_source_call_poll(call, 8, &event) == XR_SOURCE_OK);
    }
    CHECK(event.state == XR_SOURCE_RUN_CANCELLED); XrSourceResult *result = NULL;
    CHECK(xray_source_call_take(call, &result) == XR_SOURCE_OK && result);
    XrSourceResult *duplicate = NULL;
    CHECK(xray_source_call_take(call, &duplicate) == XR_SOURCE_BAD_STATE && !duplicate);
    CHECK(xray_source_call_drop(&call, &event) == XR_SOURCE_OK && !call);
    CHECK(xray_source_result_event(result, &event) == XR_SOURCE_OK && event.state == XR_SOURCE_RUN_CANCELLED);
    xray_source_result_free(result);
}
static void owned_panic(const char *stdlib) {
    XrSourceCompileOptions compiler = compile_options(stdlib); char source[] = "assert(false,\"p\\0中z\")\n";
    XrSourceArtifact *program = compile(&compiler, source, __func__);
    XrSourceCallOptions options; CHECK(xray_source_call_options_init(&options, sizeof(options)) == XR_SOURCE_OK);
    XrSourceResult *result = NULL; CHECK(execute_owned(program, &options, &result, NULL) == XR_SOURCE_OK);
    xray_source_artifact_free(program); XrSourceEvent event;
    CHECK(xray_source_result_event(result, &event) == XR_SOURCE_OK && event.state == XR_SOURCE_RUN_PANICKED);
    const char *bytes = NULL; size_t length = 0;
    CHECK(xray_source_result_bytes(result, &bytes, &length) == XR_SOURCE_OK);
    CHECK(length == 6 && !memcmp(bytes, "p\0\xe4\xb8\xad" "z", 6)); xray_source_result_free(result);
}
static void library_methods(const char *stdlib) {
    char source[] =
        "var library_integer:i64=-42\nprint(library_integer.abs(),library_integer.toString"
        "(),library_integer.toHex(),library_integer.max(7),library_integer.min(7),42.toF6"
        "4())\nvar library_max:i64=9223372036854775807\nprint(library_max.checkedAdd(1)==nu"
        "ll,library_max.saturatingAdd(1),library_max.wrappingAdd(1),library_max.addOverfl"
        "ows(1))\nvar library_bits:i64=1\nprint(library_bits.popcount(),library_bits.leadin"
        "gZeros(),library_bits.trailingZeros(),library_bits.byteswap(),library_bits.rotat"
        "eLeft(65),library_bits.rotateRight(-1))\nvar library_float:f64=-2.5\nprint(library"
        "_float.abs(),library_float.toI64(),library_float.floor(),library_float.ceil(),li"
        "brary_float.round(),library_float.isNaN(),library_float.toString(),true.toString"
        "(),false.toString(),rune(65).toUInt32(),rune(65).toString())\nvar library_text:st"
        "ring=\"A\\0B\"\nvar library_repeated:string=library_text.repeat(2)\nprint(library_tex"
        "t.toString(),library_repeated,library_repeated.contains(\"\\0B\"),library_repeated."
        "startsWith(\"A\"),library_repeated.endsWith(\"B\"),library_repeated.indexOf(\"B\"),lib"
        "rary_repeated.lastIndexOf(\"B\"))\n";
    const char expected[] =
        "42 -42 -0x2a 7 -42 42.0\ntrue 9223372036854775807 -9223372036854775808 true\n1 63 "
        "0 72057594037927936 2 2\n2.5 -2 -3 -2 -3 false -2.5 true false 65 A\nA\0B A\0BA\0B tr"
        "ue true true 2 5\n";
    XrSourceCompileOptions compiler = compile_options(stdlib);
    XrSourceArtifact *program = compile(&compiler, source, __func__);
    Output out = {0}; XrSourceCallOptions options;
    CHECK(xray_source_call_options_init(&options, sizeof(options)) == XR_SOURCE_OK);
    options.write = capture; options.context = &out;
    XrSourceResult *result = NULL; CHECK(execute_owned(program, &options, &result, NULL) == XR_SOURCE_OK && result);
    xray_source_artifact_free(program); XrSourceEvent event;
    CHECK(xray_source_result_event(result, &event) == XR_SOURCE_OK && event.state == XR_SOURCE_RUN_RETURNED);
    CHECK(out.length == sizeof(expected) - 1 && !memcmp(out.bytes, expected, sizeof(expected) - 1));
    xray_source_result_free(result);
}
static void generic_initializers(const char *stdlib) {
    char source[] =
        "import \"std/io/output\" as Lib\n"
        "final class Packet<T> { value:T; tail:Array<T>;\n"
        " init(value,tail:Array<T>=[]) { this.value=value; this.tail=tail } }\n"
        "fn make<T>(value:T)->Packet<T> { return Packet<T>(value) }\n"
        "fn tagged<T>(value:T,tail:Array<T>)->Packet<T> { return Packet<T>(value,tail) }\n"
        "fn copied(packet:Packet<string>)->string { return packet.value.toString() }\n"
        "fn number(packet:Packet<i64>)->i64 { return packet.value }\n"
        "fn count<T>(packet:Packet<T>)->i64 { return len(packet.tail) }\n"
        "fn first<T>(packet:Packet<T>)->T { return packet.tail[0] }\n"
        "const text=make<string>(\"A\\0\xe4\xb8\xad\xf0\x9f\x98\x80\")\n"
        "Lib.writeStdout(copied(text)); print(count<string>(text))\n"
        "var original=[11,22]; const value=tagged<i64>(7,original); original[0]=99\n"
        "print(number(value),count<i64>(value),first<i64>(value),original[0])\n";
    const char expected[] = "A\0\xe4\xb8\xad\xf0\x9f\x98\x80" "0\n7 2 11 99\n";
    XrSourceCompileOptions compiler = compile_options(stdlib);
    XrSourceArtifact *program = compile(&compiler, source, __func__);
    Output out = {0}; XrSourceCallOptions options;
    CHECK(xray_source_call_options_init(&options, sizeof(options)) == XR_SOURCE_OK);
    options.write = capture; options.context = &out;
    XrSourceResult *result = NULL;
    CHECK(execute_owned(program, &options, &result, NULL) == XR_SOURCE_OK && result);
    xray_source_artifact_free(program); XrSourceEvent event;
    CHECK(xray_source_result_event(result, &event) == XR_SOURCE_OK && event.state == XR_SOURCE_RUN_RETURNED);
    CHECK(out.length == sizeof(expected)-1 && !memcmp(out.bytes, expected, sizeof(expected)-1));
    int64_t code = -1; CHECK(xray_source_result_i64(result, &code) == XR_SOURCE_OK && code == 0);
    xray_source_result_free(result);
}
static void compiler_constants(const char *stdlib) {
    char first[] =
        "fn trunc(value:f64)->i64{return value.toI64()}\n"
        "fn down(value:f64)->i64{return value.floor()}\n"
        "fn up(value:f64)->i64{return value.ceil()}\n"
        "fn round(value:f64)->i64{return value.round()}\n"
        "fn constants()->(i16,i16,i16,i16){return (2,3,5,7)}\n"
        "fn nested()->((i8,()),string){var value:((i8,()),string)=((1,()),\"x\");"
        "value=((7,()),\"\xc3\xa9\xf0\x9f\x98\x80\\0\");return value}\n"
        "fn label()->string{return \"A\\0\xc3\xa9\"}\n"
        "fn echo<T>(value:T)->(T,string){return (value,label())}\n"
        "print(comptime trunc(-2.5),comptime down(-2.5),comptime up(-2.5),comptime round(-2.5))\n"
        "const (a,b,c,d)=comptime constants();print(a,b,c,d,(a as i64)+(b as i64)*10+(c as i64)*100+(d as i64)*1000)\n"
        "const tuple=comptime nested();print(tuple.0.0,tuple.1)\n"
        "const pair=comptime echo<i8>(7);print(pair.0,pair.1)\n";
    char second[] = "fn label()->string{return \"B\\0\xe4\xb8\xad\"}\n"
        "fn echo<T>(value:T)->(T,string){return (value,label())}\n"
        "const pair=comptime echo<i8>(9);print(pair.0,pair.1)\n";
    const char expected_first[] = "-2 -3 -2 -3\n2 3 5 7 7532\n7 \xc3\xa9\xf0\x9f\x98\x80\0\n7 A\0\xc3\xa9\n";
    const char expected_second[] = "9 B\0\xe4\xb8\xad\n";
    const char *expected[] = {expected_first,expected_second};
    const size_t lengths[] = {sizeof(expected_first)-1,sizeof(expected_second)-1};
    XrSourceCompileOptions compiler = compile_options(stdlib);
    XrSourceArtifact *programs[] = {compile(&compiler, first, __func__),NULL};
    programs[1] = compile(&compiler, second, __func__);
    for (size_t i=0;i<2;++i) {
        Output out = {0}; XrSourceCallOptions options;
        CHECK(xray_source_call_options_init(&options, sizeof(options)) == XR_SOURCE_OK);
        options.write = capture; options.context = &out;
        XrSourceResult *result = NULL;
        CHECK(execute_owned(programs[i], &options, &result, NULL) == XR_SOURCE_OK && result);
        xray_source_artifact_free(programs[i]); XrSourceEvent event;
        CHECK(xray_source_result_event(result, &event) == XR_SOURCE_OK && event.state == XR_SOURCE_RUN_RETURNED);
        CHECK(out.length == lengths[i] && !memcmp(out.bytes, expected[i], lengths[i]));
        xray_source_result_free(result);
    }
}
static void typed_tasks(const char *stdlib) {
    char source[] =
        "fn w_i8()->i8{return -128}\nfn w_i16()->i16{return -32768}\n"
        "fn w_i32()->i32{return -2147483648}\nfn w_u8()->u8{return 255}\n"
        "fn w_u16()->u16{return 65535}\nfn w_u32()->u32{return 4294967295}\n"
        "fn w_u64()->u64{return 18446744073709551615}\n"
        "fn w_f32()->f32{return 1.5}\nfn w_f64()->f64{return 1.5}\n"
        "fn w_rune()->rune{return 'x'}\nfn w_i64()->i64{return -42}\n"
        "fn w_bool()->bool{return true}\nfn w_string()->string{return \"A\\0B\"}\nfn w_unit(){}\n"
        "const t_i8=go w_i8();print(try await t_i8);print(try await t_i8)\n"
        "const t_i16=go w_i16();print(try await t_i16);print(try await t_i16)\n"
        "const t_i32=go w_i32();print(try await t_i32);print(try await t_i32)\n"
        "const t_u8=go w_u8();print(try await t_u8);print(try await t_u8)\n"
        "const t_u16=go w_u16();print(try await t_u16);print(try await t_u16)\n"
        "const t_u32=go w_u32();print(try await t_u32);print(try await t_u32)\n"
        "const t_u64=go w_u64();print(try await t_u64);print(try await t_u64)\n"
        "const t_f32=go w_f32();print(try await t_f32);print(try await t_f32)\n"
        "const t_f64=go w_f64();print(try await t_f64);print(try await t_f64)\n"
        "const t_rune=go w_rune();print(try await t_rune);print(try await t_rune)\n"
        "const t_i64=go w_i64();print(try await t_i64);print(try await t_i64)\n"
        "const t_bool=go w_bool();print(try await t_bool);print(try await t_bool)\n"
        "const t_string=go w_string();print(try await t_string);print(try await t_string)\n"
        "const t_unit=go w_unit();try await t_unit;try await t_unit;print(\"unit\")\n";
    const char expected[] = "-128\n-128\n-32768\n-32768\n-2147483648\n-2147483648\n"
        "255\n255\n65535\n65535\n4294967295\n4294967295\n"
        "18446744073709551615\n18446744073709551615\n1.5\n1.5\n1.5\n1.5\n"
        "x\nx\n-42\n-42\ntrue\ntrue\nA\0B\nA\0B\nunit\n";
    XrSourceCompileOptions compiler = compile_options(stdlib);
    XrSourceArtifact *program = compile(&compiler, source, __func__);
    XrSourceCallOptions options; CHECK(xray_source_call_options_init(&options, sizeof(options)) == XR_SOURCE_OK);
    Output out = {{0}, 0}; options.write = capture; options.context = &out;
    XrSourceResult *result = NULL; CHECK(execute_owned(program, &options, &result, NULL) == XR_SOURCE_OK);
    xray_source_artifact_free(program); XrSourceEvent event;
    CHECK(xray_source_result_event(result, &event) == XR_SOURCE_OK && event.state == XR_SOURCE_RUN_RETURNED);
    CHECK(out.length == sizeof(expected) - 1 && !memcmp(out.bytes, expected, sizeof(expected) - 1));
    xray_source_result_free(result);
}
static XrSourceEvent poll_state(XrSourceCall *call) {
    XrSourceEvent event = {0};
    for (size_t steps = 0; steps < 4096; ++steps) {
        CHECK(xray_source_call_poll(call, 8, &event) == XR_SOURCE_OK);
        if (event.state != XR_SOURCE_RUN_READY) return event;
    }
    CHECK(0); return event;
}
static void timer_waits(const char *stdlib) {
    XrSourceCompileOptions compiler = compile_options(stdlib);
    char source[] = "import time\nfn worker()->i64{time.sleep(3);return 43}\n"
        "fn main() throws(Error)->i64{return try await go worker()}\nprint(try main())\n";
    XrSourceArtifact *program = compile(&compiler, source, __func__);
    Output out = {0}; XrSourceCallOptions options;
    CHECK(xray_source_call_options_init(&options, sizeof(options)) == XR_SOURCE_OK);
    options.write = capture; options.context = &out;
    XrSourceCall *call = NULL; CHECK(begin_owned(program, &options, &call) == XR_SOURCE_OK);
    xray_source_artifact_free(program);
    XrSourceEvent event = poll_state(call);
    CHECK(event.state == XR_SOURCE_RUN_SUSPENDED && event.epoch && event.wake && !out.length);
    XrSourceWait wait = {XR_SOURCE_WAIT_TASK, 91, &event, 92, 93};
    unsigned char unchanged[sizeof(wait)]; memcpy(unchanged, &wait, sizeof(wait));
    CHECK(xray_source_call_wait_request(call, event.epoch, event.wake, &wait, 16) == XR_SOURCE_BAD_ARGUMENT);
    CHECK(!memcmp(unchanged, &wait, sizeof(wait)));
    CHECK(xray_source_call_wait_request(call, event.epoch + 1, event.wake, &wait, sizeof(wait)) == XR_SOURCE_BAD_STATE);
    CHECK(!memcmp(unchanged, &wait, sizeof(wait)));
    CHECK(xray_source_call_wait_request(call, event.epoch, event.wake, &wait, sizeof(wait)) == XR_SOURCE_OK);
    CHECK(wait.kind == XR_SOURCE_WAIT_TIMER && wait.after_ms == 3 && !wait.subject && !wait.generation && !wait.ticket);
    CHECK(xray_source_call_resume(call, event.epoch, event.wake + 1) == XR_SOURCE_BAD_STATE);
    struct timespec delay = {0, 3000000}, remaining;
    int slept;
    do { slept = thrd_sleep(&delay, &remaining); CHECK(slept == 0 || slept == -1); if (slept == -1) delay = remaining; } while (slept == -1);
    CHECK(xray_source_call_resume(call, event.epoch, event.wake) == XR_SOURCE_OK);
    CHECK(xray_source_call_wait_request(call, event.epoch, event.wake, &wait, sizeof(wait)) == XR_SOURCE_BAD_STATE);
    CHECK(poll_state(call).state == XR_SOURCE_RUN_RETURNED);
    XrSourceResult *result = NULL; CHECK(xray_source_call_take(call, &result) == XR_SOURCE_OK && result);
    CHECK(xray_source_call_drop(&call, &event) == XR_SOURCE_OK && !call);
    CHECK(out.length == 3 && !memcmp(out.bytes, "43\n", 3)); xray_source_result_free(result);
}
typedef struct TaskCase { const char *name, *source, *expected; size_t length; } TaskCase;
#define TASK_CASE(name, source, expected) {name, source, expected, sizeof(expected)-1}
static const TaskCase task_cases[] = {
    TASK_CASE("scalar-control",
        "fn worker()->string { Coro.yield(); return \"control\\0é\" }\n"
        "fn calculate() throws(Error) { const task:Task<string> = go worker(); const first=try await task;"
        " assert(first==\"control\\0é\"); print(try await task) }\ntry calculate()\n",
        "control\0é\n"),
    TASK_CASE("array-i64",
        "fn worker()->Array<i64> { Coro.yield(); return [17,29] }\n"
        "fn calculate() throws(Error) { const task:Task<Array<i64>> = go worker(); var first=try await task;"
        " first[0]=101; assert(first[0]==101); const next=try await task; print(next[0]); print(next[1]) }\ntry calculate()\n",
        "17\n29\n"),
    TASK_CASE("nested-array-string",
        "fn worker()->Array<Array<string>> { Coro.yield(); return [[\"a\\0b\",\"é😀\"],[\"tail\"]] }\n"
        "fn calculate() throws(Error) { const task:Task<Array<Array<string>>> = go worker(); var first=try await task;"
        " first[0][0]=\"changed\"; assert(first[0][0]==\"changed\"); const next=try await task;"
        " print(next[0][0]); print(next[0][1]); print(next[1][0]) }\ntry calculate()\n",
        "a\0b\né😀\ntail\n"),
    TASK_CASE("nullable-i64",
        "fn worker()->i64? { Coro.yield(); return -17 }\n"
        "fn calculate() throws(Error) { const task:Task<i64?> = go worker(); const first=try await task;"
        " assert(first! == -17); const next=try await task; print(next!) }\ntry calculate()\n",
        "-17\n"),
    TASK_CASE("nullable-array",
        "fn worker()->Array<i64>? { Coro.yield(); return [17,29] }\n"
        "fn calculate() throws(Error) { const task:Task<Array<i64>?> = go worker(); const first=try await task;"
        " assert(first![0]==17); const next=try await task; print(next![1]) }\ntry calculate()\n",
        "29\n"),
    TASK_CASE("tuple-string",
        "fn worker()->(i64,string) { Coro.yield(); return (-17,\"t\\0é\") }\n"
        "fn calculate() throws(Error) { const task:Task<(i64,string)> = go worker(); const first=try await task;"
        " assert(first.0 == -17); const next=try await task; print(next.0); print(next.1) }\ntry calculate()\n",
        "-17\nt\0é\n"),
    TASK_CASE("enum-array",
        "enum Choice { Empty, Selected { values:Array<i64> } }\n"
        "fn worker()->Choice { Coro.yield(); return Choice.Selected{values:[17,29]} }\n"
        "fn calculate() throws(Error) { const task:Task<Choice> = go worker(); const first=try await task;"
        " print(match(first){Choice.Empty -> 0, Choice.Selected{values} -> values[0]});"
        " const next=try await task; print(match(next){Choice.Empty -> 0, Choice.Selected{values} -> values[1]}) }\ntry calculate()\n",
        "17\n29\n"),
    TASK_CASE("atomic-shared",
        "fn worker()->Atomic<i64> { Coro.yield(); return Atomic<i64>(17) }\n"
        "fn calculate() throws(Error) { const task:Task<Atomic<i64>> = go worker(); const first=try await task;"
        " print(first.fetchAdd(5)); const next=try await task; print(next.load()) }\ntry calculate()\n",
        "17\n22\n"),
    TASK_CASE("task-nested",
        "fn leaf()->i64 { Coro.yield(); return 29 }\n"
        "fn worker()->Task<i64> { Coro.yield(); return go leaf() }\n"
        "fn calculate() throws(Error) { const task:Task<Task<i64>> = go worker(); const first=try await task;"
        " const next=try await task; print(try await first); print(try await next) }\ntry calculate()\n",
        "29\n29\n"),
    TASK_CASE("try-optional",
        "enum PullFailure { Bad { detail: string } }\n"
        "fn element(fail: bool) throws(PullFailure) -> i64 {\n"
        "    if (fail) { throw PullFailure.Bad { detail: \"\303\251\\0\347\225\214\" } }\n"
        "    return 17\n"
        "}\n"
        "fn maybeNull(mode: i64) throws(PullFailure) -> i64\? {\n"
        "    if (mode == 0) { return null }\n"
        "    if (mode == 1) { return 13 }\n"
        "    throw PullFailure.Bad { detail: \"optional failure\" }\n"
        "}\n"
        "fn values() -> Iterator<i64, PullFailure> {\n"
        "    yield 7\n"
        "    throw PullFailure.Bad { detail: \"pull failure\" }\n"
        "}\n"
        "fn pureValues() -> Iterator<i64> { yield 19 }\n"
        "fn text() throws(PullFailure) -> string { return \"owned \303\251\\0\347\225\214\" }\n"
        "fn part(mark: i64, fail: bool) throws(PullFailure) -> i64 { print(mark);return try element(fail) }\n"
        "fn foldedValue() -> i64 { return (try\? element(true)) \?\? 41 }\n"
        "fn divide(divisor: i64) -> i64 { return 1 / divisor }\n"
        "const folded = comptime foldedValue()\n"
        "print((try\? element(false)) \?\? -1)\n"
        "print((try\? element(true)) \?\? -1)\n"
        "const empty: i64\? = try\? maybeNull(0)\n"
        "const present: i64\? = try\? maybeNull(1)\n"
        "const failed: i64\? = try\? maybeNull(2)\n"
        "print(empty \?\? -2)\n"
        "print(present \?\? -2)\n"
        "print(failed \?\? -2)\n"
        "const it = values()\n"
        "print((try\? it.next()) \?\? -1)\n"
        "print((try\? it.hasNext()) \?\? false)\n"
        "const indexed = values()\n"
        "print((try\? indexed.nth(1)) \?\? -1)\n"
        "const pure = pureValues()\n"
        "print((try\? pure.next()) \?\? -1)\n"
        "print(folded)\n"
        "const owned: string\? = try\? text()\n"
        "print(owned \?\? \"missing\")\n"
        "print((try\? (part(31, false) + part(32, true) + part(33, false))) \?\? -3)\n"
        "try { const ignored = try\? divide(0); print(ignored \?\? -9) }\n"
        "catch panic (p) { print(p.code) }\n",
        "17\n-1\n-2\n13\n-2\n7\nfalse\n-1\n19\n41\nowned \303\251\000\347\225\214\n31\n32\n-3\n420\n"),
    TASK_CASE("typed-await",
        "enum A { Bad { text: string } }\n"
        "enum B { Bad }\n"
        "fn value() -> string { return \"owned \303\251\\0\347\225\214\" }\n"
        "fn failOne() throws(A) -> string { throw A.Bad { text: \"owned \303\251\\0\347\225\214\" } }\n"
        "fn failUnion(first: bool) throws(A|B) -> i64 {\n"
        "    if (first) { throw A.Bad { text: \"union \303\251\\0\347\225\214\" } }\n"
        "    throw B.Bad\n"
        "}\n"
        "export fn awaitNever() throws(Cancelled) -> string {\n"
        "    var task: Task<string> = go value()\n"
        "    const first = try await task\n"
        "    task.cancel()\n"
        "    task.cancel()\n"
        "    return try await task\n"
        "}\n"
        "export fn awaitOne() throws(A|Cancelled) -> string {\n"
        "    const task: Task<string,A> = go failOne()\n"
        "    return try await task\n"
        "}\n"
        "export fn awaitUnion(first: bool) throws(A|B|Cancelled) -> i64 {\n"
        "    const task: Task<i64,A|B> = go failUnion(first)\n"
        "    return try await task\n"
        "}\n"
        "fn spinning() -> i64 {\n"
        "    defer { print(\"child-cleanup\") }\n"
        "    while (true) { Coro.yield() }\n"
        "    return 7\n"
        "}\n"
        "export fn childCancellation() -> i64 {\n"
        "    var task: Task<i64> = go spinning()\n"
        "    task.cancel()\n"
        "    task.cancel()\n"
        "    var caught = 0\n"
        "    try { const ignored = try await task } catch (e: Cancelled) { caught += 1 }\n"
        "    try { const ignored = try await task } catch (e: Cancelled) { caught += 1 }\n"
        "    return caught\n"
        "}\n"
        "export fn cancelledError() throws(Cancelled) -> i64 {\n"
        "    var task = go spinning()\n"
        "    task.cancel()\n"
        "    return try await task\n"
        "}\n"
        "export fn ownCancellation() -> i64 {\n"
        "    var task = go spinning()\n"
        "    defer { print(\"parent-cleanup\"); task.cancel() }\n"
        "    try { return try await task }\n"
        "    catch (e: Cancelled) { print(\"caught-own\"); return -1 }\n"
        "}\n"
        "export fn independentParent() -> i64 {\n"
        "    defer { print(\"parent-cleanup\") }\n"
        "    const task = go spinning()\n"
        "    try { return try await task }\n"
        "    catch (e: Cancelled) { print(\"caught-own\"); return -1 }\n"
        "}\n"
        "\n"
        "print(childCancellation())\n"
        "print(try awaitNever())\n"
        "try { print(try awaitOne()) } catch (e: A) { print(\"one-error\") }\n"
        "try { print(try awaitUnion(true)) } catch (e: A) { print(\"union-a\") } catch (e: B) { print(\"wrong-b\") }\n"
        "try { print(try awaitUnion(false)) } catch (e: A) { print(\"wrong-a\") } catch (e: B) { print(\"union-b\") }\n"
        , "child-cleanup\n2\nowned \303\251\000\347\225\214\none-error\nunion-a\nunion-b\n"),
};
static int constructed_task_case(const TaskCase *test, const char *stdlib, size_t ordinal) {
    XrSourceCompileOptions compiler = compile_options(stdlib);
    size_t length = strlen(test->source);CHECK(length<SIZE_MAX);
    char *source=malloc(length+1);CHECK(source);memcpy(source,test->source,length+1);
    XrSourceText text = {NULL, test->name, source, length};
    XrSourceArtifact *program = NULL; XrSourceDiagnostic *diagnostic = NULL;
    XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,.value.text=text};
    XrSourceFailure failure={0};
    XrSourceStatus status = xray_source_build(&input, &compiler, &program, &failure, &diagnostic);
    if (status != XR_SOURCE_OK || !program || diagnostic)
        fprintf(stderr, "[sdk-compile-failure] case=constructed_tasks/%s status=%u\n",
            test->name, (unsigned)status);
    memset(source, '!', length+1);free(source);

    if (status != XR_SOURCE_OK) {
        CHECK(!program && diagnostic); XrSourceDiagnosticRoot info={0};XrSourceDiagnosticReportInfo report_info={0};
        CHECK((xray_source_diagnostic_report(diagnostic,&report_info)==XR_SOURCE_OK && report_info.roots==1 ? xray_source_diagnostic_root(diagnostic,0,&info) : XR_SOURCE_BAD_STATE) == XR_SOURCE_OK);
        fprintf(stderr, "case=%s compile status=%u stage=%u message=%s\n",
            test->name, (unsigned)status, (unsigned)failure.stage, info.message);
        xray_source_diagnostic_free(diagnostic);
        return (int)(10+ordinal);
    }
    CHECK(program && !diagnostic); Output out = {{0}, 0};
    XrSourceCallOptions options;
    CHECK(xray_source_call_options_init(&options, sizeof(options)) == XR_SOURCE_OK);
    options.write = capture; options.context = &out;
    XrSourceCall *call = NULL;
    CHECK(begin_owned(program, &options, &call) == XR_SOURCE_OK && call);
    xray_source_artifact_free(program);
    XrSourceEvent event = {0}; size_t steps = 0;
    for (;;) {
        CHECK(steps++ < 4096);
        CHECK(xray_source_call_poll(call, 8, &event) == XR_SOURCE_OK);
        if (event.state == XR_SOURCE_RUN_READY) continue;
        if (event.state != XR_SOURCE_RUN_SUSPENDED) break;
        XrSourceWait wait;
        CHECK(xray_source_call_wait_request(call, event.epoch, event.wake, &wait, sizeof(wait)) == XR_SOURCE_OK);
        CHECK(wait.kind == XR_SOURCE_WAIT_YIELD || wait.kind == XR_SOURCE_WAIT_TASK);
        CHECK(xray_source_call_resume(call, event.epoch, event.wake) == XR_SOURCE_OK);
        CHECK(xray_source_call_wait_request(call, event.epoch, event.wake, &wait, sizeof(wait)) == XR_SOURCE_BAD_STATE);
    }
    CHECK(event.state == XR_SOURCE_RUN_RETURNED);
    XrSourceResult *result = NULL;
    CHECK(xray_source_call_take(call, &result) == XR_SOURCE_OK && result);
    CHECK(xray_source_call_drop(&call, NULL) == XR_SOURCE_OK && !call);
    CHECK(xray_source_result_event(result, &event) == XR_SOURCE_OK && event.state == XR_SOURCE_RUN_RETURNED);
    int64_t value = -1;
    CHECK(xray_source_result_i64(result, &value) == XR_SOURCE_OK && value == 0);
    CHECK(out.length == test->length && !memcmp(out.bytes, test->expected, test->length));
    xray_source_result_free(result);
    printf("case=%s PASS bytes=%zu producers-dead\n", test->name, out.length);
    return 0;
}
static void constructed_tasks(const char *stdlib) {
    for (size_t i = 0; i < sizeof(task_cases)/sizeof(task_cases[0]); ++i)
        CHECK(constructed_task_case(&task_cases[i], stdlib, i) == 0);
}
/* Silent API4 check: the installed report survives the entire source request. */
static void installed_report(const char *stdlib) {
    char text[]="const alpha:i64=1\nconst alpha:i64=2\nconst beta:i64=3\nconst beta:i64=4\nprint(9)\n";
    XrSourceCompileOptions options=xray_source_compile_options_default();options.stdlib_path=stdlib;
    XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,.value.text={NULL,"installed-report",text,sizeof(text)-1}};
    XrSourceArtifact *artifact=NULL;XrSourceDiagnostic *report=NULL;XrSourceFailure failure={0};
    CHECK(xray_source_build(&input,&options,&artifact,&failure,&report)==XR_SOURCE_BAD_ARGUMENT&&!artifact&&report);
    CHECK(failure.status==XR_SOURCE_BAD_ARGUMENT&&failure.cleanup_status==XR_SOURCE_OK);
    memset(text,'!',sizeof(text));XrSourceDiagnosticReportInfo info={0};
    CHECK(xray_source_diagnostic_report(report,&info)==XR_SOURCE_OK&&info.roots==2&&!info.complete);
    for(uint32_t i=0;i<info.roots;++i) {
        XrSourceDiagnosticRoot root={0};XrSourceDiagnosticLabel label={0};
        CHECK(xray_source_diagnostic_root(report,i,&root)==XR_SOURCE_OK&&root.code==302&&root.labels==1);
        CHECK(root.primary.line==(int32_t)(2+2*i)&&root.primary.column==7&&!strcmp(root.message,"duplicate declaration name"));
        CHECK(xray_source_diagnostic_label(report,i,0,&label)==XR_SOURCE_OK&&label.location.line==(int32_t)(1+2*i));
        CHECK(label.location.column==7&&!strcmp(label.message,"first declaration is here"));
        CHECK((root.primary.flags&(XR_SOURCE_LOCATION_BYTES|XR_SOURCE_LOCATION_UTF16|XR_SOURCE_LOCATION_REVISION))==
            (XR_SOURCE_LOCATION_BYTES|XR_SOURCE_LOCATION_UTF16|XR_SOURCE_LOCATION_REVISION));
        CHECK(!memcmp(root.primary.revision,label.location.revision,32));
    }
    XrSourceDiagnosticRoot saved={.code=123};
    CHECK(xray_source_diagnostic_root(report,2,&saved)==XR_SOURCE_BAD_ARGUMENT&&saved.code==123);
    xray_source_diagnostic_free(report);
}
/* Parameter scopes share the report owner and outlive their borrowed input. */
static void installed_parameter_report(const char *stdlib) {
    char text[]="const value:i64=99\r\n"
        "/*\303\251\360\237\230\200*/ fn alpha(value:i64,value:i64)->i64{return value}\r\n"
        "/*\303\251\360\237\230\200*/ fn beta(value:i64,value:i64)->i64{return value}\r\n"
        "print(alpha(4,5)+beta(5,6))\r\n";
    XrSourceCompileOptions options=compile_options(stdlib);
    XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,.value.text={NULL,"installed-parameter-report",text,sizeof(text)-1}};
    XrSourceArtifact *artifact=NULL;XrSourceDiagnostic *report=NULL;XrSourceFailure failure={0};
    CHECK(xray_source_build(&input,&options,&artifact,&failure,&report)==XR_SOURCE_BAD_ARGUMENT&&!artifact&&report);
    CHECK(failure.status==XR_SOURCE_BAD_ARGUMENT&&failure.cleanup_status==XR_SOURCE_OK);
    memset(text,'!',sizeof(text));XrSourceDiagnosticReportInfo info={0};
    CHECK(xray_source_diagnostic_report(report,&info)==XR_SOURCE_OK&&info.roots==2&&!info.complete);
    const uint64_t original[]={40,100},duplicate[]={50,110};
    for(uint32_t i=0;i<2;++i) {
        XrSourceDiagnosticRoot root={0};XrSourceDiagnosticLabel label={0};
        CHECK(xray_source_diagnostic_root(report,i,&root)==XR_SOURCE_OK&&root.code==302&&root.labels==1);
        CHECK(!strcmp(root.message,"duplicate parameter name"));
        CHECK(xray_source_diagnostic_label(report,i,0,&label)==XR_SOURCE_OK);
        CHECK(!strcmp(label.message,"first declaration is here"));
        CHECK(root.primary.line==(int32_t)(2+i)&&root.primary.column==(int32_t)(31-i));
        CHECK(label.location.line==(int32_t)(2+i)&&label.location.column==(int32_t)(21-i));
        CHECK(root.primary.start_byte==duplicate[i]&&root.primary.end_byte==duplicate[i]+5);
        CHECK(label.location.start_byte==original[i]&&label.location.end_byte==original[i]+5);
        CHECK(root.primary.utf16_line==1+i&&root.primary.utf16_column==27-i);
        CHECK(label.location.utf16_line==1+i&&label.location.utf16_column==17-i);
        CHECK((root.primary.flags&(XR_SOURCE_LOCATION_LINE|XR_SOURCE_LOCATION_BYTES|XR_SOURCE_LOCATION_UTF16|XR_SOURCE_LOCATION_REVISION))==
            (XR_SOURCE_LOCATION_LINE|XR_SOURCE_LOCATION_BYTES|XR_SOURCE_LOCATION_UTF16|XR_SOURCE_LOCATION_REVISION));
        CHECK(root.primary.flags==label.location.flags&&!memcmp(root.primary.revision,label.location.revision,32));
    }
    XrSourceDiagnosticLabel saved={.location={.line=123}};
    CHECK(xray_source_diagnostic_label(report,0,1,&saved)==XR_SOURCE_BAD_ARGUMENT&&saved.location.line==123);
    xray_source_diagnostic_free(report);
    char corrected[]="const value:i64=99\r\n"
        "/*\303\251\360\237\230\200*/ fn alpha(value:i64,other:i64)->i64{return value}\r\n"
        "/*\303\251\360\237\230\200*/ fn beta(value:i64,other:i64)->i64{return value}\r\n"
        "print(alpha(4,5)+beta(5,6))\r\n";
    artifact=compile(&options,corrected,__func__);
    Output out={0};XrSourceCallOptions runtime;
    CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    runtime.write=capture;runtime.context=&out;
    XrSourceResult *result=NULL;XrSourceEvent event={0};
    CHECK(execute_owned(artifact,&runtime,&result,&event)==XR_SOURCE_OK&&result);
    xray_source_artifact_free(artifact);
    CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK&&event.state==XR_SOURCE_RUN_RETURNED);
    int64_t value=99;CHECK(xray_source_result_i64(result,&value)==XR_SOURCE_OK&&!value);
    CHECK(out.length==2&&!memcmp(out.bytes,"9\n",2));xray_source_result_free(result);
}
static XrSourceWriteStatus error_report_capture(void *context,XrSourceOutputStream stream,const char *bytes,size_t length) {
    CHECK(stream==XR_SOURCE_STDERR);return capture(context,XR_SOURCE_STDOUT,bytes,length);
}
static void installed_error_union(const char *stdlib) {
    char source[]=
        "enum First{Bad{message:string}}\n"
        "enum Second{Bad}\n"
        "enum Third{Bad}\n"
        "enum Fourth{Bad}\n"
        "enum Fifth{Bad}\n"
        "enum Sixth{Bad}\n"
        "enum Seventh{Bad}\n"
        "fn value(which:i64) throws(First|Second|Third|Fourth|Fifth|Sixth|Seventh)->i64{\n"
        "    if(which==0){throw First.Bad{message:\"owned \303\251\\0\347\225\214\"}}\n"
        "    throw Seventh.Bad\n"
        "}\n"
        "fn relay(which:i64) throws(First|Second|Third|Fourth|Fifth|Sixth|Seventh)->i64{return try value(which)}\n"
        "export fn escaped() throws(First|Seventh)->i64{throw First.Bad{message:\"owned \303\251\\0\347\225\214\"}}\n"
        "export fn answer()->i64{\n"
        "    try{return relay(6)}catch(error:Seventh){return 42}catch(error){return -1}\n"
        "}\n"
        "print(answer())\n";
    XrSourceCompileOptions compiler=compile_options(stdlib);XrSourceArtifact *artifact=compile(&compiler,source,__func__);
    Output output={0};XrSourceCallOptions options;CHECK(xray_source_call_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.write=capture;options.context=&output;XrSourceResult *result=NULL;XrSourceEvent event={0};
    CHECK(execute_owned(artifact,&options,&result,&event)==XR_SOURCE_OK && result);
    CHECK(output.length==3 && !memcmp(output.bytes,"42\n",3));xray_source_result_free(result);result=NULL;output=(Output){0};
    XrSourceExecutionOptions policy=xray_source_execution_options_default();XrSourceExecution *execution=NULL;
    CHECK(xray_source_execution_new(&policy,&execution)==XR_SOURCE_OK);
    XrSourceInvocation invocation;CHECK(xray_source_invocation_init(&invocation,sizeof(invocation))==XR_SOURCE_OK);
    invocation.name="escaped";invocation.name_length=7;
    CHECK(xray_source_invoke(execution,artifact,&invocation,&options,&result,&event)==XR_SOURCE_OK && result);
    xray_source_execution_free(execution);xray_source_artifact_free(artifact);
    CHECK(output.length==3 && !memcmp(output.bytes,"42\n",3));
    XrSourceResult *copy=NULL;CHECK(xray_source_result_copy(result,&copy)==XR_SOURCE_OK && copy);xray_source_result_free(result);
    CHECK(xray_source_result_event(copy,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_THROWN);
    XrSourceValue *value=NULL;CHECK(xray_source_result_value(copy,&value)==XR_SOURCE_OK && value);
    output=(Output){0};CHECK(xray_source_result_format(copy,error_report_capture,&output)==XR_SOURCE_OK);
    const char report[]="[Uncaught Error] First.Bad(\"owned \303\251\000\347\225\214\")\n";
    CHECK(output.length==sizeof(report)-1 && !memcmp(output.bytes,report,output.length));xray_source_result_free(copy);
    XrSourceValueKind kind=XR_SOURCE_VALUE_UNIT;CHECK(xray_source_value_kind(value,&kind)==XR_SOURCE_OK && kind==XR_SOURCE_VALUE_ENUM);
    output=(Output){0};CHECK(xray_source_value_format(value,capture,&output)==XR_SOURCE_OK);
    const char rendered[]="First.Bad(\"owned \303\251\000\347\225\214\")";
    CHECK(output.length==sizeof(rendered)-1 && !memcmp(output.bytes,rendered,output.length));xray_source_value_free(value);
}
static void installed_error_union_300(const char *stdlib) {
    char source[32768];size_t length=0;
    for (unsigned i=0;i<300;++i) {
        int written=snprintf(source+length,sizeof(source)-length,"enum E%u{Bad}\n",i);
        CHECK(written>0 && (size_t)written<sizeof(source)-length);length+=(size_t)written;
    }
    const char start[]="fn value() throws(";memcpy(source+length,start,sizeof(start)-1);length+=sizeof(start)-1;
    for (unsigned i=0;i<300;++i) {
        int written=snprintf(source+length,sizeof(source)-length,"%sE%u",i?"|":"",i);
        CHECK(written>0 && (size_t)written<sizeof(source)-length);length+=(size_t)written;
    }
    const char tail[]=")->i64{throw E299.Bad}\nexport fn answer()->i64{try{return value()}catch(e:E299){return 42}catch(e){return -1}}\nprint(answer())\n";
    CHECK(sizeof(tail)<sizeof(source)-length);memcpy(source+length,tail,sizeof(tail));length+=sizeof(tail)-1;CHECK(length==5924);
    XrSourceCompileOptions compiler=compile_options(stdlib);XrSourceArtifact *artifact=compile(&compiler,source,__func__);
    Output output={0};XrSourceCallOptions options;CHECK(xray_source_call_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.write=capture;options.context=&output;XrSourceResult *result=NULL;XrSourceEvent event={0};
    CHECK(execute_owned(artifact,&options,&result,&event)==XR_SOURCE_OK && result);xray_source_artifact_free(artifact);
    CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_RETURNED);
    CHECK(output.length==3 && !memcmp(output.bytes,"42\n",3));xray_source_result_free(result);
}
int main(int argc, char **argv) {
    CHECK(argc == 2); installed_error_union(argv[1]); installed_error_union_300(argv[1]); installed_report(argv[1]); installed_parameter_report(argv[1]); checked_program(argv[1]); detached_cancellation(argv[1]); owned_panic(argv[1]); typed_tasks(argv[1]); timer_waits(argv[1]); library_methods(argv[1]); generic_initializers(argv[1]); compiler_constants(argv[1]); constructed_tasks(argv[1]);
    puts("Installed public Source SDK: Checked io/time, detached cancellation and owned panic PASS"); return 0;
}
