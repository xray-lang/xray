/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_sdk_consumer.cpp - C++ use of the installed opaque C API
 */
#include "xray_source.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
static_assert(sizeof(XrSourceCallOptions) == 80, "execution call option ABI");
static_assert(sizeof(XrSourceWait) == 40 && offsetof(XrSourceWait, ticket) == 32, "public wait ABI");
struct Output { char bytes[32]{}; size_t size{}; };
static XrSourceWriteStatus capture(void *context, XrSourceOutputStream stream, const char *bytes, size_t length) {
    auto *out = static_cast<Output *>(context);
    CHECK(stream == XR_SOURCE_STDOUT && length <= sizeof(out->bytes) - out->size);
    std::memcpy(out->bytes + out->size, bytes, length); out->size += length; return XR_SOURCE_WRITE_OK;
}
int main(int argc, char **argv) {
    CHECK(argc == 2); auto options=xray_source_compile_options_default();
    options.stdlib_path=argv[1];
    char source[] = "import \"std/io/output\" as Output\nOutput.writeStdout(\"C++\\0SDK\")\n";
    XrSourceText text{nullptr, "cpp-embedding", source, std::strlen(source)};
    XrSourceArtifact *program = nullptr; XrSourceDiagnostic *diagnostic = nullptr;
    XrSourceInput input{};input.kind=XR_SOURCE_INPUT_TEXT;input.value.text=text;
    XrSourceFailure failure{};
    CHECK(xray_source_build(&input,&options,&program,&failure,&diagnostic)==XR_SOURCE_OK && !diagnostic);
    std::memset(source, '!', sizeof(source));
    Output out{}; XrSourceCallOptions call_options{};
    CHECK(xray_source_call_options_init(&call_options, sizeof(call_options)) == XR_SOURCE_OK);
    call_options.write = capture; call_options.context = &out;
    XrSourceResult *result = nullptr;
    auto policy=xray_source_execution_options_default();XrSourceExecution *execution=nullptr;
    CHECK(xray_source_execution_new(&policy,&execution)==XR_SOURCE_OK);
    CHECK(xray_source_artifact_execute(execution,program,&call_options,&result,nullptr)==XR_SOURCE_OK);
    xray_source_artifact_free(program);xray_source_execution_free(execution);XrSourceEvent event{};
    CHECK(xray_source_result_event(result, &event) == XR_SOURCE_OK && event.state == XR_SOURCE_RUN_RETURNED);
    CHECK(out.size == 7 && !std::memcmp(out.bytes, "C++\0SDK", 7)); xray_source_result_free(result);
    std::puts("Installed C++ public Source SDK: detached ownership and NUL output PASS"); return 0;
}
