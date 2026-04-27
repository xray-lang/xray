/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xisolate_scripting.c - Script execution API (dostring, dofile)
 *
 * KEY CONCEPT:
 *   Functions that require the compiler/parser are isolated here so that
 *   bytecode-only executables (xray build) never link this .o file.
 *
 * RELATED MODULES:
 *   - xray_isolate.c: core lifecycle (new/delete)
 *   - xray_isolate_tls.c: thread-local storage
 */

#include "../runtime/xisolate_internal.h"
#include "../runtime/xisolate_api.h"
#include "../base/xchecks.h"
#include "../frontend/parser/xparse.h"
#include "../frontend/parser/xast.h"
#include "../runtime/object/xstring.h"
#include "../runtime/object/xarray.h"
#include "../runtime/class/xinstance.h"
#include "../runtime/class/xclass_system.h"
#include "../base/xglobal_indices.h"
#include "../runtime/value/xvalue.h"
#include "../vm/xic_method.h"
#include "../vm/xvm_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "../os/os_fs.h"

#include "../base/xmalloc.h"
#include "../vm/xdebug.h"
#include "../base/xsource_cache.h"

/* ========== IC Feedback Dump ========== */

// Recursively dump IC type feedback for a proto tree, reading the
// per-coroutine IC tables on the resolved VM context (IC state lives
// ctx-side now, not on the immutable proto).
static void dump_ic_feedback_recursive(XrVMContext *ctx, XrProto *proto) {
    if (!proto || !ctx)
        return;

    const char *name = proto->name ? (const char *) proto->name->data : "<script>";
    XrICMethodTable *icm = xr_vm_ctx_get_ic_methods(ctx, proto);
    if (icm) {
        xr_ic_method_table_dump_feedback(icm, name);
    }

    // Recurse into nested functions
    int nprotos = PROTO_PROTO_COUNT(proto);
    for (int i = 0; i < nprotos; i++) {
        dump_ic_feedback_recursive(ctx, PROTO_PROTO(proto, i));
    }
}

// Common execute + optional dump logic shared by dostring/dofile
static int execute_and_dump(XrayIsolate *isolate, XrProto *code, const char *label) {
    if (isolate->params.dump_bytecode) {
        xr_disassemble_proto(code, label);
    }
    int result = xr_execute(isolate, code);
    if (isolate->params.dump_ic_feedback) {
        fprintf(stderr, "\n========== IC Type Feedback ==========\n");
        dump_ic_feedback_recursive(xr_vm_current_ctx(isolate), code);
        fprintf(stderr, "======================================\n");
    }
    return result;
}

/* ========== File Reading Helpers ========== */

static char *read_file_source(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        return NULL;
    }

    long size = -1;
    if (fseek(file, 0, SEEK_END) == 0) {
        size = ftell(file);
        fseek(file, 0, SEEK_SET);
    }

    char *source = NULL;
    size_t read_size = 0;

    if (size > 0) {
        source = (char *) xr_malloc(size + 1);
        if (source == NULL) {
            fclose(file);
            return NULL;
        }
        read_size = fread(source, 1, size, file);
        source[read_size] = '\0';
    } else {
        // Pipe/stdin fallback: read in 4KB blocks
        size_t capacity = 4096;
        source = (char *) xr_malloc(capacity);
        if (source == NULL) {
            fclose(file);
            return NULL;
        }
        size_t n;
        while ((n = fread(source + read_size, 1, capacity - read_size - 1, file)) > 0) {
            read_size += n;
            if (read_size + 1 >= capacity) {
                capacity *= 2;
                char *new_source = (char *) xr_realloc(source, capacity);
                if (new_source == NULL) {
                    xr_free(source);
                    fclose(file);
                    return NULL;
                }
                source = new_source;
            }
        }
        source[read_size] = '\0';
    }

    fclose(file);
    return source;
}

/* ========== Execution API ========== */

int xray_isolate_dostring(XrayIsolate *isolate, const char *source) {
    xray_api_checkr(isolate != NULL, "xray_isolate_dostring: NULL isolate", -1);
    xray_api_checkr(source != NULL, "xray_isolate_dostring: NULL source", -1);

    AstNode *ast = xr_parse(isolate, source);
    if (ast == NULL) {
        fprintf(stderr, "Parse error\n");
        return -1;
    }

    XrProto *code = xr_compile_ast(isolate, ast);
    if (code == NULL) {
        fprintf(stderr, "Compilation error\n");
        xr_program_destroy(ast);
        return -1;
    }

    int result = execute_and_dump(isolate, code, "<eval>");

    xr_free_code(isolate, code);
    xr_program_destroy(ast);

    return result;
}

int xray_isolate_dofile(XrayIsolate *isolate, const char *filename) {
    xray_api_checkr(isolate != NULL, "xray_isolate_dofile: NULL isolate", -1);
    xray_api_checkr(filename != NULL, "xray_isolate_dofile: NULL filename", -1);

    char *source = read_file_source(filename);
    if (source == NULL) {
        fprintf(stderr, "Cannot open file: %s\n", filename);
        return -1;
    }

    if (isolate->source_cache) {
        xr_source_cache_add(isolate->source_cache, filename, source);
    }

    AstNode *ast = xr_parse_with_source(isolate, source, filename);
    if (ast == NULL) {
        xr_free(source);
        return -1;
    }

    XrProto *code = xr_compile_ast_with_source(isolate, ast, filename);
    if (code == NULL) {
        xr_program_destroy(ast);
        xr_free(source);
        return -1;
    }

    int result = execute_and_dump(isolate, code, filename);

    xr_free_code(isolate, code);
    xr_program_destroy(ast);
    xr_free(source);

    return result;
}

// Debug version: compile and execute but don't free code (for debug resume)
// Returns proto via out_proto, caller must free with xr_free_code when done
int xray_isolate_dofile_debug(XrayIsolate *isolate, const char *filename, void **out_proto) {
    xray_api_checkr(isolate != NULL, "xray_isolate_dofile_debug: NULL isolate", -1);
    xray_api_checkr(filename != NULL, "xray_isolate_dofile_debug: NULL filename", -1);

    char *source = read_file_source(filename);
    if (source == NULL) {
        fprintf(stderr, "Cannot open file: %s\n", filename);
        return -1;
    }

    if (isolate->source_cache) {
        xr_source_cache_add(isolate->source_cache, filename, source);
    }

    AstNode *ast = xr_parse_with_source(isolate, source, filename);
    if (ast == NULL) {
        xr_free(source);
        return -1;
    }

    XrProto *code = xr_compile_ast_with_source(isolate, ast, filename);
    if (code == NULL) {
        xr_program_destroy(ast);
        xr_free(source);
        return -1;
    }

    int result = xr_execute(isolate, code);

    if (out_proto) {
        *out_proto = code;
    }

    xr_program_destroy(ast);
    xr_free(source);

    return result;
}

/* ========== Script Info ========== */

void xray_isolate_set_script_info(XrayIsolate *isolate, const char *script_file, int argc,
                                  char **argv) {
    if (isolate == NULL)
        return;

    isolate->params.script_file = script_file;
    isolate->params.script_argc = argc;
    isolate->params.script_argv = argv;

    char abs_path[PATH_MAX];
    char dir_path[PATH_MAX];
    XrString *main_str = NULL;
    XrString *dir_str = NULL;

    if (script_file && xr_fs_realpath(script_file, abs_path, sizeof(abs_path))) {
        main_str = xr_string_intern(isolate, abs_path, strlen(abs_path), 0);

        snprintf(dir_path, PATH_MAX, "%s", abs_path);
        char *last_slash = strrchr(dir_path, '/');
        if (last_slash) {
            *last_slash = '\0';
            dir_str = xr_string_intern(isolate, dir_path, strlen(dir_path), 0);
        } else if (xr_fs_getcwd(dir_path, PATH_MAX)) {
            dir_str = xr_string_intern(isolate, dir_path, strlen(dir_path), 0);
        }
    } else if (script_file) {
        main_str = xr_string_intern(isolate, script_file, strlen(script_file), 0);
    }

    XrArray *args_array = xr_array_new(xr_current_coro(isolate));
    for (int i = 0; i < argc; i++) {
        XrString *arg_str = xr_string_intern(isolate, argv[i], strlen(argv[i]), 0);
        xr_array_push(args_array, xr_string_value(arg_str));
    }

    // Create process singleton with file, args, dir fields
    if (isolate->core && isolate->core->processClass) {
        XrInstance *process = xr_instance_new(isolate, isolate->core->processClass);
        if (process) {
            xr_instance_set_field_fast(process, 0,
                                       main_str ? xr_string_value(main_str) : xr_null());
            xr_instance_set_field_fast(process, 1, xr_value_from_array(args_array));
            xr_instance_set_field_fast(process, 2, dir_str ? xr_string_value(dir_str) : xr_null());

            isolate->vm.builtins[XR_GLOBAL_VAR_PROCESS] = xr_value_from_instance(process);
        }
    }

    // Module-level __file__ and __dir__
    isolate->vm.builtins[XR_GLOBAL_VAR_FILE] = main_str ? xr_string_value(main_str) : xr_null();
    isolate->vm.builtins[XR_GLOBAL_VAR_DIR] = dir_str ? xr_string_value(dir_str) : xr_null();

    if (isolate->vm.builtin_count < XR_USER_GLOBALS_START) {
        isolate->vm.builtin_count = XR_USER_GLOBALS_START;
    }
}
