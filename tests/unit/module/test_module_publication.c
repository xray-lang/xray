/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */

#include "../test_framework.h"
#include "base/xmalloc.h"
#include "base/xfileio.h"
#include "os/os_temp.h"
#include "../test_win_compat.h"
#include <stdio.h>
#include "module/xmodule.h"
#include "module/xmodule_resolver.h"
#include "module/xstdlib_embedded.h"
#include "xray_vm.h"
#include "runtime/xisolate_api.h"
#include "runtime/xvm_call.h"
#include "runtime/core/xr_runtime_core.h"
#include "runtime/core/xr_exec_context.h"
#include "runtime/mem/xfixed_heap.h"
#include "runtime/mem/xcoro_heap.h"
#include "base/xhashmap.h"
#include "runtime/object/xstring.h"
#include "runtime/xexec_frame.h"
#include "runtime/value/xchunk.h"
#include "runtime/value/xtype.h"
#include "module/xmodule_identity.h"
#include "toolchain/xcompiler_session.h"
#include "ir/xi.h"
#include "vm/xvm.h"
#include <string.h>

TEST(module_exports_are_invisible_until_atomic_publication) {
    XrVMConfig config = {0};
    XrVMRuntime *isolate = xray_vm_new_full(&config);
    ASSERT_NOT_NULL(isolate);

    XrModule *module = xr_module_create_native(isolate, "publication_test");
    ASSERT_NOT_NULL(module);
    ASSERT_EQ_INT(xr_module_state(module), XR_MODULE_NEW);

    xr_module_add_export_sym(isolate, module, 10, xr_int(41), true);
    xr_module_add_export_sym(isolate, module, 11, xr_int(1), false);
    ASSERT_TRUE(XR_IS_NULL(xr_module_get_sym(module, 10)));
    ASSERT_FALSE(xr_module_has_sym(module, 10));

    ASSERT_TRUE(xr_module_begin_initialization(module));
    ASSERT_EQ_INT(xr_module_state(module), XR_MODULE_INITIALIZING);
    ASSERT_FALSE(xr_module_begin_initialization(module));
    ASSERT_TRUE(XR_IS_NULL(xr_module_get_sym(module, 10)));

    ASSERT_TRUE(xr_module_publish(module));
    ASSERT_EQ_INT(xr_module_state(module), XR_MODULE_PUBLISHED);
    ASSERT_EQ_INT(XR_TO_INT(xr_module_get_sym(module, 10)), 41);
    ASSERT_EQ_INT(XR_TO_INT(xr_module_get_sym(module, 11)), 1);
    ASSERT_TRUE(xr_module_is_const_sym(module, 10));
    ASSERT_FALSE(xr_module_is_const_sym(module, 11));

    ASSERT_FALSE(xr_module_set_sym(module, 10, xr_int(99)));
    ASSERT_TRUE(xr_module_set_sym(module, 11, xr_int(2)));
    ASSERT_EQ_INT(XR_TO_INT(xr_module_get_sym(module, 10)), 41);
    ASSERT_EQ_INT(XR_TO_INT(xr_module_get_sym(module, 11)), 2);

    xr_module_add_export_sym(isolate, module, 10, xr_int(99), true);
    ASSERT_EQ_INT(XR_TO_INT(xr_module_get_sym(module, 10)), 41);
    ASSERT_FALSE(xr_module_publish(module));

    XrValue text_value = xr_module_import(isolate, "text");
    ASSERT_TRUE(xr_value_is_module(text_value));
    XrValue lower = xr_module_get_export(isolate, xr_value_to_module(text_value), "lower");
    ASSERT_TRUE(xr_value_is_closure(lower) || xr_value_is_cfunction(lower));

    /* Public call behavior is stable whether the same Source function uses
     * its VM code or its verified native performance cache. */
    XrRuntimeCore *core = xr_isolate_get_runtime_core(isolate);
    XrExecutionContext *previous = xr_exec_context_enter(xr_runtime_core_root_exec(core));
    XrString *input = xr_string_new(isolate, "ABCD", 4);
    ASSERT_NOT_NULL(input);
    XrValue argument = xr_string_value(input);
    XrValue lowered;
    if (xr_value_is_closure(lower)) {
        lowered = xr_vm_call_closure(isolate, xr_value_to_closure(lower), &argument, 1);
    } else {
        XrCFunction *native = xr_value_to_cfunction(lower);
        ASSERT_NOT_NULL(native);
        ASSERT_FALSE(native->is_yieldable);
        lowered = native->as.func(isolate, &argument, 1);
    }
    ASSERT_TRUE(XR_IS_STRING(lowered));
    ASSERT_EQ_INT(XR_TO_STRING(lowered)->length, 4);
    ASSERT_EQ_INT(memcmp(XR_TO_STRING(lowered)->data, "abcd", 4), 0);
    xr_exec_context_restore(previous);

    XrValue base64_value = xr_module_import(isolate, "base64");
    ASSERT_TRUE(xr_value_is_module(base64_value));
    XrValue encode = xr_module_get_export(isolate, xr_value_to_module(base64_value), "encode");
    ASSERT_TRUE(xr_value_is_closure(encode) || xr_value_is_cfunction(encode));

#if defined(XR_HAS_DATA_FORMATS)
    XrValue csv_value = xr_module_import(isolate, "csv");
    ASSERT_TRUE(xr_value_is_module(csv_value));
    XrValue parse = xr_module_get_export(isolate, xr_value_to_module(csv_value), "parse");
    ASSERT_TRUE(xr_value_is_closure(parse) || xr_value_is_cfunction(parse));
#else
    ASSERT_TRUE(XR_IS_NULL(xr_module_import(isolate, "csv")));
#endif

#if defined(XR_HAS_FILESYSTEM)
    XrValue os_value = xr_module_import(isolate, "os");
    ASSERT_TRUE(xr_value_is_module(os_value));
    XrModule *os_module = xr_value_to_module(os_value);
    ASSERT_TRUE(XR_IS_NULL(xr_module_get_export(isolate, os_module, "sleep")));
    ASSERT_TRUE(XR_IS_NULL(xr_module_get_export(isolate, os_module, "__sleep")));
    ASSERT_TRUE(xr_value_is_cfunction(xr_module_get_export(isolate, os_module, "__getpid")));
#else
    ASSERT_TRUE(XR_IS_NULL(xr_module_import(isolate, "os")));
#endif

    xr_module_free(module);
    xray_vm_delete(isolate);
}

TEST(failed_module_never_publishes_partial_exports) {
    XrVMConfig config = {0};
    XrVMRuntime *isolate = xray_vm_new_full(&config);
    ASSERT_NOT_NULL(isolate);

    XrModule *module = xr_module_create_script(isolate, "failed_publication", "failed.xr");
    ASSERT_NOT_NULL(module);
    ASSERT_TRUE(xr_module_begin_initialization(module));
    xr_module_add_export_sym(isolate, module, 20, xr_int(7), true);

    xr_module_fail(module);
    ASSERT_EQ_INT(xr_module_state(module), XR_MODULE_FAILED);
    ASSERT_TRUE(XR_IS_NULL(xr_module_get_sym(module, 20)));
    ASSERT_FALSE(xr_module_has_sym(module, 20));
    ASSERT_FALSE(xr_module_publish(module));

    xr_module_free(module);
    xray_vm_delete(isolate);
}

TEST(declared_native_entry_binding_is_exact_and_one_shot) {
    XrVMConfig config = {0};
    XrVMRuntime *isolate = xray_vm_new_full(&config);
    ASSERT_NOT_NULL(isolate);

    XrModule *foreign = xr_module_create_native(isolate, "foreign");
    ASSERT_NOT_NULL(foreign);
    ASSERT_FALSE(xr_stdlib_module_install_native_entries(isolate, foreign, "os"));

    XrModule *source_only = xr_module_create_native(isolate, "base64");
    ASSERT_NOT_NULL(source_only);
    ASSERT_TRUE(xr_stdlib_module_install_native_entries(isolate, source_only, "base64"));
    ASSERT_TRUE(xr_module_begin_initialization(source_only));
    ASSERT_TRUE(xr_module_publish(source_only));
    ASSERT_FALSE(xr_stdlib_module_install_native_entries(isolate, source_only, "base64"));

#if defined(XR_HAS_FILESYSTEM)
    XrModule *os_module = xr_module_create_native(isolate, "os");
    ASSERT_NOT_NULL(os_module);
    ASSERT_TRUE(xr_stdlib_module_install_native_entries(isolate, os_module, "os"));
    ASSERT_TRUE(os_module->export_count > 0);
    ASSERT_FALSE(xr_stdlib_module_install_native_entries(isolate, os_module, "os"));
    xr_module_free(os_module);
#endif

    xr_module_free(source_only);
    xr_module_free(foreign);
    xray_vm_delete(isolate);
}

TEST(stdlib_resolver_follows_build_authority) {
    XrModuleResolverConfig config = {0};
    XrModuleResolver *resolver = xr_module_resolver_new(&config);
    ASSERT_NOT_NULL(resolver);

    XrModuleId text_id = {0};
    char *error = NULL;
    ASSERT_EQ_INT(xr_module_resolver_resolve(resolver, "text", NULL, NULL, &text_id, &error), 0);
    ASSERT_NULL(error);
    ASSERT_EQ_INT(text_id.kind, XR_MOD_STDLIB);
    ASSERT_STR_EQ(text_id.canonical, "stdlib-module-v1:module=4:text:path=12:text/text.xr");
    ASSERT_NULL(text_id.source_path);
    xr_module_id_cleanup(&text_id);

    XrModuleId csv_id = {0};
#if defined(XR_HAS_DATA_FORMATS)
    ASSERT_EQ_INT(xr_module_resolver_resolve(resolver, "csv", NULL, NULL, &csv_id, &error), 0);
    ASSERT_NULL(error);
    ASSERT_EQ_INT(csv_id.kind, XR_MOD_STDLIB);
    ASSERT_STR_EQ(csv_id.canonical, "stdlib-module-v1:module=3:csv:path=10:csv/csv.xr");
    ASSERT_NULL(csv_id.source_path);
    xr_module_id_cleanup(&csv_id);
#else
    ASSERT_EQ_INT(xr_module_resolver_resolve(resolver, "csv", NULL, NULL, &csv_id, &error), -1);
    ASSERT_NOT_NULL(error);
    xr_free(error);
    error = NULL;
#endif

#if !defined(XR_HAS_FILESYSTEM)
    XrModuleId os_id = {0};
    ASSERT_EQ_INT(xr_module_resolver_resolve(resolver, "os", NULL, NULL, &os_id, &error), -1);
    ASSERT_NOT_NULL(error);
    xr_free(error);
    error = NULL;
#endif
    xr_module_resolver_free(resolver);
}

TEST(source_module_context_detaches_metadata_before_first_export_call) {
    XrVMConfig config = {0};
    XrVMRuntime *isolate = xray_vm_new_full(&config);
    ASSERT_NOT_NULL(isolate);
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(isolate);
    XrModuleRegistry *registry = xr_isolate_get_module_registry(isolate);
    ASSERT_NOT_NULL(session);
    ASSERT_NOT_NULL(registry);
    const XrBytecodeModule *previous_image = registry->embedded_modules;
    size_t previous_count = registry->embedded_module_count;
    XrModule **previous_table = registry->module_table;
    int previous_table_count = registry->module_table_count;
    XrModuleIdentityAuthority authority = {
        .kind = XR_MODULE_IDENTITY_MEMORY, .namespace_id = "owned-source-module-metadata",
    };
    XrModuleSourceCompilation compilation = {0};
    const char *source = "export fn answer() -> i64 { return 42 }\n"
                         "fn pair() -> (i64, i64) { return (40, 2) }\n";
    ASSERT_TRUE(xr_compile_module_source(session, source, NULL, &authority, &compilation));
    ASSERT_NOT_NULL(compilation.initializer);
    ASSERT_NOT_NULL(compilation.context);
    ASSERT_NOT_NULL(compilation.dispose_context);
    XrModule *module = xr_module_create_script(isolate, "owned_source", "owned_source.xr");
    ASSERT_NOT_NULL(module);
    ASSERT_TRUE(xr_module_begin_initialization(module));
    module->initializer = compilation.initializer;
    XrModule *previous_module = xr_isolate_get_current_module(isolate);
    xr_isolate_set_current_module(isolate, module);
    ASSERT_EQ_INT(xr_vm_execute_module(isolate, module->initializer), 0);
    xr_isolate_set_current_module(isolate, previous_module);
    ASSERT_TRUE(compilation.dispose_context(compilation.context, true));
    ASSERT_NULL(xr_compiler_session_module_graph(session));
    ASSERT_TRUE(registry->embedded_modules == previous_image);
    ASSERT_EQ_INT(registry->embedded_module_count, previous_count);
    ASSERT_TRUE(registry->module_table == previous_table);
    ASSERT_EQ_INT(registry->module_table_count, previous_table_count);
    ASSERT_TRUE(xr_isolate_get_current_module(isolate) == previous_module);
    bool saw_tuple = false;
    for (int i = 0; i < PROTO_PROTO_COUNT(module->initializer); i++) {
        XrProto *child = PROTO_PROTO(module->initializer, i);
        ASSERT_NOT_NULL(child->xi_func);
        if (child->return_type_info) {
            ASSERT_TRUE(child->return_type_info == ((XiFunc *) child->xi_func)->return_type);
            if (child->return_type_info->kind == XR_KIND_TUPLE) {
                ASSERT_EQ_INT(child->return_type_info->tuple.element_count, 2);
                saw_tuple = true;
            }
        }
    }
    ASSERT_TRUE(saw_tuple);
    ASSERT_TRUE(xr_module_publish(module));
    XrValue answer = xr_module_get_export(isolate, module, "answer");
    ASSERT_TRUE(xr_value_is_closure(answer));
    XrRuntimeCore *core = xr_isolate_get_runtime_core(isolate);
    XrExecutionContext *previous = xr_exec_context_enter(xr_runtime_core_root_exec(core));
    XrValue value = xr_vm_call_closure(isolate, xr_value_to_closure(answer), NULL, 0);
    ASSERT_TRUE(XR_IS_INT(value));
    ASSERT_EQ_INT(XR_TO_INT(value), 42);
    xr_exec_context_restore(previous);
    xr_module_free(module);
    xray_vm_delete(isolate);
}

TEST(source_module_failure_never_publishes_a_partial_compiler_result) {
    XrVMConfig config = {0};
    XrVMRuntime *isolate = xray_vm_new_full(&config);
    ASSERT_NOT_NULL(isolate);
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(isolate);
    XrModuleIdentityAuthority authority = {
        .kind = XR_MODULE_IDENTITY_MEMORY, .namespace_id = "failed-source-module-metadata",
    };
    XrModuleSourceCompilation compilation = {0};
    ASSERT_FALSE(xr_compile_module_source(session, "fn missing( {", NULL, &authority, &compilation));
    ASSERT_NULL(compilation.initializer);
    ASSERT_NULL(compilation.context);
    ASSERT_NULL(compilation.dispose_context);
    ASSERT_NULL(xr_compiler_session_module_graph(session));
    ASSERT_TRUE(xr_compile_module_source(session, "fn recovered() -> i64 { return 42 }", NULL,
                                          &authority, &compilation));
    ASSERT_TRUE(compilation.dispose_context(compilation.context, true));
    xr_instruction_unit_free(compilation.initializer);
    xray_vm_delete(isolate);
}


static bool source_module_has_positional_imports(const XrProto *code) {
    for (int i = 0; i < PROTO_CODE_COUNT(code); ++i) {
        OpCode op = GET_OPCODE(PROTO_CODE(code, i));
        if (op == OP_LOAD_MODULE || op == OP_LOAD_MODULE_SLOT) return true;
    }
    for (int i = 0; i < PROTO_PROTO_COUNT(code); ++i)
        if (source_module_has_positional_imports(PROTO_PROTO(code, i))) return true;
    return false;
}

static bool write_source_module_fixture(const char *directory, const char *name,
                                         const char *content) {
    char path[1024];
    int length = snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (length < 0 || (size_t) length >= sizeof(path)) return false;
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    size_t bytes = strlen(content);
    bool written = fwrite(content, 1, bytes, file) == bytes;
    return fclose(file) == 0 && written;
}

static XrModule *publish_source_module_fixture(XrVMRuntime *isolate, const char *directory,
    const char *name, const char *source) {
    char path[1024];
    int length = snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (length < 0 || (size_t) length >= sizeof(path)) return NULL;
    XrModuleIdentityAuthority authority = {
        .kind = XR_MODULE_IDENTITY_SCRIPT, .physical_root = directory,
    };
    XrModuleSourceCompilation compilation = {0};
    if (!xr_compile_module_source(xr_compiler_session_current_for_isolate(isolate),
                                   source, path, &authority, &compilation)) return NULL;
    XrModule *module = xr_module_create_script(isolate, name, path);
    if (!module || !xr_module_begin_initialization(module)) {
        xr_instruction_unit_free(compilation.initializer);
        (void) compilation.dispose_context(compilation.context, false);
        xr_module_free(module);
        return NULL;
    }
    module->initializer = compilation.initializer;
    XrModule *previous = xr_isolate_get_current_module(isolate);
    xr_isolate_set_current_module(isolate, module);
    int executed = xr_vm_execute_module(isolate, module->initializer);
    xr_isolate_set_current_module(isolate, previous);
    bool disposed = compilation.dispose_context(compilation.context, executed == 0);
    if (executed != 0 || !disposed || !xr_module_publish(module)) {
        xr_module_free(module);
        return NULL;
    }
    return module;
}

TEST(source_module_exports_ignore_an_unrelated_program_module_table) {
    char directory[512];
    ASSERT_EQ_INT(xr_temp_dir_create("xray-source-module-binding", directory, sizeof(directory)), 0);
    char *canonical = xr_realpath(directory);
    ASSERT_NOT_NULL(canonical);
    const char *source_a = "import { value } from \"./leaf_a\"\n"
        "export fn answer() -> i64 { return value() + 1 }\n";
    const char *source_b = "import { value } from \"./leaf_b\"\n"
        "export fn answer() -> i64 { return value() + 1 }\n";
    ASSERT_TRUE(write_source_module_fixture(canonical, "leaf_a.xr",
        "export fn value() -> i64 { return 41 }\n"));
    ASSERT_TRUE(write_source_module_fixture(canonical, "leaf_b.xr",
        "export fn value() -> i64 { return 99 }\n"));
    ASSERT_TRUE(write_source_module_fixture(canonical, "root_a.xr", source_a));
    ASSERT_TRUE(write_source_module_fixture(canonical, "root_b.xr", source_b));
    XrVMConfig config = {0};
    XrVMRuntime *isolate = xray_vm_new_full(&config);
    ASSERT_NOT_NULL(isolate);
    XrModuleRegistry *registry = xr_isolate_get_module_registry(isolate);
    XrModule *module_a = publish_source_module_fixture(isolate, canonical, "root_a.xr", source_a);
    XrModule *module_b = publish_source_module_fixture(isolate, canonical, "root_b.xr", source_b);
    ASSERT_NOT_NULL(module_a);
    ASSERT_NOT_NULL(module_b);
    ASSERT_FALSE(source_module_has_positional_imports(module_a->initializer));
    ASSERT_FALSE(source_module_has_positional_imports(module_b->initializer));
    ASSERT_NULL(xr_compiler_session_module_graph(xr_compiler_session_current_for_isolate(isolate)));
    ASSERT_NULL(registry->module_table);
    char leaf_path[1024];
    snprintf(leaf_path, sizeof(leaf_path), "%s/leaf_b.xr", canonical);
    char *leaf_canonical = xr_realpath(leaf_path);
    ASSERT_NOT_NULL(leaf_canonical);
    XrValue leaf_b = xr_module_import(isolate, leaf_canonical);
    xr_free(leaf_canonical);
    ASSERT_TRUE(xr_value_is_module(leaf_b));
    XrModule *unrelated_table[] = {xr_value_to_module(leaf_b), module_b};
    registry->module_table = unrelated_table;
    registry->module_table_count = 2;
    XrValue answer_a = xr_module_get_export(isolate, module_a, "answer");
    XrValue answer_b = xr_module_get_export(isolate, module_b, "answer");
    ASSERT_TRUE(xr_value_is_closure(answer_a));
    ASSERT_TRUE(xr_value_is_closure(answer_b));
    XrExecutionContext *previous = xr_exec_context_enter(
        xr_runtime_core_root_exec(xr_isolate_get_runtime_core(isolate)));
    XrValue value_a = xr_vm_call_closure(isolate, xr_value_to_closure(answer_a), NULL, 0);
    XrValue value_b = xr_vm_call_closure(isolate, xr_value_to_closure(answer_b), NULL, 0);
    xr_exec_context_restore(previous);
    registry->module_table = NULL;
    registry->module_table_count = 0;
    ASSERT_TRUE(XR_IS_INT(value_a));
    ASSERT_EQ_INT(XR_TO_INT(value_a), 42);
    ASSERT_TRUE(XR_IS_INT(value_b));
    ASSERT_EQ_INT(XR_TO_INT(value_b), 100);
    xr_module_free(module_a);
    xr_module_free(module_b);
    xray_vm_delete(isolate);
    const char *files[] = {"leaf_a.xr", "leaf_b.xr", "root_a.xr", "root_b.xr"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", canonical, files[i]);
        ASSERT_EQ_INT(remove(path), 0);
    }
    ASSERT_EQ_INT(xr_test_rmdir(canonical), 0);
    xr_free(canonical);
}


typedef struct SourceModuleLifetimeObject {
    XrObjHeader header;
    XrModule *module;
    XrVMRuntime *isolate;
} SourceModuleLifetimeObject;

static int lifetime_finalizers, lifetime_code_reads, lifetime_handle_drops, lifetime_errors;

static void source_module_lifetime_destroy(XrObjHeader *object, XrCoroHeap *heap) {
    SourceModuleLifetimeObject *body = (SourceModuleLifetimeObject *) object;
    (void) heap;
    ++lifetime_finalizers;
    if (!xr_isolate_get_module_registry(body->isolate) ||
        !xr_compiler_session_current_for_isolate(body->isolate) ||
        !body->module->initializer) {
        ++lifetime_errors;
        return;
    }
    XrProto *initializer = body->module->initializer;
    if (PROTO_PROTO_COUNT(initializer) != 1 ||
        !PROTO_PROTO(initializer, 0)->xi_func ||
        !PROTO_PROTO(initializer, 0)->return_type_info ||
        PROTO_PROTO(initializer, 0)->return_type_info->kind != XR_KIND_INT) {
        ++lifetime_errors;
        return;
    }
    ++lifetime_code_reads;
}

static void source_module_lifetime_handle_destroy(void *handle) {
    XrModule *module = (XrModule *) handle;
    ++lifetime_handle_drops;
    if (lifetime_finalizers != 2 || lifetime_code_reads != 2 || !module->initializer)
        ++lifetime_errors;
}

TEST(source_module_code_outlives_fixed_and_root_finalizers) {
    lifetime_finalizers = lifetime_code_reads = lifetime_handle_drops = lifetime_errors = 0;
    XrVMConfig config = {0};
    XrVMRuntime *isolate = xray_vm_new_full(&config);
    ASSERT_NOT_NULL(isolate);
    XrModuleIdentityAuthority authority = {
        .kind = XR_MODULE_IDENTITY_MEMORY, .namespace_id = "source-module-finalization",
    };
    XrModuleSourceCompilation compilation = {0};
    ASSERT_TRUE(xr_compile_module_source(xr_compiler_session_current_for_isolate(isolate),
        "export fn answer() -> i64 { return 42 }\n", NULL, &authority, &compilation));
    XrModule *module = xr_module_create_script(isolate, "finalization", "finalization.xr");
    ASSERT_NOT_NULL(module);
    ASSERT_TRUE(xr_module_begin_initialization(module));
    module->initializer = compilation.initializer;
    XrModule *previous_module = xr_isolate_get_current_module(isolate);
    xr_isolate_set_current_module(isolate, module);
    ASSERT_EQ_INT(xr_vm_execute_module(isolate, module->initializer), 0);
    xr_isolate_set_current_module(isolate, previous_module);
    ASSERT_TRUE(compilation.dispose_context(compilation.context, true));
    ASSERT_TRUE(xr_module_publish(module));
    XrModuleRegistry *registry = xr_isolate_get_module_registry(isolate);
    ASSERT_TRUE(xr_hashmap_set(registry->loaded_modules, module->path, module));
    module->native_handle = module;
    module->native_handle_destroy = source_module_lifetime_handle_destroy;
    XrRuntimeCore *core = xr_isolate_get_runtime_core(isolate);
    xr_runtime_core_set_destroy_op(core, 62, source_module_lifetime_destroy);
    xr_runtime_core_set_destroy_op(core, 63, source_module_lifetime_destroy);
    SourceModuleLifetimeObject *fixed = (SourceModuleLifetimeObject *)
        xr_fixed_heap_new_obj(&core->fixed_heap, 62, sizeof(*fixed));
    SourceModuleLifetimeObject *root_object = (SourceModuleLifetimeObject *)
        xr_coro_heap_new_obj(&core->root_heap, 63, sizeof(*root_object));
    ASSERT_NOT_NULL(fixed);
    ASSERT_NOT_NULL(root_object);
    fixed->module = root_object->module = module;
    fixed->isolate = root_object->isolate = isolate;
    xray_vm_delete(isolate);
    ASSERT_EQ_INT(lifetime_finalizers, 2);
    ASSERT_EQ_INT(lifetime_code_reads, 2);
    ASSERT_EQ_INT(lifetime_handle_drops, 1);
    ASSERT_EQ_INT(lifetime_errors, 0);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Module publication");
RUN_TEST(module_exports_are_invisible_until_atomic_publication);
RUN_TEST(failed_module_never_publishes_partial_exports);
RUN_TEST(declared_native_entry_binding_is_exact_and_one_shot);
RUN_TEST(stdlib_resolver_follows_build_authority);
RUN_TEST(source_module_context_detaches_metadata_before_first_export_call);
RUN_TEST(source_module_failure_never_publishes_a_partial_compiler_result);
RUN_TEST(source_module_exports_ignore_an_unrelated_program_module_table);
RUN_TEST(source_module_code_outlives_fixed_and_root_finalizers);
TEST_MAIN_END()
