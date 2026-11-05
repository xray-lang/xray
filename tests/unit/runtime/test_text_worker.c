/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_text_worker.c - Private text worker ownership and boundary rejection
 */
#include "../test_framework.h"
#include "../test_helper.h"
#include "runtime/object/xtext_worker.h"
#include "runtime/object/xnative_type.h"
#include "runtime/class/xclass_builder.h"
#include "runtime/class/xinstance.h"
#include "runtime/object/xarray.h"
#include "runtime/object/xstring.h"
#include "runtime/value/xvalue_format.h"
#include "runtime/value/xtype.h"
#include "runtime/mem/xcoro_heap.h"
#include "coro/xdeep_copy.h"
#include "vm/xvm.h"
#include "vm/xvm_worker_state.h"
#include "vm/xvm_coro_state.h"
#include "coro/xworker_internal.h"
#include "shared/xr_print_plan.h"
#include <string.h>

static void free_test_proto(XrProto *proto) {
    if (!proto) return;
    /* Diagnostic labels below are static and borrowed by the test proto. */
    proto->source_file = NULL;
    xr_instruction_unit_free(proto);
}
static XrCoroutine *new_test_coro(XrVMRuntime *vm, XrProto **owned_proto) {
    xr_exec_context_enter(xr_runtime_core_root_exec(vm->core_rt));
    *owned_proto = xr_instruction_unit_new();
    if (!*owned_proto) return NULL;
    (*owned_proto)->maxstacksize = 8;
    xr_instruction_unit_write(*owned_proto, CREATE_ABC(OP_RETURN0, 0, 0, 0), 1);
    XrClosure *closure = xr_closure_new(vm, *owned_proto, NULL);
    if (!closure) return NULL;
    XrCoroutine *coro = xr_coro_create_vm_closure(vm, closure, NULL, NULL, 0, "private-test", "<test>", 0);
    if (!coro) return NULL;
    xr_coro_flags_set(coro, XR_CORO_FLG_MAIN);
    (void) xr_coro_reset_execution_state(coro, vm);
    vm->main_coro = coro;
    xr_exec_context_enter(&coro->exec_ctx);
    return coro;
}
static void extension_destroy(XrObjHeader *object, void *heap) {
    (void) object;
    (void) heap;
}
static void extension_traverse(void *heap, XrObjHeader *object) {
    (void) heap;
    (void) object;
}
TEST(extension_interval) {
    XrVMConfig config = {0};
    XrVMRuntime *vm = xray_vm_new_full(&config);
    ASSERT_NOT_NULL(vm);
    xr_exec_context_enter(xr_runtime_core_root_exec(vm->core_rt));
    XrObjDestroyFn original = xr_runtime_core_destroy_op(vm->core_rt, XR_TTEXT_WORKER);
    ASSERT_TRUE(original == xr_obj_destroy_text_worker);
    const uint8_t invalid[] = {XR_TINSTANCE, XR_TTEXT_WORKER, 36u, 63u, 64u};
    for (size_t i = 0; i < sizeof(invalid); ++i) {
        xr_register_extension_destroy(vm, invalid[i], extension_destroy);
        xr_register_extension_traverse(vm, invalid[i], extension_traverse);
        ASSERT_TRUE(xr_isolate_get_ext_destroy(vm, invalid[i]) == NULL);
        ASSERT_TRUE(xr_isolate_get_ext_traverse(vm, invalid[i]) == NULL);
    }
    ASSERT_TRUE(xr_runtime_core_destroy_op(vm->core_rt, XR_TTEXT_WORKER) == original);
    ASSERT_EQ_UINT(xr_alloc_extension_type(vm, "test-extension"), 36u);
    xr_register_extension_destroy(vm, 36u, extension_destroy);
    xr_register_extension_traverse(vm, 36u, extension_traverse);
    ASSERT_TRUE(xr_isolate_get_ext_destroy(vm, 36u) == extension_destroy);
    ASSERT_TRUE(xr_isolate_get_ext_traverse(vm, 36u) == extension_traverse);
    ASSERT_EQ_UINT(xr_isolate_get_ext_finalize_bitmap(vm), UINT64_C(1) << 36u);
    ASSERT_EQ_UINT(xr_isolate_get_ext_has_refs_bitmap(vm), UINT64_C(1) << 36u);
    XrNativeTypeInfo info = {0};
    info.name = "OrdinaryName";
    info.gc_type = XR_TTEXT_WORKER;
    ASSERT_TRUE(xr_register_native_type(vm, &info) == NULL);
    ASSERT_TRUE(xr_get_native_type_class(vm, XR_TTEXT_WORKER) == NULL);
    xray_vm_delete(vm);
}
TEST(real_finish_close_and_guards) {
    XrVMConfig config = {0};
    XrVMRuntime *vm = xray_vm_new_full(&config);
    ASSERT_NOT_NULL(vm);
    xr_exec_context_enter(xr_runtime_core_root_exec(vm->core_rt));
    XrCoroHeap *heap = &vm->core_rt->root_heap;
    uint32_t before_objects = heap->object_count;
    uint32_t before_finalizers = heap->finalizer_count;
    XrValue worker = xr_null();
    ASSERT_EQ_INT(xr_text_worker_new(vm, XR_TEXT_WORKER_CONCAT, &worker), XR_TEXT_WORKER_OK);
    ASSERT_EQ_UINT(heap->object_count, before_objects + 1u);
    ASSERT_EQ_INT(xr_value_typeid(worker), UINT8_MAX);
    ASSERT_FALSE(xr_value_in_json_domain(worker));
    ASSERT_TRUE(xr_value_to_string(vm, worker) == NULL);
    ASSERT_TRUE(XR_IS_NULL(xr_deep_copy(vm, worker, NULL)));
    XrTextWorkerView view = {0};
    XrObjHeader legacy = {0};
    legacy.type = 35u;
    legacy.objsize = (uint32_t) sizeof(legacy);
    ASSERT_EQ_INT(xr_text_worker_view(vm, xr_make_typed_ptr_val(&legacy, 35u),
                                      XR_TEXT_WORKER_CONCAT, &view), XR_TEXT_WORKER_INVALID);
    ASSERT_EQ_INT(xr_text_worker_view(vm, worker, XR_TEXT_WORKER_PRINT, &view), XR_TEXT_WORKER_INVALID);
    XrObjHeader *header = (XrObjHeader *) worker.ptr;
    uint32_t extent = header->objsize;
    header->objsize = (uint32_t) sizeof(XrObjHeader);
    ASSERT_EQ_INT(xr_text_worker_view(vm, worker, XR_TEXT_WORKER_CONCAT, &view), XR_TEXT_WORKER_INVALID);
    header->objsize = extent;
    uint8_t kind = header->type;
    header->type = XR_TBLOB;
    ASSERT_EQ_INT(xr_text_worker_view(vm, worker, XR_TEXT_WORKER_CONCAT, &view), XR_TEXT_WORKER_INVALID);
    header->type = kind;
    XrVMRuntime *other = xray_vm_new_full(&config);
    ASSERT_NOT_NULL(other);
    xr_exec_context_enter(xr_runtime_core_root_exec(other->core_rt));
    ASSERT_EQ_INT(xr_text_worker_view(other, worker, XR_TEXT_WORKER_CONCAT, &view), XR_TEXT_WORKER_INVALID);
    ASSERT_EQ_INT(xr_text_worker_view(vm, worker, XR_TEXT_WORKER_CONCAT, &view), XR_TEXT_WORKER_INVALID);
    xray_vm_delete(other);
    xr_exec_context_enter(xr_runtime_core_root_exec(vm->core_rt));
    ASSERT_EQ_INT(xr_text_worker_append_bytes(vm, worker, XR_TEXT_WORKER_CONCAT, "x\0y", 3u), XR_TEXT_WORKER_OK);
    ASSERT_EQ_INT(xr_text_worker_append_int(vm, worker, XR_TEXT_WORKER_CONCAT, -12), XR_TEXT_WORKER_OK);
    ASSERT_EQ_INT(xr_text_worker_append_float(vm, worker, XR_TEXT_WORKER_CONCAT, 1.5), XR_TEXT_WORKER_OK);
    ASSERT_EQ_INT(xr_text_worker_view(vm, worker, XR_TEXT_WORKER_CONCAT, &view), XR_TEXT_WORKER_OK);
    ASSERT_EQ_UINT(view.length, 9u);
    ASSERT_TRUE(memcmp(view.bytes, "x\0y-121.5", 9u) == 0);
    ASSERT_EQ_INT(xr_text_worker_finish(vm, &worker), XR_TEXT_WORKER_OK);
    ASSERT_TRUE(XR_IS_STRING(worker));
    ASSERT_EQ_UINT(XR_TO_STRING(worker)->length, 9u);
    ASSERT_TRUE(memcmp(XR_TO_STRING(worker)->data, "x\0y-121.5", 9u) == 0);
    ASSERT_EQ_UINT(heap->object_count, before_objects);
    ASSERT_EQ_UINT(heap->finalizer_count, before_finalizers + 1u);
    XrValue print = xr_null();
    ASSERT_EQ_INT(xr_text_worker_new(vm, XR_TEXT_WORKER_PRINT, &print), XR_TEXT_WORKER_OK);
    ASSERT_EQ_INT(xr_text_worker_finish(vm, &print), XR_TEXT_WORKER_INVALID);
    ASSERT_EQ_INT(xr_text_worker_close(vm, &print, XR_TEXT_WORKER_PRINT), XR_TEXT_WORKER_OK);
    ASSERT_TRUE(XR_IS_NULL(print));
    ASSERT_EQ_INT(xr_text_worker_close(vm, &print, XR_TEXT_WORKER_PRINT), XR_TEXT_WORKER_OK);
    XrValue absent = xr_make_typed_ptr_val(NULL, XR_TTEXT_WORKER);
    absent.heap_type = XR_TTEXT_WORKER;
    ASSERT_FALSE(xr_value_in_json_domain(absent));
    ASSERT_EQ_INT(xr_value_typeid(absent), UINT8_MAX);
    xray_vm_delete(vm);
}
TEST(instance_and_provider_reject_worker) {
    XrVMConfig config = {0};
    XrVMRuntime *vm = xray_vm_new_full(&config);
    ASSERT_NOT_NULL(vm);
    xr_exec_context_enter(xr_runtime_core_root_exec(vm->core_rt));
    XrClassBuilder *builder = xr_class_builder_new(vm, "UserContainer", NULL);
    ASSERT_NOT_NULL(builder);
    ASSERT_EQ_INT(xr_class_builder_add_field(builder, "slot", 0), 0);
    XrClass *klass = xr_class_builder_finalize(builder);
    ASSERT_NOT_NULL(klass);
    XrObjectInstance *instance = xr_instance_new(vm, klass);
    ASSERT_NOT_NULL(instance);
    XrValue worker = xr_null();
    ASSERT_EQ_INT(xr_text_worker_new(vm, XR_TEXT_WORKER_CONCAT, &worker), XR_TEXT_WORKER_OK);
    xr_instance_set_field(vm, instance, "slot", worker);
    ASSERT_TRUE(XR_IS_NULL(xr_instance_get_field_by_index(instance, 0)));
    xr_instance_set_field_by_index(instance, 0, worker);
    ASSERT_TRUE(XR_IS_NULL(xr_instance_get_field_by_index(instance, 0)));
    xr_instance_set_field_fast(instance, 0, worker);
    ASSERT_TRUE(XR_IS_NULL(xr_instance_get_field_by_index(instance, 0)));
    ASSERT_FALSE(xr_instance_set_decoded_field(instance, 0, worker));
    XrValue disguised = worker;
    disguised.heap_type = XR_TINSTANCE;
    ASSERT_TRUE(XR_IS_NULL(xr_instance_source_provider_storage(disguised)));
    xr_isolate_set_native_type_class(vm, XR_TTEXT_WORKER, klass);
    ASSERT_TRUE(xr_isolate_get_native_type_class(vm, XR_TTEXT_WORKER) == NULL);
    xr_instance_set_field_by_index(instance, 0, xr_int(42));
    ASSERT_EQ_INT(XR_TO_INT(xr_instance_get_field_by_index(instance, 0)), 42);
    ASSERT_EQ_INT(xr_text_worker_close(vm, &worker, XR_TEXT_WORKER_CONCAT), XR_TEXT_WORKER_OK);
    xray_vm_delete(vm);
}
TEST(nested_copy_rejects_worker) {
    XrVMConfig config = {0};
    XrVMRuntime *vm = xray_vm_new_full(&config);
    ASSERT_NOT_NULL(vm);
    xr_exec_context_enter(xr_runtime_core_root_exec(vm->core_rt));
    XrValue worker = xr_null();
    ASSERT_EQ_INT(xr_text_worker_new(vm, XR_TEXT_WORKER_CONCAT, &worker), XR_TEXT_WORKER_OK);
    XrArray *array = xr_array_with_capacity_in(&vm->core_rt->root_alloc, 2, XR_ELEM_ANY);
    ASSERT_NOT_NULL(array);
    xr_array_push(array, worker);
    ASSERT_TRUE(XR_IS_NULL(xr_deep_copy(vm, xr_make_typed_ptr_val(array, XR_TARRAY), NULL)));
    /* The input graph still owns the worker; ordinary heap teardown owns both. */
    xray_vm_delete(vm);
}
TEST(vm_private_concat_and_print) {
    XrVMConfig config = {0};
    XrVMRuntime *vm = xray_vm_new_full(&config);
    ASSERT_NOT_NULL(vm);
    xr_exec_context_enter(xr_runtime_core_root_exec(vm->core_rt));
    XrString *part = xr_string_intern(vm, "z", 1u, 0);
    ASSERT_NOT_NULL(part);
    XrProto *proto = xr_instruction_unit_new();
    ASSERT_NOT_NULL(proto);
    proto->source_file = "<private-concat-two-registers>";
    proto->maxstacksize = 2;
    int constant = xr_instruction_unit_add_constant(proto, xr_string_value(part));
    xr_instruction_unit_write(proto, CREATE_ABC(OP_STRBUF_NEW, 0, 0, 0), 1);
    xr_instruction_unit_write(proto, CREATE_ABx(OP_LOADK, 1, (uint32_t) constant), 1);
    for (unsigned i = 0; i < 512u; ++i)
        xr_instruction_unit_write(proto, CREATE_ABC(OP_STRBUF_APPEND, 0, 1, 0), 1);
    xr_instruction_unit_write(proto, CREATE_ABC(OP_STRBUF_FINISH, 0, 0, 0), 1);
    xr_instruction_unit_write(proto, CREATE_ABC(OP_RETURN1, 0, 0, 0), 1);
    ASSERT_EQ_INT(xr_vm_interpret_proto(vm, proto), XR_VM_OK);
    XrValue result = xr_vm_current_ctx(vm)->stack[0];
    ASSERT_TRUE(XR_IS_STRING(result));
    ASSERT_EQ_UINT(XR_TO_STRING(result)->length, 512u);
    for (unsigned i = 0; i < 512u; ++i)
        ASSERT_EQ_INT(XR_TO_STRING(result)->data[i], 'z');
    xray_vm_delete(vm);
    free_test_proto(proto);

    vm = xray_vm_new_full(&config);
    ASSERT_NOT_NULL(vm);
    xr_exec_context_enter(xr_runtime_core_root_exec(vm->core_rt));
    FILE *output = tmpfile();
    ASSERT_NOT_NULL(output);
    xr_isolate_set_stdout(vm, output);
    proto = xr_instruction_unit_new();
    ASSERT_NOT_NULL(proto);
    proto->source_file = "<private-print-group>";
    proto->maxstacksize = 4;
    constant = xr_instruction_unit_add_constant(proto, xr_string_value(xr_string_intern(vm, "ab", 2u, 0)));
    xr_instruction_unit_write(proto, CREATE_ABC(OP_PRINT_GROUP_NEW, 0, 0, 0), 1);
    xr_instruction_unit_write(proto, CREATE_ABx(OP_LOADK, 3, (uint32_t) constant), 1);
    xr_instruction_unit_write(proto, CREATE_ABC(OP_PRINT_GROUP_APPEND, 0, 3, 0), 1);
    xr_instruction_unit_write(proto, CREATE_ABC(OP_PRINT_GROUP_APPEND, 0, 3, XR_PRINT_BC_SEPARATE), 1);
    xr_instruction_unit_write(proto, CREATE_ABC(OP_PRINT_GROUP_FLUSH, 0, XR_PRINT_BC_TERMINATE, 0), 1);
    xr_instruction_unit_write(proto, CREATE_ABC(OP_RETURN0, 0, 0, 0), 1);
    ASSERT_EQ_INT(xr_vm_interpret_proto(vm, proto), XR_VM_OK);
    rewind(output);
    char bytes[7] = {0};
    ASSERT_EQ_UINT(fread(bytes, 1u, sizeof(bytes), output), 6u);
    ASSERT_TRUE(memcmp(bytes, "ab ab\n", 6u) == 0);
    xray_vm_delete(vm);
    free_test_proto(proto);
    fclose(output);
}
static unsigned formatter_calls;
static XrValue record_formatter_call(XrVMRuntime *vm, XrValue *args, int count) {
    (void) vm; (void) args; (void) count;
    ++formatter_calls;
    return xr_null();
}
TEST(print_user_formatter_return_continuations) {
    for (unsigned return_one = 0; return_one < 2u; ++return_one) {
        XrVMConfig config = {0};
        XrVMRuntime *vm = xray_vm_new_full(&config);
        ASSERT_NOT_NULL(vm);
        xr_exec_context_enter(xr_runtime_core_root_exec(vm->core_rt));
        FILE *output = tmpfile();
        ASSERT_NOT_NULL(output);
        xr_isolate_set_stdout(vm, output);
        formatter_calls = 0u;
        XrProto *method = xr_instruction_unit_new();
        ASSERT_NOT_NULL(method);
        method->source_file = "<ordinary-user-formatter>";
        method->maxstacksize = 4;
        method->numparams = method->min_params = 1;
        XrCFunction *counter = xr_cfunction_new(vm, record_formatter_call, "recordFormatter");
        ASSERT_NOT_NULL(counter);
        int call_constant = xr_instruction_unit_add_constant(method, xr_make_typed_ptr_val(counter, XR_TCFUNCTION));
        int text_constant = xr_instruction_unit_add_constant(method, xr_string_value(xr_string_intern(vm, "ok", 2u, 0)));
        xr_instruction_unit_write(method, CREATE_ABx(OP_LOADK, 1, (uint32_t) call_constant), 1);
        xr_instruction_unit_write(method, CREATE_ABC(OP_CALL, 1, 0, 0), 1);
        xr_instruction_unit_write(method, CREATE_ABx(OP_LOADK, 1, (uint32_t) text_constant), 1);
        xr_instruction_unit_write(method, CREATE_ABC(return_one ? OP_RETURN1 : OP_RETURN, 1, 1, 0), 1);
        XrClosure *closure = xr_closure_new(vm, method, NULL);
        ASSERT_NOT_NULL(closure);
        /* The spelling has no builtin privilege. This is an ordinary class. */
        XrClassBuilder *builder = xr_class_builder_new(vm, "StringBuilder", NULL);
        ASSERT_NOT_NULL(builder);
        ASSERT_EQ_INT(xr_class_builder_add_method_closure(builder, "toString", closure,
                                                         XMETHOD_CLOSURE, 0, 0, 0), 0);
        XrClass *klass = xr_class_builder_finalize(builder);
        ASSERT_NOT_NULL(klass);
        XrObjectInstance *instance = xr_instance_new(vm, klass);
        ASSERT_NOT_NULL(instance);
        XrProto *main_proto = xr_instruction_unit_new();
        ASSERT_NOT_NULL(main_proto);
        main_proto->source_file = "<print-user-formatter>";
        main_proto->maxstacksize = 8;
        int instance_constant = xr_instruction_unit_add_constant(main_proto,
                                  xr_make_typed_ptr_val(instance, XR_TINSTANCE));
        xr_instruction_unit_write(main_proto, CREATE_ABx(OP_LOADK, 6, (uint32_t) instance_constant), 1);
        xr_instruction_unit_write(main_proto, CREATE_ABC(OP_PRINT_GROUP_NEW, 0, 0, 0), 1);
        xr_instruction_unit_write(main_proto, CREATE_ABC(OP_PRINT_GROUP_APPEND, 0, 6, 0), 1);
        xr_instruction_unit_write(main_proto, CREATE_ABC(OP_PRINT_GROUP_APPEND, 0, 6, XR_PRINT_BC_SEPARATE), 1);
        xr_instruction_unit_write(main_proto, CREATE_ABC(OP_PRINT_GROUP_FLUSH, 0, XR_PRINT_BC_TERMINATE, 0), 1);
        xr_instruction_unit_write(main_proto, CREATE_ABC(OP_RETURN0, 0, 0, 0), 1);
        ASSERT_EQ_INT(xr_vm_interpret_proto(vm, main_proto), XR_VM_OK);
        ASSERT_EQ_UINT(formatter_calls, 2u);
        rewind(output);
        char bytes[7] = {0};
        ASSERT_EQ_UINT(fread(bytes, 1u, sizeof(bytes), output), 6u);
        ASSERT_TRUE(memcmp(bytes, "ok ok\n", 6u) == 0);
        xray_vm_delete(vm);
        free_test_proto(main_proto);
        free_test_proto(method);
        fclose(output);
    }
}
TEST(vm_abandonment_and_private_escape) {
    for (unsigned exit_kind = 0; exit_kind < 3u; ++exit_kind) {
        XrVMConfig config = {0};
        XrVMRuntime *vm = xray_vm_new_full(&config);
        ASSERT_NOT_NULL(vm);
        xr_isolate_set_suppress_exception_print(vm, true);
        XrProto *bootstrap = NULL;
        XrCoroutine *coro = new_test_coro(vm, &bootstrap);
        ASSERT_NOT_NULL(coro);
        XrProto *proto = NULL;
        if (exit_kind == 2u) {
            XrValue worker = xr_null();
            ASSERT_EQ_INT(xr_text_worker_new(vm, XR_TEXT_WORKER_PRINT, &worker), XR_TEXT_WORKER_OK);
            xr_coro_cancel(coro);
        } else {
            proto = xr_instruction_unit_new();
            ASSERT_NOT_NULL(proto);
            proto->source_file = "<private-worker-abandonment>";
            proto->maxstacksize = 2;
            xr_instruction_unit_write(proto, CREATE_ABC(OP_STRBUF_NEW, 0, 0, 0), 1);
            xr_instruction_unit_write(proto, CREATE_ABC(exit_kind == 0u ? OP_RETURN0 : OP_RETURN1,
                                                        0, 0, 0), 1);
            XrVMResult result = xr_vm_interpret_proto(vm, proto);
            ASSERT_EQ_INT(result, exit_kind == 0u ? XR_VM_OK : XR_VM_RUNTIME_ERROR);
        }
        /* These cases leave the registered worker to the real coroutine
         * shutdown path. Instrumented execution must prove cleanup. */
        xray_vm_delete(vm);
        free_test_proto(proto);
        free_test_proto(bootstrap);
    }
}
/* Fail in place after installing TLS, rather than leave a stack-owned fixture
 * behind and let later tests dereference it. All callbacks below are real VM
 * closures; the fixture only supplies the ordinary worker/coroutine identity. */
#define GROW_REQUIRE(condition) do { if (!(condition)) { \
    fprintf(stderr, "formatter growth requirement failed at %d: %s\n", __LINE__, #condition); \
    abort(); } } while (0)
TEST(formatter_callbacks_grow_stack_and_frames) {
    for (unsigned mode = 0; mode < 5u; ++mode) {
        XrVMConfig config = {0};
        XrVMRuntime *vm = xray_vm_new_full(&config);
        GROW_REQUIRE(vm != NULL);
        XrProto *bootstrap = NULL;
        XrCoroutine *coro = new_test_coro(vm, &bootstrap);
        GROW_REQUIRE(coro != NULL);
        XrRuntime runtime = {0};
        XrWorker worker = {0};
        XrMachine machine;
        runtime.core = vm->core_rt;
        runtime.worker_count = 1;
        runtime.workers = &worker;
        runtime.machines = &machine;
        worker.p.runtime = &runtime;
        worker.m = &machine;
        xr_machine_init(&machine, 0, &runtime);
        atomic_store_explicit(&worker.p.current_m, &machine, memory_order_relaxed);
        atomic_store_explicit(&machine.current_p, &worker.p, memory_order_relaxed);
        XrVMContext *machine_ctx = xr_machine_vm_context(&machine, vm);
        GROW_REQUIRE(machine_ctx != NULL);
        machine_ctx->current_coro = coro;
        XrWorker *previous_worker = tls_current_worker;
        XrMachine *previous_machine = tls_current_machine;
        tls_current_worker = &worker;
        tls_current_machine = &machine;
        XrVMContext *ctx = xr_vm_current_ctx(vm);
        GROW_REQUIRE(ctx == xr_coro_vm_ctx(coro));
        GROW_REQUIRE(ctx->frame_count == 0);
        /* Stack and frames can share one allocation. Leave both original
         * capacities and interior pointers intact. A genuine non-tail closure
         * chain longer than the initial frame capacity forces real growth. */
        int initial_stack_capacity = ctx->stack_capacity;
        int initial_frame_capacity = ctx->frame_capacity;
        GROW_REQUIRE(initial_frame_capacity > 0 && initial_frame_capacity < XR_FRAMES_MAX - 4);
        size_t chain_count = (size_t) initial_frame_capacity + 2u;
        XrProto **chain = xr_calloc(chain_count, sizeof(*chain));
        GROW_REQUIRE(chain != NULL);
        XrClosure *chain_head = NULL;
        for (size_t remaining = chain_count; remaining > 0; --remaining) {
            size_t index = remaining - 1u;
            chain[index] = xr_instruction_unit_new();
            GROW_REQUIRE(chain[index] != NULL);
            chain[index]->maxstacksize = 3;
            if (chain_head) {
                int child = xr_instruction_unit_add_constant(chain[index], xr_value_from_closure(chain_head));
                xr_instruction_unit_write(chain[index], CREATE_ABx(OP_LOADK, 0, (uint32_t) child), 1);
                xr_instruction_unit_write(chain[index], CREATE_ABC(OP_CALL, 0, 0, 1), 1);
            }
            xr_instruction_unit_write(chain[index], CREATE_ABC(OP_RETURN0, 0, 0, 0), 1);
            chain_head = xr_closure_new(vm, chain[index], NULL);
            GROW_REQUIRE(chain_head != NULL);
        }
        XrProto *date_method = xr_instruction_unit_new();
        GROW_REQUIRE(date_method != NULL);
        date_method->maxstacksize = initial_stack_capacity * 8 + 128;
        date_method->numparams = date_method->min_params = 1;
        int one = xr_instruction_unit_add_constant(date_method, xr_int(1));
        int text = xr_instruction_unit_add_constant(date_method, xr_string_value(xr_string_intern(vm, "dt", 2u, 0)));
        int nested = xr_instruction_unit_add_constant(date_method, xr_value_from_closure(chain_head));
        xr_instruction_unit_write(date_method, CREATE_ABx(OP_LOADK, 3, (uint32_t) nested), 1);
        xr_instruction_unit_write(date_method, CREATE_ABC(OP_CALL, 3, 0, 0), 1);
        xr_instruction_unit_write(date_method, CREATE_ABC(OP_GETFIELD, 1, 0, 0), 1);
        xr_instruction_unit_write(date_method, CREATE_ABx(OP_LOADK, 2, (uint32_t) one), 1);
        xr_instruction_unit_write(date_method, CREATE_ABC(OP_ADD, 1, 1, 2), 1);
        xr_instruction_unit_write(date_method, CREATE_ABC(OP_SETFIELD, 0, 0, 1), 1);
        xr_instruction_unit_write(date_method, CREATE_ABx(OP_LOADK, 1, (uint32_t) text), 1);
        xr_instruction_unit_write(date_method, CREATE_ABC(OP_RETURN1, 1, 0, 0), 1);
        XrClosure *date_closure = xr_closure_new(vm, date_method, NULL);
        GROW_REQUIRE(date_closure != NULL);
        XrClassBuilder *date_builder = xr_class_builder_new(vm, "DateTime", NULL);
        GROW_REQUIRE(date_builder != NULL);
        GROW_REQUIRE(xr_class_builder_add_field(date_builder, "calls", 0) == 0);
        GROW_REQUIRE(xr_class_builder_add_method_closure(date_builder, "toString", date_closure,
                                                       XMETHOD_CLOSURE, 0, 0, 0) == 0);
        XrClass *date_class = xr_class_builder_finalize(date_builder);
        GROW_REQUIRE(date_class != NULL);
        XrObjectInstance *date = xr_instance_new(vm, date_class);
        GROW_REQUIRE(date != NULL);
        xr_instance_set_field_by_index(date, 0, xr_int(0));
        XrValue date_value = xr_make_typed_ptr_val(date, XR_TINSTANCE);
        XrArray *array = xr_array_new(coro);
        GROW_REQUIRE(array != NULL);
        xr_array_push(array, date_value);
        XrValue input = mode == 4u ? date_value : xr_make_typed_ptr_val(array, XR_TARRAY);
        XrProto *outer_method = NULL;
        if (mode == 2u || mode == 3u) {
            outer_method = xr_instruction_unit_new();
            GROW_REQUIRE(outer_method != NULL);
            outer_method->maxstacksize = 4;
            outer_method->numparams = outer_method->min_params = 1;
            int value = xr_instruction_unit_add_constant(outer_method, input);
            xr_instruction_unit_write(outer_method, CREATE_ABx(OP_LOADK, 1, (uint32_t) value), 1);
            xr_instruction_unit_write(outer_method, CREATE_ABC(mode == 2u ? OP_RETURN : OP_RETURN1, 1, 1, 0), 1);
            XrClosure *outer_closure = xr_closure_new(vm, outer_method, NULL);
            GROW_REQUIRE(outer_closure != NULL);
            XrClassBuilder *builder = xr_class_builder_new(vm, "UserFormatter", NULL);
            GROW_REQUIRE(builder != NULL);
            GROW_REQUIRE(xr_class_builder_add_method_closure(builder, "toString", outer_closure,
                                                             XMETHOD_CLOSURE, 0, 0, 0) == 0);
            XrClass *klass = xr_class_builder_finalize(builder);
            GROW_REQUIRE(klass != NULL);
            XrObjectInstance *instance = xr_instance_new(vm, klass);
            GROW_REQUIRE(instance != NULL);
            input = xr_make_typed_ptr_val(instance, XR_TINSTANCE);
        }
        FILE *output = tmpfile();
        GROW_REQUIRE(output != NULL);
        xr_isolate_set_stdout(vm, output);
        XrProto *main_proto = xr_instruction_unit_new();
        GROW_REQUIRE(main_proto != NULL);
        main_proto->maxstacksize = 8;
        int value = xr_instruction_unit_add_constant(main_proto, input);
        xr_instruction_unit_write(main_proto, CREATE_ABx(OP_LOADK, 6, (uint32_t) value), 1);
        bool concatenate = mode == 0u || mode == 4u;
        xr_instruction_unit_write(main_proto, CREATE_ABC(concatenate ? OP_STRBUF_NEW : OP_PRINT_GROUP_NEW, 0, 0, 0), 1);
        xr_instruction_unit_write(main_proto, CREATE_ABC(concatenate ? OP_STRBUF_APPEND : OP_PRINT_GROUP_APPEND, 0, 6, 0), 1);
        xr_instruction_unit_write(main_proto, CREATE_ABC(concatenate ? OP_STRBUF_FINISH : OP_PRINT_GROUP_FLUSH,
                                                        0, concatenate ? 0 : XR_PRINT_BC_TERMINATE, 0), 1);
        xr_instruction_unit_write(main_proto, CREATE_ABC(concatenate ? OP_RETURN1 : OP_RETURN0, 0, 0, 0), 1);
        GROW_REQUIRE(xr_vm_interpret_proto(vm, main_proto) == XR_VM_OK);
        GROW_REQUIRE(ctx->stack_capacity > initial_stack_capacity && ctx->frame_capacity > initial_frame_capacity);
        GROW_REQUIRE(XR_TO_INT(xr_instance_get_field_by_index(date, 0)) == 1);
        if (concatenate) {
            XrValue result = ctx->stack[0];
            const char *expected = mode == 4u ? "dt" : "[dt]";
            GROW_REQUIRE(XR_IS_STRING(result) && XR_TO_STRING(result)->length == strlen(expected));
            GROW_REQUIRE(memcmp(XR_TO_STRING(result)->data, expected, strlen(expected)) == 0);
        } else {
            rewind(output);
            char bytes[6] = {0};
            GROW_REQUIRE(fread(bytes, 1u, sizeof(bytes), output) == 5u);
            GROW_REQUIRE(memcmp(bytes, "[dt]\n", 5u) == 0);
        }
        machine_ctx->current_coro = NULL;
        tls_current_worker = previous_worker;
        tls_current_machine = previous_machine;
        xr_machine_destroy(&machine);
        xray_vm_delete(vm);
        free_test_proto(main_proto);
        free_test_proto(outer_method);
        free_test_proto(date_method);
        for (size_t index = 0; index < chain_count; ++index)
            free_test_proto(chain[index]);
        xr_free(chain);
        free_test_proto(bootstrap);
        fclose(output);
    }
}
int text_worker_fault_cases(void);
TEST_MAIN_BEGIN()
    RUN_TEST(extension_interval);
    RUN_TEST(real_finish_close_and_guards);
    RUN_TEST(instance_and_provider_reject_worker);
    RUN_TEST(nested_copy_rejects_worker);
    RUN_TEST(vm_private_concat_and_print);
    RUN_TEST(print_user_formatter_return_continuations);
    RUN_TEST(vm_abandonment_and_private_escape);
    RUN_TEST(formatter_callbacks_grow_stack_and_frames);
    if (text_worker_fault_cases() != 0)
        return 1;
TEST_MAIN_END()
