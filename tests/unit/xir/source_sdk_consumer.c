/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_sdk_consumer.c - Installed public API ownership and Checked consumers
 */
#include "xray_source.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
_Static_assert(XRAY_SOURCE_API_VERSION == 2, "Source API version");
_Static_assert(sizeof(XrSourceCompilerOptions) == 48, "compiler options ABI");
_Static_assert(offsetof(XrSourceCompilerOptions, wait_size) == 8, "wait size ABI");
_Static_assert(offsetof(XrSourceCompilerOptions, allocated_bytes) == 16, "compiler limits ABI");
_Static_assert(offsetof(XrSourceCompilerOptions, stdlib_path) == 40, "stdlib path ABI");
_Static_assert(sizeof(XrSourceText) == 32, "text ABI");
_Static_assert(sizeof(XrSourceDiagnosticInfo) == 32, "diagnostic ABI");
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
static XrSourceCompiler *compiler_new(const char *stdlib) {
    XrSourceCompilerOptions options;
    CHECK(xray_source_compiler_options_init(&options, sizeof(options)) == XR_SOURCE_OK);
    CHECK(options.api_version == 2 && options.struct_size == 48 && options.wait_size == 40);
    options.stdlib_path = stdlib; options.allocated_bytes = UINT64_C(64) << 20;
    options.live_bytes = UINT64_C(8) << 20; options.work = UINT64_C(128000000);
    XrSourceCompiler *compiler = NULL;
    CHECK(xray_source_compiler_new(&options, &compiler) == XR_SOURCE_OK && compiler); return compiler;
}
static XrSourceProgram *compile(XrSourceCompiler *compiler, char *bytes) {
    XrSourceText text = {NULL, "installed-embedding", bytes, strlen(bytes)};
    XrSourceProgram *program = NULL; XrSourceDiagnostic *diagnostic = NULL;
    XrSourceStatus status = xray_source_compile_text(compiler, &text, &program, &diagnostic);
    if (diagnostic) { XrSourceDiagnosticInfo info; CHECK(xray_source_diagnostic_info(diagnostic, &info) == XR_SOURCE_OK); fprintf(stderr, "%s\n", info.message); }
    CHECK(status == XR_SOURCE_OK && program && !diagnostic); memset(bytes, '!', strlen(bytes)); return program;
}
static void checked_program(const char *stdlib) {
    XrSourceCompiler *compiler = compiler_new(stdlib);
    char source[] = "import time as Clock\nimport \"std/io/output\" as Output\n"
        "Output.writeStdout(\"A\\0B\")\nconst begin=Clock.nanos()\nClock.sleep(1)\n"
        "print(Clock.nanos()-begin>=1000000)\n";
    XrSourceProgram *program = compile(compiler, source); xray_source_compiler_free(compiler);
    Output out = {0}; XrSourceCallOptions options;
    CHECK(xray_source_call_options_init(&options, sizeof(options)) == XR_SOURCE_OK);
    options.write = capture; options.context = &out;
    XrSourceResult *result = NULL; XrSourceEvent failure = {0};
    CHECK(xray_source_execute(program, &options, &result, &failure) == XR_SOURCE_OK && result);
    xray_source_program_free(program); XrSourceEvent event;
    CHECK(xray_source_result_event(result, &event) == XR_SOURCE_OK && event.state == XR_SOURCE_RUN_RETURNED);
    int64_t value = -1; CHECK(xray_source_result_i64(result, &value) == XR_SOURCE_OK && value == 0);
    CHECK(out.length == 8 && !memcmp(out.bytes, "A\0Btrue\n", 8)); xray_source_result_free(result);
}
static void detached_cancellation(const char *stdlib) {
    XrSourceCompiler *compiler = compiler_new(stdlib);
    char source[] = "var n:i64=0\nwhile(true){ n+=1 }\n";
    XrSourceProgram *program = compile(compiler, source); xray_source_compiler_free(compiler);
    XrSourceCallOptions options; CHECK(xray_source_call_options_init(&options, sizeof(options)) == XR_SOURCE_OK);
    XrSourceCall *call = NULL; CHECK(xray_source_call_begin(program, &options, &call) == XR_SOURCE_OK);
    xray_source_program_free(program); XrSourceEvent event = {0};
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
    XrSourceCompiler *compiler = compiler_new(stdlib); char source[] = "assert(false,\"p\\0中z\")\n";
    XrSourceProgram *program = compile(compiler, source); xray_source_compiler_free(compiler);
    XrSourceCallOptions options; CHECK(xray_source_call_options_init(&options, sizeof(options)) == XR_SOURCE_OK);
    XrSourceResult *result = NULL; CHECK(xray_source_execute(program, &options, &result, NULL) == XR_SOURCE_OK);
    xray_source_program_free(program); XrSourceEvent event;
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
    XrSourceCompiler *compiler = compiler_new(stdlib);
    XrSourceProgram *program = compile(compiler, source); xray_source_compiler_free(compiler);
    Output out = {0}; XrSourceCallOptions options;
    CHECK(xray_source_call_options_init(&options, sizeof(options)) == XR_SOURCE_OK);
    options.write = capture; options.context = &out;
    XrSourceResult *result = NULL; CHECK(xray_source_execute(program, &options, &result, NULL) == XR_SOURCE_OK && result);
    xray_source_program_free(program); XrSourceEvent event;
    CHECK(xray_source_result_event(result, &event) == XR_SOURCE_OK && event.state == XR_SOURCE_RUN_RETURNED);
    CHECK(out.length == sizeof(expected) - 1 && !memcmp(out.bytes, expected, sizeof(expected) - 1));
    xray_source_result_free(result);
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
        "const t_i8=go w_i8();print(await t_i8);print(await t_i8)\n"
        "const t_i16=go w_i16();print(await t_i16);print(await t_i16)\n"
        "const t_i32=go w_i32();print(await t_i32);print(await t_i32)\n"
        "const t_u8=go w_u8();print(await t_u8);print(await t_u8)\n"
        "const t_u16=go w_u16();print(await t_u16);print(await t_u16)\n"
        "const t_u32=go w_u32();print(await t_u32);print(await t_u32)\n"
        "const t_u64=go w_u64();print(await t_u64);print(await t_u64)\n"
        "const t_f32=go w_f32();print(await t_f32);print(await t_f32)\n"
        "const t_f64=go w_f64();print(await t_f64);print(await t_f64)\n"
        "const t_rune=go w_rune();print(await t_rune);print(await t_rune)\n"
        "const t_i64=go w_i64();print(await t_i64);print(await t_i64)\n"
        "const t_bool=go w_bool();print(await t_bool);print(await t_bool)\n"
        "const t_string=go w_string();print(await t_string);print(await t_string)\n"
        "const t_unit=go w_unit();await t_unit;await t_unit;print(\"unit\")\n";
    const char expected[] = "-128\n-128\n-32768\n-32768\n-2147483648\n-2147483648\n"
        "255\n255\n65535\n65535\n4294967295\n4294967295\n"
        "18446744073709551615\n18446744073709551615\n1.5\n1.5\n1.5\n1.5\n"
        "x\nx\n-42\n-42\ntrue\ntrue\nA\0B\nA\0B\nunit\n";
    XrSourceCompiler *compiler = compiler_new(stdlib);
    XrSourceProgram *program = compile(compiler, source); xray_source_compiler_free(compiler);
    XrSourceCallOptions options; CHECK(xray_source_call_options_init(&options, sizeof(options)) == XR_SOURCE_OK);
    Output out = {{0}, 0}; options.write = capture; options.context = &out;
    XrSourceResult *result = NULL; CHECK(xray_source_execute(program, &options, &result, NULL) == XR_SOURCE_OK);
    xray_source_program_free(program); XrSourceEvent event;
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
    XrSourceCompiler *compiler = compiler_new(stdlib);
    char source[] = "import time\nfn worker()->i64{time.sleep(3);return 43}\n"
        "fn main()->i64{return await go worker()}\nprint(main())\n";
    XrSourceProgram *program = compile(compiler, source); xray_source_compiler_free(compiler);
    Output out = {0}; XrSourceCallOptions options;
    CHECK(xray_source_call_options_init(&options, sizeof(options)) == XR_SOURCE_OK);
    options.write = capture; options.context = &out;
    XrSourceCall *call = NULL; CHECK(xray_source_call_begin(program, &options, &call) == XR_SOURCE_OK);
    xray_source_program_free(program);
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
int main(int argc, char **argv) {
    CHECK(argc == 2); checked_program(argv[1]); detached_cancellation(argv[1]); owned_panic(argv[1]); typed_tasks(argv[1]); timer_waits(argv[1]); library_methods(argv[1]);
    puts("Installed public Source SDK: Checked io/time, detached cancellation and owned panic PASS"); return 0;
}
