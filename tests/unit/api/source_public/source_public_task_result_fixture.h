/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_public_task_result_fixture.h - Shared real Source outcome producers
 */
#ifndef SOURCE_PUBLIC_TASK_RESULT_FIXTURE_H
#define SOURCE_PUBLIC_TASK_RESULT_FIXTURE_H
static const char task_result_source[]=
    "enum Failure{Bad}\n"
    "fn number()->i64{return 42}\nfn text()->string{return \"A\\0中\"}\n"
    "fn array()->Array<string>{return [\"A\\0中\",\"next\"]}\n"
    "fn unit()->(){return ()}\nfn nested()->Task<i64>{const t=go number();await t;return t}\n"
    "fn broken()->i64{throw Failure.Bad}\nfn panicValue()->i64{assert(false,\"panic\\0中\");return 0}\n"
    "fn slow()->i64{while(true){Coro.yield()};return 0}\n"
    "fn allocating()->Array<string>{Coro.yield();return [\"terminal\"]}\n"
    "export fn numberTask()->Task<i64>{const t=go number();await t;return t}\n"
    "export fn textTask()->Task<string>{const t=go text();await t;return t}\n"
    "export fn arrayTask()->Task<Array<string>>{const t=go array();await t;return t}\n"
    "export fn unitTask()->Task<()>{const t=go unit();await t;return t}\n"
    "export fn nestedTask()->Task<Task<i64>>{const t=go nested();await t;return t}\n"
    "export fn errorTask()->Task<i64>{const t=go broken();try{await t}catch(e){};return t}\n"
    "export fn panicTask()->Task<i64>{const t=go panicValue();try{await t}catch panic(p){};return t}\n"
    "export fn pendingTask()->Task<i64>{return go slow()}\n"
    "export fn allocatingTask()->Task<Array<string>>{return go allocating()}\n";
#endif // SOURCE_PUBLIC_TASK_RESULT_FIXTURE_H
