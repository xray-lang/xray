/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_compile.c - Compilation and execution API
 *
 * KEY CONCEPT:
 *   AST compilation, bytecode execution, VM lifecycle management.
 */

#include "../runtime/xisolate_internal.h"
#include "../runtime/xisolate_api.h"
#include "../base/xchecks.h"
#include "../base/xlog.h"
#include "../base/xglobal_indices.h"
#include "../vm/xvm.h"
#include "../frontend/codegen/xcompiler.h"
#include "../frontend/codegen/xcompiler_context.h"
#include "../runtime/value/xchunk.h"
#include "../vm/xdebug.h"
#include "../runtime/value/xvalue.h"
#include "../base/xmalloc.h"
#include "../frontend/parser/xast.h"
#include "../frontend/parser/xparse.h"
#include "../runtime/class/xclass_system.h"
#include "xglobal_object.h"
#include "../vm/xvm_internal.h"
#include "../coro/xchannel.h"
#include "../coro/xworker.h"
#include "../runtime/gc/xgc.h"
#ifdef XRAY_HAS_JIT
#include "../jit/xir_jit.h"
#endif
#include "../runtime/object/xstring.h"
#include <stdio.h>
#include <string.h>

/* ========== Forward Declarations ========== */

XrVMResult xr_vm_interpret_proto_isolate(XrayIsolate *isolate, XrProto *proto);

/* ========== Compilation API ========== */

// Compile AST to bytecode (internal)
//
// The compiler's for-in desugaring (xstmt_forin.c) creates new AST nodes
// via xr_ast_* helpers which allocate from the Isolate's current arena.
// After xr_parse_with_source returns, the arena is transferred to the
// ProgramNode and the Isolate's pointer is restored to NULL.  We must
// re-install the program's arena for the duration of compilation so the
// desugaring allocations succeed, then restore the previous value.
static XrProto* compile_ast_internal(XrayIsolate *isolate, AstNode *ast, const char *source_file) {
    XR_DCHECK(isolate != NULL, "compile_ast_internal: NULL isolate");
    XR_DCHECK(ast != NULL, "compile_ast_internal: NULL ast");

    // Re-install the parse arena so compiler desugaring can allocate nodes.
    struct XrArena *saved_arena = xr_isolate_get_current_arena(isolate);
    if (ast->type == AST_PROGRAM && ast->as.program.arena) {
        xr_isolate_set_current_arena(isolate, ast->as.program.arena);
    }

    XrCompilerContext *ctx = xr_compiler_context_new(isolate);
    if (ctx == NULL) {
        xr_log_warning("vm", "failed to create compiler context");
        xr_isolate_set_current_arena(isolate, saved_arena);
        return NULL;
    }

    ctx->source_file = source_file;

    ctx->shared_offset = isolate->vm.shared.count;

    XrProto *proto = xr_compile(ctx, ast);

    // Sync shared variable count back to isolate (offset-adjusted)
    int total_shared = ctx->shared_offset + ctx->shared_var_count;
    if (total_shared > isolate->vm.shared.count) {
        isolate->vm.shared.count = total_shared;
        xr_shared_array_ensure(&isolate->vm.shared, total_shared - 1);
    }

    xr_compiler_context_free(ctx);

    // Restore previous arena.
    xr_isolate_set_current_arena(isolate, saved_arena);

    return proto;
}

// Compile AST to bytecode
XrProto* xr_compile_ast(XrayIsolate *isolate, AstNode *ast) {
    return compile_ast_internal(isolate, ast, NULL);
}

// Compile AST to bytecode (with source file path)
XrProto* xr_compile_ast_with_source(XrayIsolate *isolate, AstNode *ast, const char *source_file) {
    return compile_ast_internal(isolate, ast, source_file);
}

// Compile source code to bytecode (creates compiler context before parsing)
// This ensures type pool is valid during parsing for type annotations
XrProto* xr_compile_source_with_path(XrayIsolate *isolate, const char *source, const char *source_file) {
    XR_DCHECK(isolate != NULL, "compile_source_with_path: NULL isolate");
    XR_DCHECK(source != NULL, "compile_source_with_path: NULL source");
    // Create compiler context FIRST to ensure type pool is valid during parsing
    XrCompilerContext *ctx = xr_compiler_context_new(isolate);
    if (!ctx) {
        xr_log_warning("vm", "failed to create compiler context");
        return NULL;
    }

    ctx->source_file = source_file;
    ctx->shared_offset = isolate->vm.shared.count;

    // Now parse with valid type pool
    AstNode *ast = xr_parse_with_source(isolate, source, source_file);
    if (!ast) {
        xr_compiler_context_free(ctx);
        return NULL;
    }

    // Compile
    XrProto *proto = xr_compile(ctx, ast);
    int total_shared = ctx->shared_offset + ctx->shared_var_count;
    if (total_shared > isolate->vm.shared.count) {
        isolate->vm.shared.count = total_shared;
        xr_shared_array_ensure(&isolate->vm.shared, total_shared - 1);
    }

    xr_compiler_context_free(ctx);

    // Free AST (not needed after compilation)
    xr_program_destroy(ast);

    return proto;
}

/* ========== Execution API ========== */

// Execute bytecode
// main_coro already exists (created as bootstrap during isolate init),
// just upgrade it with the compiled closure.
int xr_execute(XrayIsolate *isolate, XrProto *proto) {
    XR_DCHECK(isolate != NULL, "xr_execute: NULL isolate");
    XR_DCHECK(proto != NULL, "xr_execute: NULL proto");
    if (proto == NULL) {
        xr_log_warning("vm", "invalid bytecode");
        return -1;
    }

    XrRuntime *runtime = (XrRuntime *)isolate->vm.runtime;
    if (!runtime) {
        XrVMResult result = xr_vm_interpret_proto_isolate(isolate, proto);
        return (result == XR_VM_OK) ? 0 : -1;
    }

    XrCoroutine *main_coro = isolate->main_coro;
    if (!main_coro) {
        xr_log_warning("vm", "main_coro not initialized");
        return -1;
    }

    XrClosure *closure = xr_closure_new(isolate, proto, main_coro);
    if (!closure) {
        xr_log_warning("vm", "failed to create main closure");
        return -1;
    }

    xr_coro_setup_main(main_coro, isolate, closure);

    return xr_main_thread_run(isolate, main_coro);
}

// Free bytecode
void xr_free_code(XrayIsolate *isolate, XrProto *proto) {
#ifdef XRAY_HAS_JIT
    // Drain bg compilation queue before freeing protos — the bg thread
    // may still be reading proto fields (use-after-free otherwise).
    if (isolate && isolate->vm.jit) {
        XirJitState *jit = isolate->vm.jit;
        if (jit->bg_queue) {
            xjit_queue_destroy(jit->bg_queue);
            xr_free(jit->bg_queue);
            jit->bg_queue = NULL;
        }
    }
#else
    (void)isolate;
#endif
    if (proto != NULL) {
        xr_vm_proto_free(proto);
    }
}

/* ========== VM Lifecycle ========== */

// Initialize global variables table
static void init_globals(XrayIsolate *isolate) {
    isolate->vm.builtin_count = 0;
    for (int i = 0; i < 256; i++) {
        isolate->vm.builtins[i] = xr_null();
    }

    xr_shared_array_init(&isolate->vm.shared);

    // Core class registration is done in isolate_init_full() (xisolate_full.c)
    // because isolate->core is NULL at this point (before init_extra callback).

    // User globals start from index XR_USER_GLOBALS_START
    if (isolate->vm.builtin_count < XR_USER_GLOBALS_START) {
        isolate->vm.builtin_count = XR_USER_GLOBALS_START;
    }
}

// Initialize coroutine scheduler
static void init_scheduler(XrayIsolate *isolate) {
    XrScheduler *sched = (XrScheduler *)xr_malloc(sizeof(XrScheduler));
    if (sched) {
        xr_sched_init(sched);
    }
    isolate->vm.scheduler = sched;
    isolate->vm.current_coro = NULL;
}

// Initialize unified VM context
static void init_vm_context(XrayIsolate *isolate) {
    XrVMContext *ctx = &isolate->vm_ctx;
    ctx->stack = isolate->vm.stack;
    ctx->stack_top = isolate->vm.stack_top;
    ctx->stack_capacity = XR_STACK_MAX;
    ctx->frames = isolate->vm.frames;
    ctx->frame_count = isolate->vm.frame_count;
    ctx->frame_capacity = XR_FRAMES_MAX;
    ctx->module_base_frame = isolate->vm.module_base_frame;
    ctx->handlers = isolate->vm.exception_handlers;
    ctx->handler_count = isolate->vm.handler_count;
    ctx->handler_capacity = XR_EXCEPTION_HANDLERS_MAX;
    ctx->current_exception = isolate->vm.current_exception;
    ctx->current_coro = isolate->vm.current_coro;
    ctx->trace_execution = isolate->vm.trace_execution;
    ctx->isolate = isolate;

    ctx->tmp_strbuf = NULL;
}

// Initialize VM execution engine
int xr_vm_init(XrayIsolate *isolate) {
    XR_DCHECK(isolate != NULL, "vm_init: NULL isolate");
    isolate->vm.stack_top = isolate->vm.stack;
    for (int i = 0; i < XR_STACK_MAX; i++) {
        isolate->vm.stack[i] = xr_null();
    }

    // Initialize call frames
    isolate->vm.frame_count = 0;
    isolate->vm.module_base_frame = -1;
    memset(isolate->vm.frames, 0, sizeof(XrBcCallFrame) * XR_FRAMES_MAX);

    // Initialize exception handling
    isolate->vm.handler_count = 0;
    isolate->vm.current_exception = xr_null();

    // Initialize string intern table
    isolate->vm.strings_map = xr_hashmap_new();
    if (isolate->vm.strings_map == NULL) {
        xr_log_warning("vm", "failed to create string intern table");
        return -1;
    }
    isolate->vm.trace_execution = isolate->params.trace_execution;

    init_globals(isolate);
    init_scheduler(isolate);

    // Initialize defer stack (lazy allocation)
    isolate->vm.defer_stack = NULL;
    isolate->vm.defer_count = 0;
    isolate->vm.defer_capacity = 0;
    isolate->vm.defer_frame_marks = NULL;

    init_vm_context(isolate);

#ifdef XRAY_HAS_JIT
    if (isolate->params.enable_jit) {
        int thr = isolate->params.jit_threshold > 0 ? isolate->params.jit_threshold : 100;
        isolate->vm.jit = xir_jit_init(isolate, thr);
        isolate->vm.jit_threshold = thr;
        if (isolate->vm.jit && isolate->params.jit_stats)
            isolate->vm.jit->stats_enabled = true;
    }
#endif

    return 0;
}

// Cleanup VM execution engine
void xr_vm_cleanup(XrayIsolate *isolate) {
    if (isolate->vm.strings_map != NULL) {
        xr_hashmap_free(isolate->vm.strings_map);
        isolate->vm.strings_map = NULL;
    }

    // Cleanup scheduler
    if (isolate->vm.scheduler != NULL) {
        xr_sched_destroy((XrScheduler*)isolate->vm.scheduler);
        xr_free(isolate->vm.scheduler);
        isolate->vm.scheduler = NULL;
    }

#ifdef XRAY_HAS_JIT
    if (isolate->vm.jit) {
        xir_jit_destroy(isolate->vm.jit);
        isolate->vm.jit = NULL;
    }
#endif

    // Cleanup defer stack
    if (isolate->vm.defer_stack != NULL) {
        xr_free(isolate->vm.defer_stack);
        isolate->vm.defer_stack = NULL;
    }
    if (isolate->vm.defer_frame_marks != NULL) {
        xr_free(isolate->vm.defer_frame_marks);
        isolate->vm.defer_frame_marks = NULL;
    }
}
