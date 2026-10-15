/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_hash_source.c - Source hash constraints, shadowing and carrier execution
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "p09_hash_source.h"
#include "p09_hash_source_program.h"
static void hash_source_cases(const XrXirCompileContext *context) {
    static const struct { const char *source; XrXirStatus status; } cases[] = {
        {"fn echo<K:Hashable,V>(x:Map<K,V>)->Map<K,V>{return x}\nconst f=echo<i64,string>\n", XR_XIR_OK},
        {"fn echo<T:Hashable>(x:Set<T>)->Set<T>{return x}\nconst f=echo<bool>\n", XR_XIR_OK},
        {"fn echo<V>(x:Map<string,V>)->Map<string,V>{return x}\nconst f=echo<i64>\n", XR_XIR_OK},
        {"fn echo(x:Map<i64,Map<string,Array<i64>>>)->Map<i64,Map<string,Array<i64>>>{return x}\n", XR_XIR_OK},
        {"fn echo(x:Map<string,()>)->Map<string,()>{return x}\n", XR_XIR_OK},
        {"struct Map<K,V>{}\nfn echo(x:Map<Array<i64>,i64>)->Map<Array<i64>,i64>{return x}\n", XR_XIR_OK},
        {"struct Set<T>{}\nfn echo(x:Set<Array<i64>>)->Set<Array<i64>>{return x}\n", XR_XIR_OK},
        {"fn echo<K,V>(x:Map<K,V>)->Map<K,V>{return x}\n", XR_XIR_BAD_TYPE},
        {"fn echo<K:Equal,V>(x:Map<K,V>)->Map<K,V>{return x}\n", XR_XIR_BAD_TYPE},
        {"interface Hashable{}\nfn echo<K:Hashable,V>(x:Map<K,V>)->Map<K,V>{return x}\n", XR_XIR_BAD_TYPE},
        {"fn echo(x:Map<Array<i64>,i64>)->i64{return 0}\n", XR_XIR_BAD_TYPE},
        {"fn echo(x:Set<Array<i64>>)->i64{return 0}\n", XR_XIR_BAD_TYPE},
        {"fn echo(x:Map<i64>)->i64{return 0}\n", XR_XIR_BAD_TYPE},
        {"fn echo(x:Map<i64,i64,i64>)->i64{return 0}\n", XR_XIR_BAD_TYPE},
        {"fn echo(x:Set<i64,i64>)->i64{return 0}\n", XR_XIR_BAD_TYPE},
        {"fn echo<Map>(x:Map<i64,i64>)->i64{return 0}\n", XR_XIR_BAD_STRUCTURE},
        {"fn echo<Map>(x:Map)->Map{return x}\nconst f=echo<i64>\n", XR_XIR_OK},
        {"fn echo<K:Hashable,V>(x:Map<K,Array<V>>)->Map<K,Array<V>>{return x}\nconst f=echo<string,i64>\n", XR_XIR_OK},
        {"fn missing(x:Map<string,i64>)->i64?{return x.get(\"key\")}\n", XR_XIR_BAD_TYPE},
        {"fn missing(x:Set<i64>)->bool{return x.contains(1)}\n", XR_XIR_BAD_TYPE},
    };
    unsigned failures = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        XrXirSourceResult result = {0};
        XrXirStatus status = hash_source_check(context, cases[i].source, &result);
        if (status != cases[i].status) {
            fprintf(stderr, "hash source case=%u expected=%u actual=%u\n", i, cases[i].status, status); ++failures;
        }
        if (status == XR_XIR_OK && (i == 5 || i == 6)) {
            const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(result.snapshot);
            for (uint32_t d = 0; d < view->declaration_count; ++d)
                CHECK(view->declarations[d].kind != XR_XIR_SOURCE_TYPE ||
                    (view->declarations[d].native_identity != 6 && view->declarations[d].native_identity != 7));
        }
        if (status == XR_XIR_OK && cases[i].status == XR_XIR_OK) {
            XrXirArtifact *lowered = hash_source_replay(context, &result);
            xr_xir_compile_artifact_free(lowered);
        }
        xr_xir_compile_source_result_free(&result);
    }
    CHECK(!failures);
}
int main(int argc, char **argv) {
    SourceFixtureOwner owner = {0}; source_fixture_owner_new(&owner);
    XrXirArtifact *lowered = hash_source_lower(&owner.context);
    uint32_t entries[4]; hash_source_entries(xr_xir_compile_artifact_module(lowered), entries);
    if (argc == 2) {
        XrXirCSource source = {0};
        CHECK(xr_xir_compile_emit_c(lowered, "p09_hash_source", 1048576, &source) == XR_XIR_OK);
        CHECK(!strstr(source.text, "({")); FILE *file = fopen(argv[1], "wb"); CHECK(file);
        CHECK(fwrite(source.text, 1, source.length, file) == source.length);
        CHECK(fprintf(file, "\nconst uint32_t p09_hash_source_function_ids[4] = {%u,%u,%u,%u};\n",
            entries[0], entries[1], entries[2], entries[3]) > 0 && !fclose(file));
        printf("Source Map/Set generated portable C11 bytes=%zu\n", source.length);
        xr_xir_compile_c_source_free(&source); xr_xir_compile_artifact_free(lowered);
    } else {
        XrXirProgram *program = NULL;
        CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK && !lowered);
        hash_source_program_run(program, entries[1], entries[3]); hash_source_cases(&owner.context);
    }
    source_fixture_owner_free(&owner);
    puts("Source Map/Set type facts, Checked replay and owned carrier PASS"); return 0;
}
