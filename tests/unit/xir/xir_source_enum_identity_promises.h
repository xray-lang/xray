/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_enum_identity_promises.h - Tag-only effects and receiver evaluation
 *
 * KEY CONCEPT:
 *   Unread payload callbacks add no effect; evaluating the receiver still does.
 */
#ifndef XIR_SOURCE_ENUM_IDENTITY_PROMISES_H
#define XIR_SOURCE_ENUM_IDENTITY_PROMISES_H
static void source_enum_identity_promises(const XrXirSourceRequest *request) {
    char path[8192]; source_fixture_path(request,"witness_promises.xr",path);
    XrXirSourceRequest local=*request;local.entry_path=path;
    witness_promise_file(&local,"witness_promises.xr",
        "enum E<T>{Full{value:T}}\n"
        "fn callback()->string{Coro.yield();return \"never\"}\n"
        "fn text<T>(value:E<T>)->string{return value.toString()}\n"
        "export fn answer()->i64{const name=text<fn()->string>(E<fn()->string>.Full{value:callback});if(name==\"E.Full\"){return 41}else{return 0}}\n");
    SourceTestDeclaration record={"text",NULL,NULL,true};
    source_manifest_write(&local,"witness_promises.xr",&record,1);
    witness_promise_execute(witness_promise_check(&local,true,"tag-only enum identity effect",NULL),
        "enum_identity_effect",NULL);
    witness_promise_file(&local,"witness_promises.xr",
        "enum E{Unit}\n"
        "fn make()->E{Coro.yield();return E.Unit}\n"
        "fn text()->string{return make().toString()}\n"
        "export fn answer()->i64{return 41}\n");
    CHECK(!witness_promise_check(&local,false,"enum receiver effects remain visible","declared no_suspend"));
    source_manifest_raw(&local,""); CHECK(xr_test_unlink(path)==0);
}
#endif // XIR_SOURCE_ENUM_IDENTITY_PROMISES_H
