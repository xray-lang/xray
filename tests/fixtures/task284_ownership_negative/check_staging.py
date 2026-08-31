#!/usr/bin/env python3
"""Validate staged ownership source contracts and constructed negative oracles."""

from __future__ import annotations

import copy
import hashlib
import json
import subprocess
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
INVENTORY_PATH = HERE / "boundary_inventory.json"
ORACLES_PATH = HERE / "negative_oracles.json"
TYPED_CONTRACT_PATH = HERE / "typed_contract_staging.json"
ATOMIC_CUT_PATH = HERE / "atomic_cut_staging.json"
ACTIVATION_GATE_PATH = HERE / "activation_gate_staging.json"
POSITIVE_FIXTURE_DIR = HERE.parent / "task284_ownership_positive"

FROZEN_COMMIT = "49f71d1ea0f8e12aa2af8e64fb5d8d1c6f3ee79c"
FROZEN_TREE = "478e72a21e54ae5cc182caaa77cf9e526725a0a6"
TRAIN_COMMIT = "eabe56cc986cf2147d02254ae1b1acaa1418663f"
TRAIN_TREE = "45f708658f0945bf3f588be332859d3ecde58782"

EXPECTED_SURFACES = {
    "parser-ast": "LOCK-FRONTEND",
    "parameter-function-type": "LOCK-FRONTEND and LOCK-SCHEMA",
    "receiver": "LOCK-FRONTEND and LOCK-SCHEMA",
    "view-return": "LOCK-FRONTEND and LOCK-SCHEMA",
    "alias-loan": "LOCK-SCHEMA",
    "closure-capture": "LOCK-SCHEMA",
    "program-semantic-closure": "LOCK-SCHEMA",
    "storage-domain": "LOCK-SCHEMA and LOCK-RUNTIME",
    "xi-arc": "LOCK-SCHEMA",
    "semantic-plan": "LOCK-SCHEMA",
    "target-plan": "LOCK-SCHEMA",
    "vm": "LOCK-SCHEMA",
    "aot": "LOCK-BUILD-GEN and LOCK-SCHEMA",
    "runtime": "LOCK-RUNTIME",
    "lsp": "LOCK-FRONTEND and LOCK-BUILD-GEN",
    "docs": "frozen docs task",
    "ports": "integration train coordinated downstream lane",
}

EXPECTED_ROWS = {
    "recoverable-root-alias": ("P3", {"LOCK-SCHEMA", "LOCK-RESIDUE"}),
    "source-view-last-use": ("P4", {"LOCK-SCHEMA", "LOCK-RESIDUE"}),
    "closure-capture-authority": (
        "P4", {"LOCK-SCHEMA", "LOCK-RUNTIME", "LOCK-BUILD-GEN", "LOCK-RESIDUE"},
    ),
    "body-inferred-receiver": (
        "P5",
        {"LOCK-FRONTEND", "LOCK-SCHEMA", "LOCK-RESIDUE"},
    ),
    "body-inferred-borrow-origin": (
        "P5",
        {"LOCK-FRONTEND", "LOCK-SCHEMA", "LOCK-RESIDUE"},
    ),
    "read-retain-return-alias": (
        "P2",
        {"LOCK-FRONTEND", "LOCK-SCHEMA", "LOCK-RESIDUE"},
    ),
    "read-ref-suspend-authority": (
        "P2",
        {"LOCK-FRONTEND", "LOCK-SCHEMA", "LOCK-RESIDUE"},
    ),
    "typed-domain-edge-authority": (
        "P3",
        {"LOCK-SCHEMA", "LOCK-RUNTIME", "LOCK-RESIDUE"},
    ),
    "move-boundary-graph-retag": (
        "P7",
        {"LOCK-SCHEMA", "LOCK-RUNTIME", "LOCK-BUILD-GEN", "LOCK-RESIDUE"},
    ),
    "name-driven-ownership-permission": (
        "P6",
        {"LOCK-SCHEMA", "LOCK-BUILD-GEN", "LOCK-RESIDUE"},
    ),
}

EXPECTED_RETAINED_CLEANUP_RESIDUE = {
    ("src/frontend/analyzer/xa_ownership.h", "XA_LOAN_CLEANUP_READ,", 1, 1),
    (
        "src/frontend/analyzer/xanalyzer_visitor_internal.h",
        "XR_FUNC void xa_register_cleanup_loans(", 1, 1,
    ),
    ("src/frontend/analyzer/xa_ownership.c", "static bool xa_scope_is_within(", 1, 1),
    (
        "src/frontend/analyzer/xa_ownership.c",
        "static void xa_register_cleanup_loan_node(", 1, 1,
    ),
    (
        "src/frontend/analyzer/xa_ownership.c",
        "XR_FUNC void xa_register_cleanup_loans(", 1, 1,
    ),
    (
        "src/frontend/analyzer/xanalyzer_visitor.c",
        "xa_register_cleanup_loans(ctx, node);", 1, 1,
    ),
    (
        "src/frontend/analyzer/xanalyzer_visitor_internal.h",
        "xa_clear_active_loans_in_scope", 1, 1,
    ),
    ("src/frontend/analyzer/xa_ownership.c", "xa_clear_active_loans_in_scope", 1, 1),
    (
        "src/frontend/analyzer/xanalyzer_visitor_stmt.c",
        "xa_clear_active_loans_in_scope", 2, 2,
    ),
    ("src/frontend/analyzer/xa_ownership.c", "XA_LOAN_CLEANUP_READ", 7, 7),
}

EXPECTED_CLEANUP_WITNESSES = {
    (
        "tests/regression/07_strings/0733_string_bytes_receiver_nll.xr",
        "borrow_ends_after_last_use", 1,
    ),
    (
        "tests/compile_errors/type/span_active_borrow_owner_mutation.xr", "bytes.push(2)", 1,
    ),
    (
        "tests/compile_errors/ownership/180_defer_cleanup_blocks_move.xr",
        "consume(move buf)", 1,
    ),
    (
        "tests/compile_errors/ownership/180_defer_cleanup_blocks_move.xr.expected",
        "cannot move 'buf': a cleanup in this block reads it", 1,
    ),
    (
        "tests/compile_errors/ownership/181_defer_cleanup_blocks_return.xr", "return buf", 1,
    ),
    (
        "tests/compile_errors/ownership/181_defer_cleanup_blocks_return.xr.expected",
        "cannot return 'buf': a cleanup in this block reads it", 1,
    ),
    ("tests/regression/11_coroutine/1103_defer.xr", "defer { cleanup3() }", 1),
    (
        "tests/regression/11_coroutine/1103_defer.xr.expected",
        "cleanup 3\ncleanup 2\ncleanup 1", 1,
    ),
}

EXPECTED_CLOSURE_ANCHORS = {
    "producers": {
        ("src/frontend/analyzer/xa_ownership.h", "XA_LOAN_CAPTURE,", 1),
        ("src/frontend/analyzer/xanalyzer_infer.h", "#define XA_PENDING_CAPTURE_MAX 32", 1),
        (
            "src/frontend/analyzer/xanalyzer_infer.h",
            "XaSymbol *pending_captures[XA_PENDING_CAPTURE_MAX];", 1,
        ),
        ("src/frontend/analyzer/xanalyzer_infer.h", "int pending_capture_count;", 1),
        (
            "src/frontend/analyzer/xanalyzer_visitor_internal.h",
            "XR_FUNC void xa_record_pending_capture(", 1,
        ),
        (
            "src/frontend/analyzer/xanalyzer_visitor_internal.h",
            "XR_FUNC void xa_register_pending_capture_loans(", 1,
        ),
        (
            "src/frontend/analyzer/xanalyzer_visitor_internal.h",
            "XR_FUNC void xa_discard_pending_captures(", 1,
        ),
        (
            "src/frontend/analyzer/xanalyzer_visitor_internal.h",
            "XR_FUNC void xa_escape_pending_captures(", 1,
        ),
        ("src/frontend/analyzer/xa_ownership.c", "XR_FUNC void xa_record_pending_capture(", 1),
        (
            "src/frontend/analyzer/xa_ownership.c",
            "XR_FUNC void xa_register_pending_capture_loans(", 1,
        ),
        (
            "src/frontend/analyzer/xa_ownership.c",
            "XR_FUNC void xa_discard_pending_captures(", 1,
        ),
        ("src/frontend/analyzer/xa_ownership.c", "XR_FUNC void xa_escape_pending_captures(", 1),
        (
            "src/frontend/analyzer/xanalyzer_escape.c",
            "static void ea_mark_capture_for_go(", 1,
        ),
        (
            "src/frontend/analyzer/xanalyzer_escape.c",
            "static void ea_mark_borrowed_capture_for_closure(", 1,
        ),
        (
            "src/frontend/analyzer/xanalyzer_visitor_expr.c",
            "static bool xa_symbol_is_closure_borrowed_root(", 1,
        ),
        (
            "src/frontend/analyzer/xanalyzer_visitor_expr.c",
            "static void xa_note_closure_capture(", 1,
        ),
        ("src/frontend/analyzer/xanalyzer_infer.h", "int closure_body_depth;", 1),
        ("src/frontend/analyzer/xanalyzer_escape.c", "int go_scope_boundary;", 1),
        ("src/frontend/analyzer/xanalyzer_escape.c", "bool in_fn_closure;", 1),
        ("src/frontend/analyzer/xanalyzer_escape.c", "int fn_scope_boundary;", 1),
        (
            "src/frontend/analyzer/xanalyzer_escape.c",
            "#define EA_CAPTURE_STACK_MAX 16", 1,
        ),
        (
            "src/frontend/analyzer/xanalyzer_escape.c",
            "#define EA_CAPTURE_NAMES_MAX 32", 1,
        ),
        ("src/frontend/analyzer/xanalyzer_escape.c", "} EaCaptureSet;", 1),
        (
            "src/frontend/analyzer/xanalyzer_escape.c",
            "EaCaptureSet capture_stack[EA_CAPTURE_STACK_MAX];", 1,
        ),
        ("src/frontend/analyzer/xanalyzer_escape.c", "int capture_depth;", 1),
        (
            "src/frontend/analyzer/xanalyzer_escape.c",
            "bool capture_stack_overflowed;", 1,
        ),
        ("src/frontend/analyzer/xanalyzer_escape.c", "EaCaptureSet last_closure;", 1),
        ("src/frontend/analyzer/xanalyzer_escape.c", "bool last_closure_valid;", 1),
        ("src/frontend/analyzer/xanalyzer_escape.c", "static void ea_capture_note(", 1),
        (
            "src/frontend/analyzer/xanalyzer_escape.c",
            "static bool ea_capture_set_contains(", 1,
        ),
        (
            "src/frontend/analyzer/xanalyzer_escape.c",
            "static void ea_check_closure_cycle(", 1,
        ),
        ("src/aot/xaot_storage_plan.h", "XAOT_CAPTURE_EV_", 4),
        ("src/aot/xaot_storage_plan.h", "typedef struct XaotCapturePlan {", 1),
        ("src/aot/xaot_storage_plan.c", "static XaotCapturePlan derive_capture(", 1),
        (
            "src/aot/xaot_storage_plan.c",
            "bool xaot_storage_capture_plans_build(", 1,
        ),
    },
    "consumers": {
        (
            "src/frontend/analyzer/xanalyzer_visitor_expr.c",
            "xa_record_pending_capture(ctx, sym);", 1,
        ),
        (
            "src/frontend/analyzer/xanalyzer_visitor_stmt.c",
            "xa_discard_pending_captures(ctx);", 1,
        ),
        (
            "src/frontend/analyzer/xanalyzer_visitor_stmt.c",
            "xa_register_pending_capture_loans(ctx, sym,", 2,
        ),
        (
            "src/frontend/analyzer/xanalyzer_visitor_call.c",
            "xa_escape_pending_captures(ctx);", 1,
        ),
        (
            "src/frontend/analyzer/xanalyzer_escape.c",
            "ea_mark_capture_for_go(ctx, node, name);", 1,
        ),
        (
            "src/frontend/analyzer/xanalyzer_escape.c",
            "ea_mark_borrowed_capture_for_closure(ctx, node, name);", 1,
        ),
        (
            "src/frontend/analyzer/xanalyzer_visitor_expr.c",
            "xa_note_closure_capture(ctx, node, sym,", 2,
        ),
        (
            "src/frontend/analyzer/xanalyzer_visitor_expr.c",
            "ctx->closure_body_depth++;", 1,
        ),
        (
            "src/frontend/analyzer/xanalyzer_visitor_expr.c",
            "ctx->closure_body_depth--;", 1,
        ),
        (
            "src/frontend/analyzer/xa_ownership.c",
            "ctx && ctx->closure_body_depth == 0", 1,
        ),
        (
            "src/frontend/analyzer/xanalyzer_escape.c",
            "static void ea_walk_go_closure(", 1,
        ),
        (
            "src/frontend/analyzer/xanalyzer_escape.c",
            "static void ea_walk_function_expr_closure(", 1,
        ),
        (
            "src/frontend/analyzer/xanalyzer_escape.c",
            "ea_check_closure_cycle(ctx, node);", 1,
        ),
        ("src/plan/semantic/xr_semantic_builder.c", "static bool build_capture_records(", 1),
        ("src/plan/format/xr_xsm_encode.c", "static void encode_functions(", 1),
        ("src/plan/format/xr_xsm_decode.c", "static void decode_functions(", 1),
        ("src/ir/xi_lower.c", "xi_lower_capture_publish_semantics(", 2),
        (
            "src/plan/semantic/xr_semantic_verify.c",
            "capture->storage_domain == XR_STORAGE_DOMAIN_UNKNOWN", 1,
        ),
        ("src/ir/xi_lower.c", "finalize_capture_metadata(", 5),
        ("src/ir/xi_pass_close.c", "resolve_capture_kind(", 2),
        (
            "src/ir/xi_own.h",
            "XR_FUNC XrTransferAction xi_capture_cross_execution_action(", 1,
        ),
        (
            "src/ir/xi_own.c",
            "XR_FUNC XrTransferAction xi_capture_cross_execution_action(", 1,
        ),
        ("src/ir/xi_emit_object.c", "xi_capture_cross_execution_action(cap)", 2),
        ("src/runtime/value/xchunk.h", "uint8_t capture_action;", 1),
        (
            "src/runtime/value/xchunk.h",
            "uint8_t capture_action, struct XrType *type_info);", 1,
        ),
        ("src/runtime/value/xchunk.c", "uint8_t capture_action,", 1),
        ("src/runtime/value/xchunk.c", "capture_action = capture_action", 1),
        (
            "src/vm/xvm_coro_backend.c",
            "PROTO_UPVALUE(closure->proto, i).capture_action", 1,
        ),
        (
            "src/vm/xvm_coro_backend.c",
            "vm_closure_has_explicit_copy_capture(", 3,
        ),
        (
            "src/vm/xvm_coro_backend.c",
            "PROTO_UPVAL_COUNT(closure->proto) != closure->upval_count", 1,
        ),
        (
            "src/vm/xvm_coro_backend.c",
            "if (force_private_closure && closure->upval_count > 0 &&", 1,
        ),
        (
            "src/vm/xvm_coro_backend.c",
            "xr_deep_copy_explicit_to_coro(X, xr_value_from_closure(closure), coro)", 1,
        ),
        (
            "src/coro/xdeep_copy.c",
            "PROTO_UPVALUE(closure->proto, i).capture_action", 2,
        ),
        (
            "src/coro/xdeep_copy.c",
            "[XR_TFUNCTION] = xr_deep_copy_closure_with_ctx,", 1,
        ),
        ("src/coro/xdeep_copy.c", "XrValue xr_deep_copy_closure_with_ctx(", 1),
        (
            "src/coro/xdeep_copy.c",
            "(action == XR_TRANSFER_REJECT && !ctx->explicit_copy)", 1,
        ),
        ("src/coro/xdeep_copy.c", "action == XR_TRANSFER_MOVE_UNIQUE", 1),
        (
            "src/coro/xdeep_copy.c",
            "if (action == XR_TRANSFER_EXPLICIT_COPY || action == XR_TRANSFER_REJECT) {", 1,
        ),
        ("src/aot/xaot_storage_plan.c", "derive_capture(", 3),
        (
            "src/aot/xaot_storage_plan.c",
            "if (plan.source_domain == XR_STORAGE_DOMAIN_UNKNOWN)", 1,
        ),
        (
            "src/aot/xaot_storage_plan.c",
            "xi_capture_cross_execution_action(capture)", 1,
        ),
        ("src/aot/xaot_storage_plan.c", "XAOT_CAPTURE_EV_", 4),
        (
            "src/aot/xaot_storage_plan.h",
            "XR_FUNC bool xaot_storage_capture_plans_build(", 1,
        ),
        (
            "src/aot/xaot_storage_plan.h",
            "XR_FUNC bool xaot_storage_capture_plans_verify(", 1,
        ),
        (
            "src/aot/xaot_storage_plan.h",
            "XR_FUNC const XaotCapturePlan *xaot_capture_plan_find(", 1,
        ),
        ("src/aot/xaot_bundle.h", "XaotCapturePlan *capture_plans;", 1),
        ("src/aot/xaot_bundle.h", "uint32_t ncapture_plans;", 1),
        ("src/aot/xaot_bundle.h", "uint32_t capture_plan_cap;", 1),
        ("src/aot/xaot_storage_plan.c", "add_captures_recursive(", 3),
        ("src/aot/xaot_storage_plan.c", "verify_captures_recursive(", 3),
        (
            "src/aot/xaot_storage_plan.c",
            "bool xaot_storage_capture_plans_verify(", 1,
        ),
        (
            "src/aot/xaot_storage_plan.c",
            "const XaotCapturePlan *xaot_capture_plan_find(", 1,
        ),
        ("src/aot/xaot_bundle.c", "xr_free(bundle->capture_plans);", 1),
        ("src/aot/xaot_bundle.c", "bundle->capture_plans = NULL;", 1),
        ("src/aot/xaot_bundle.c", "bundle->capture_plan_cap = 0;", 1),
        ("src/aot/xaot_bundle.c", "xaot_storage_capture_plans_build(bundle)", 1),
        ("src/aot/xaot_bundle.c", "bundle->ncapture_plans", 2),
        (
            "src/aot/xaot_bundle.c",
            "const XaotCapturePlan *cp = &bundle->capture_plans[ci];", 1,
        ),
        (
            "src/aot/xaot_verify.c",
            "xaot_capture_plan_find(bundle, target, ci)", 1,
        ),
        ("src/aot/xaot_verify.c", "verify_spawn_capture_target(", 2),
        ("src/aot/xaot_verify.c", "verify_spawn_capture_materialization(", 2),
        (
            "src/aot/xaot_verify.c",
            "if (plan->action == XR_TRANSFER_REJECT || plan->action == "
            "XR_TRANSFER_MOVE_UNIQUE)", 1,
        ),
        (
            "src/aot/xaot_verify.c",
            "xaot_storage_capture_plans_verify(bundle, errbuf, errbuf_len)", 1,
        ),
        (
            "src/aot/xi_cgen_coro.inc.c",
            "xaot_capture_plan_find(bundle, target, ci)", 2,
        ),
        ("src/aot/xi_cgen_coro.inc.c", "emit_aot_frame_transfer_cl_arg(", 2),
        (
            "src/aot/xi_cgen_coro.inc.c",
            "if (!plan || plan->action == XR_TRANSFER_REJECT ||", 1,
        ),
        (
            "src/aot/xi_cgen_coro.inc.c",
            "fprintf(out, \"({ xrt_closure_t *_src = ", 1,
        ),
        (
            "src/aot/xi_cgen_coro.inc.c",
            "if (plan->action == XR_TRANSFER_EXPLICIT_COPY)", 1,
        ),
        (
            "src/aot/xi_cgen_coro.inc.c",
            "xrt_value_clone_for_coro(_src->upvals[%u]);", 1,
        ),
    },
    "witnesses": {
        (
            "tests/compile_errors/ownership/185_move_rejects_escaping_closure_capture.xr",
            "stash(fn() -> i64", 1,
        ),
        (
            "tests/diff/cases/semantics/ownership/in_closure_copy_capture_allowed.xr",
            "var snapshot = copy(data)", 1,
        ),
        (
            "tests/fixtures/task284_ownership_positive/closure_immediate_read_borrow.xr",
            "return call_now(fn() -> i64", 1,
        ),
        (
            "tests/fixtures/task284_ownership_positive/closure_move_adoption.xr",
            "var reader = make_reader(move values)", 1,
        ),
        (
            "tests/fixtures/task284_ownership_negative/read_cross_execution.xr",
            "var task = go fn() -> i64", 1,
        ),
        ("tests/compile_errors/closure/036_go_lambda_capture_var.xr", "var t = go fn()", 1),
        (
            "tests/diff/cases/semantics/ownership/converged_capability_positive.xr",
            "const captured = [8, 9]", 1,
        ),
        (
            "tests/diff/cases/semantics/ownership/in_go_copy_argument_allowed.xr",
            "}(copy(data))", 1,
        ),
        (
            "tests/diff/cases/semantics/ownership/move_into_go.xr",
            "go consume(move job)", 1,
        ),
    },
}

EXPECTED_CLOSURE_RESIDUE = {
    ("src/frontend/analyzer/xa_ownership.h", "XA_LOAN_CAPTURE,", 1, "replace-with-new-owner"),
    (
        "src/frontend/analyzer/xanalyzer_infer.h", "#define XA_PENDING_CAPTURE_MAX 32",
        1, "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_infer.h",
        "XaSymbol *pending_captures[XA_PENDING_CAPTURE_MAX];", 1, "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_infer.h", "int pending_capture_count;", 1,
        "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_visitor_internal.h",
        "XR_FUNC void xa_record_pending_capture(", 1, "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_visitor_internal.h",
        "XR_FUNC void xa_register_pending_capture_loans(", 1, "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_visitor_internal.h",
        "XR_FUNC void xa_discard_pending_captures(", 1, "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_visitor_internal.h",
        "XR_FUNC void xa_escape_pending_captures(", 1, "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xa_ownership.c", "XR_FUNC void xa_record_pending_capture(",
        1, "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xa_ownership.c",
        "XR_FUNC void xa_register_pending_capture_loans(", 1, "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xa_ownership.c", "XR_FUNC void xa_discard_pending_captures(",
        1, "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xa_ownership.c", "XR_FUNC void xa_escape_pending_captures(",
        1, "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_visitor_expr.c",
        "xa_record_pending_capture(ctx, sym);", 1, "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_visitor_stmt.c",
        "xa_discard_pending_captures(ctx);", 1, "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_visitor_stmt.c",
        "xa_register_pending_capture_loans(ctx, sym,", 2, "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_visitor_call.c",
        "xa_escape_pending_captures(ctx);", 1, "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_escape.c", "static void ea_mark_capture_for_go(",
        1, "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_escape.c",
        "ea_mark_capture_for_go(ctx, node, name);", 1, "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_escape.c",
        "static void ea_mark_borrowed_capture_for_closure(", 1, "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_escape.c",
        "ea_mark_borrowed_capture_for_closure(ctx, node, name);", 1,
        "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_visitor_expr.c",
        "static bool xa_symbol_is_closure_borrowed_root(", 1, "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_visitor_expr.c",
        "static void xa_note_closure_capture(", 1, "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_visitor_expr.c",
        "xa_note_closure_capture(ctx, node, sym,", 2, "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_infer.h", "int closure_body_depth;", 1,
        "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_visitor_expr.c", "ctx->closure_body_depth++;", 1,
        "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_visitor_expr.c", "ctx->closure_body_depth--;", 1,
        "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xa_ownership.c", "ctx && ctx->closure_body_depth == 0", 1,
        "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_escape.c", "int go_scope_boundary;", 1,
        "retain-bounded",
    ),
    (
        "src/frontend/analyzer/xanalyzer_escape.c", "bool in_fn_closure;", 1,
        "retain-bounded",
    ),
    (
        "src/frontend/analyzer/xanalyzer_escape.c", "int fn_scope_boundary;", 1,
        "retain-bounded",
    ),
    (
        "src/frontend/analyzer/xanalyzer_escape.c", "static void ea_walk_go_closure(", 1,
        "retain-bounded",
    ),
    (
        "src/frontend/analyzer/xanalyzer_escape.c",
        "static void ea_walk_function_expr_closure(", 1, "retain-bounded",
    ),
    (
        "src/frontend/analyzer/xanalyzer_escape.c", "#define EA_CAPTURE_STACK_MAX 16", 1,
        "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_escape.c", "#define EA_CAPTURE_NAMES_MAX 32", 1,
        "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_escape.c", "} EaCaptureSet;", 1,
        "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_escape.c",
        "EaCaptureSet capture_stack[EA_CAPTURE_STACK_MAX];", 1, "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_escape.c", "int capture_depth;", 1,
        "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_escape.c", "bool capture_stack_overflowed;", 1,
        "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_escape.c", "EaCaptureSet last_closure;", 1,
        "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_escape.c", "bool last_closure_valid;", 1,
        "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_escape.c", "static void ea_capture_note(", 1,
        "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_escape.c", "static bool ea_capture_set_contains(",
        1, "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_escape.c", "static void ea_check_closure_cycle(",
        1, "replace-with-new-owner",
    ),
    (
        "src/frontend/analyzer/xanalyzer_escape.c", "ea_check_closure_cycle(ctx, node);", 1,
        "replace-with-new-owner",
    ),
    (
        "src/plan/semantic/xr_semantic_builder.c", "static bool build_capture_records(", 1,
        "retain-bounded",
    ),
    (
        "src/plan/format/xr_xsm_encode.c", "static void encode_functions(", 1,
        "retain-bounded",
    ),
    (
        "src/plan/format/xr_xsm_decode.c", "static void decode_functions(", 1,
        "retain-bounded",
    ),
    (
        "src/plan/semantic/xr_semantic_verify.c",
        "capture->storage_domain == XR_STORAGE_DOMAIN_UNKNOWN", 1, "retain-bounded",
    ),
    (
        "src/ir/xi_lower.c", "xi_lower_capture_publish_semantics(", 2,
        "replace-with-new-owner",
    ),
    (
        "src/ir/xi_lower.c", "finalize_capture_metadata(", 5, "replace-with-new-owner",
    ),
    (
        "src/ir/xi_pass_close.c", "resolve_capture_kind(", 2, "replace-with-new-owner",
    ),
    (
        "src/ir/xi_own.h", "XR_FUNC XrTransferAction xi_capture_cross_execution_action(", 1,
        "replace-with-new-owner",
    ),
    (
        "src/ir/xi_own.c", "XR_FUNC XrTransferAction xi_capture_cross_execution_action(", 1,
        "replace-with-new-owner",
    ),
    (
        "src/ir/xi_emit_object.c", "xi_capture_cross_execution_action(cap)", 2,
        "replace-with-new-owner",
    ),
    ("src/runtime/value/xchunk.h", "uint8_t capture_action;", 1, "retain-bounded"),
    (
        "src/runtime/value/xchunk.h", "uint8_t capture_action, struct XrType *type_info);", 1,
        "retain-bounded",
    ),
    ("src/runtime/value/xchunk.c", "uint8_t capture_action,", 1, "retain-bounded"),
    ("src/runtime/value/xchunk.c", "capture_action = capture_action", 1, "retain-bounded"),
    (
        "src/vm/xvm_coro_backend.c", "PROTO_UPVALUE(closure->proto, i).capture_action", 1,
        "retain-bounded",
    ),
    (
        "src/vm/xvm_coro_backend.c", "vm_closure_has_explicit_copy_capture(", 3,
        "replace-with-new-owner",
    ),
    (
        "src/vm/xvm_coro_backend.c",
        "PROTO_UPVAL_COUNT(closure->proto) != closure->upval_count", 1, "delete-zero",
    ),
    (
        "src/vm/xvm_coro_backend.c",
        "if (force_private_closure && closure->upval_count > 0 &&", 1,
        "replace-with-new-owner",
    ),
    (
        "src/vm/xvm_coro_backend.c",
        "xr_deep_copy_explicit_to_coro(X, xr_value_from_closure(closure), coro)", 1,
        "retain-bounded",
    ),
    (
        "src/coro/xdeep_copy.c", "PROTO_UPVALUE(closure->proto, i).capture_action", 2,
        "retain-bounded",
    ),
    (
        "src/coro/xdeep_copy.c", "[XR_TFUNCTION] = xr_deep_copy_closure_with_ctx,", 1,
        "retain-bounded",
    ),
    (
        "src/coro/xdeep_copy.c", "XrValue xr_deep_copy_closure_with_ctx(", 1,
        "retain-bounded",
    ),
    (
        "src/coro/xdeep_copy.c", "(action == XR_TRANSFER_REJECT && !ctx->explicit_copy)",
        1, "delete-zero",
    ),
    (
        "src/coro/xdeep_copy.c", "action == XR_TRANSFER_MOVE_UNIQUE", 1,
        "retain-bounded",
    ),
    (
        "src/coro/xdeep_copy.c",
        "if (action == XR_TRANSFER_EXPLICIT_COPY || action == XR_TRANSFER_REJECT) {", 1,
        "delete-zero",
    ),
    ("src/aot/xaot_storage_plan.h", "XAOT_CAPTURE_EV_", 4, "delete-zero"),
    ("src/aot/xaot_storage_plan.h", "typedef struct XaotCapturePlan {", 1, "delete-zero"),
    (
        "src/aot/xaot_storage_plan.h", "XR_FUNC bool xaot_storage_capture_plans_build(",
        1, "delete-zero",
    ),
    (
        "src/aot/xaot_storage_plan.h", "XR_FUNC bool xaot_storage_capture_plans_verify(",
        1, "delete-zero",
    ),
    (
        "src/aot/xaot_storage_plan.h",
        "XR_FUNC const XaotCapturePlan *xaot_capture_plan_find(", 1, "delete-zero",
    ),
    ("src/aot/xaot_bundle.h", "XaotCapturePlan *capture_plans;", 1, "delete-zero"),
    ("src/aot/xaot_bundle.h", "uint32_t ncapture_plans;", 1, "delete-zero"),
    ("src/aot/xaot_bundle.h", "uint32_t capture_plan_cap;", 1, "delete-zero"),
    (
        "src/aot/xaot_storage_plan.c", "derive_capture(", 3, "replace-with-new-owner",
    ),
    (
        "src/aot/xaot_storage_plan.c",
        "if (plan.source_domain == XR_STORAGE_DOMAIN_UNKNOWN)", 1, "delete-zero",
    ),
    (
        "src/aot/xaot_storage_plan.c", "xi_capture_cross_execution_action(capture)", 1,
        "replace-with-new-owner",
    ),
    ("src/aot/xaot_storage_plan.c", "XAOT_CAPTURE_EV_", 4, "delete-zero"),
    (
        "src/aot/xaot_storage_plan.c", "add_captures_recursive(", 3, "delete-zero",
    ),
    (
        "src/aot/xaot_storage_plan.c", "verify_captures_recursive(", 3, "delete-zero",
    ),
    (
        "src/aot/xaot_storage_plan.c", "bool xaot_storage_capture_plans_build(", 1,
        "delete-zero",
    ),
    (
        "src/aot/xaot_storage_plan.c", "bool xaot_storage_capture_plans_verify(", 1,
        "delete-zero",
    ),
    (
        "src/aot/xaot_storage_plan.c", "const XaotCapturePlan *xaot_capture_plan_find(", 1,
        "delete-zero",
    ),
    ("src/aot/xaot_bundle.c", "xr_free(bundle->capture_plans);", 1, "delete-zero"),
    ("src/aot/xaot_bundle.c", "bundle->capture_plans = NULL;", 1, "delete-zero"),
    ("src/aot/xaot_bundle.c", "bundle->capture_plan_cap = 0;", 1, "delete-zero"),
    ("src/aot/xaot_bundle.c", "xaot_storage_capture_plans_build(bundle)", 1, "delete-zero"),
    ("src/aot/xaot_bundle.c", "bundle->ncapture_plans", 2, "delete-zero"),
    (
        "src/aot/xaot_bundle.c", "const XaotCapturePlan *cp = &bundle->capture_plans[ci];",
        1, "delete-zero",
    ),
    (
        "src/aot/xaot_verify.c", "xaot_capture_plan_find(bundle, target, ci)", 1,
        "delete-zero",
    ),
    (
        "src/aot/xaot_verify.c", "verify_spawn_capture_target(", 2, "retain-bounded",
    ),
    (
        "src/aot/xaot_verify.c", "verify_spawn_capture_materialization(", 2,
        "retain-bounded",
    ),
    (
        "src/aot/xaot_verify.c",
        "if (plan->action == XR_TRANSFER_REJECT || plan->action == "
        "XR_TRANSFER_MOVE_UNIQUE)", 1, "replace-with-new-owner",
    ),
    (
        "src/aot/xaot_verify.c",
        "xaot_storage_capture_plans_verify(bundle, errbuf, errbuf_len)", 1, "delete-zero",
    ),
    (
        "src/aot/xi_cgen_coro.inc.c", "xaot_capture_plan_find(bundle, target, ci)", 2,
        "delete-zero",
    ),
    (
        "src/aot/xi_cgen_coro.inc.c", "emit_aot_frame_transfer_cl_arg(", 2,
        "replace-with-new-owner",
    ),
    (
        "src/aot/xi_cgen_coro.inc.c",
        "if (!plan || plan->action == XR_TRANSFER_REJECT ||", 1, "replace-with-new-owner",
    ),
    (
        "src/aot/xi_cgen_coro.inc.c", "fprintf(out, \"({ xrt_closure_t *_src = ",
        1, "delete-zero",
    ),
    (
        "src/aot/xi_cgen_coro.inc.c",
        "if (plan->action == XR_TRANSFER_EXPLICIT_COPY)", 1, "retain-bounded",
    ),
    (
        "src/aot/xi_cgen_coro.inc.c",
        "xrt_value_clone_for_coro(_src->upvals[%u]);", 1, "retain-bounded",
    ),
}

EXPECTED_CASES = {
    "receiver-read-write": (
        "source", "receiver_read_write.xr", "OWN-E-RECEIVER-READ-WRITE", {"LOCK-FRONTEND"}
    ),
    "receiver-move-plain-lvalue": (
        "source", "receiver_move_plain_lvalue.xr", "OWN-E-RECEIVER-MOVE", {"LOCK-FRONTEND"}
    ),
    "borrow-origin-ambiguous": (
        "source", "borrow_origin_ambiguous.xr", "OWN-E-VIEW-ORIGIN-AMBIGUOUS",
        {"LOCK-FRONTEND", "LOCK-SCHEMA"},
    ),
    "borrow-origin-invalid": (
        "source", "borrow_origin_invalid.xr", "OWN-E-VIEW-ORIGIN-INVALID",
        {"LOCK-FRONTEND", "LOCK-SCHEMA"},
    ),
    "read-escape": (
        "source", "read_escape.xr", "OWN-E-READ-ESCAPE", {"LOCK-FRONTEND", "LOCK-SCHEMA"}
    ),
    "read-return": (
        "source", "read_return.xr", "OWN-E-READ-ESCAPE", {"LOCK-FRONTEND", "LOCK-SCHEMA"}
    ),
    "read-capture": (
        "source", "read_capture.xr", "OWN-E-READ-ESCAPE", {"LOCK-SCHEMA"}
    ),
    "read-cross-execution": (
        "source", "read_cross_execution.xr", "OWN-E-READ-ESCAPE", {"LOCK-SCHEMA"}
    ),
    "read-foreign-unknown": (
        "semantic-plan-mutation", "read_foreign_unknown.mutation.json", "OWN-E-UNKNOWN-EDGE",
        {"LOCK-SCHEMA"},
    ),
    "read-unknown-target": (
        "source", "read_unknown_target.xr", "OWN-E-READ-SUSPEND",
        {"LOCK-FRONTEND", "LOCK-SCHEMA"},
    ),
    "read-suspend": (
        "source", "read_suspend.xr", "OWN-E-READ-SUSPEND", {"LOCK-FRONTEND", "LOCK-SCHEMA"}
    ),
    "ref-escape": (
        "source", "ref_escape.xr", "OWN-E-REF-ESCAPE", {"LOCK-FRONTEND", "LOCK-SCHEMA"}
    ),
    "ref-suspend": (
        "source", "ref_suspend.xr", "OWN-E-REF-SUSPEND", {"LOCK-FRONTEND", "LOCK-SCHEMA"}
    ),
    "external-alias-after-scope": (
        "source", "external_alias_after_scope.xr", "OWN-E-EXTERNAL-ALIAS", {"LOCK-SCHEMA"}
    ),
    "view-after-owner-invalidation": (
        "source", "view_after_owner_invalidation.xr", "OWN-E-VIEW-INVALIDATED", {"LOCK-SCHEMA"}
    ),
    "view-active-conflict": (
        "source", "view_active_conflict.xr", "OWN-E-VIEW-ACTIVE-CONFLICT", {"LOCK-SCHEMA"}
    ),
    "view-escaped": (
        "source", "view_escaped.xr", "OWN-E-VIEW-ESCAPED", {"LOCK-SCHEMA"}
    ),
    "view-mutable-return": (
        "source", "view_mutable_return.xr", "OWN-E-VIEW-MUTABLE",
        {"LOCK-FRONTEND", "LOCK-SCHEMA"},
    ),
    "domain-edge": (
        "semantic-plan-mutation", "domain_edge.mutation.json", "OWN-E-DOMAIN-EDGE",
        {"LOCK-SCHEMA"},
    ),
    "unknown-domain-edge": (
        "semantic-plan-mutation", "unknown_domain_edge.mutation.json", "OWN-E-UNKNOWN-EDGE",
        {"LOCK-SCHEMA"},
    ),
}

EXPECTED_HARNESSES = {
    "tests/compile_errors/run_compile_error_tests.py",
    "tests/unit/frontend/test_xa_program_semantic_closure.c",
    "tests/unit/plan/test_semantic_plan.c",
    "tests/unit/plan/test_target_plan.c",
    "tests/unit/ir/test_xi_program_semantic.c",
    "tests/unit/ir/test_xi_source_move_verify.c",
    "tests/unit/runtime/test_ownership_audit.c",
    "tests/unit/runtime/test_runtime_target_plan_load_archive.c",
    "tests/unit/aot/test_xr_aot_refinement.c",
}

EXPECTED_FIXTURE_SHA256 = {
    "borrow_origin_ambiguous.xr": "d5a33d60c7d41e1e0fbc1a31117d19c51a73e6734ef70db652d43081ca566ca5",
    "borrow_origin_invalid.xr": "0432334f4d2383338cbd089c0978ba888e36f4100737c9fb2de3274347f1f713",
    "domain_edge.mutation.json": "6af7508e525cefc04da64dc64abac255257599e20184e7d45e1cb1943836812b",
    "external_alias_after_scope.xr": "a51654da5e778a2e58370b5753155bfaf939a55728fcf2fa0e55726d949d3072",
    "read_escape.xr": "ad3da40171848dbadfcc61ee49d772125a3f96babc2e94fe28ab233ca0805e87",
    "read_return.xr": "f92fc94a92306a2436469fde1c6ff1f0c1477f6675b3c450046939d67ebff87b",
    "read_capture.xr": "30a286b296e3073ef7c5c3eca86c1f1ec89b0290c8efde678228d3cb0cd8450d",
    "read_cross_execution.xr": "f00871360458c0b4e21308228032e793ced5b1c3fcb0541accfdbe796ff6ffdc",
    "read_foreign_unknown.mutation.json": "b09660ac36858c73bb411e2206585047f8ea79937ebf577c0d7f4612137d6c03",
    "read_unknown_target.xr": "333d2fdbba008e84d3259c6f775c795a2ea7606ed95b845e31761cc2acb8932d",
    "read_suspend.xr": "e4b8fadd1026f42d7183aac70749e0a6783055b6c22aaeae4ca238a8142c16c9",
    "receiver_move_plain_lvalue.xr": "cc069bf9fde1b301ee7bce65a8cdb0c26e1ad1737fa8575e5769b6c2cc9e89fa",
    "receiver_read_write.xr": "bfe0832a49d11663b82048c8353180aef74da5ad4f6a648b9d56bbcdd77cf6f1",
    "ref_escape.xr": "0d4a4a643667aaae4e0fde3d4d952b6d219b39aeb94aea3a888bf0cb60324c15",
    "ref_suspend.xr": "66079eee7654b99786887762a338ca0b2d112d203440c2a859cfd04ed039f990",
    "unknown_domain_edge.mutation.json": "c1040dbf137f3507ce8adc6a7df7ace98a9e8ebb40ed571539c3398eca9613e6",
    "view_active_conflict.xr": "d5544b27ca5d9dab7a0b274e51f9d66889e587ef009ff6e8729c835d9ab9adb4",
    "view_after_owner_invalidation.xr": "831d391efefb2a06dc3c7ff9d76df89c92f42c633a8f16b5583228b0cd009f9d",
    "view_escaped.xr": "a730cddcf4bc466d74ff39eb24557f1dcb2fdd1b9266feb405beebe6ccd03496",
    "view_mutable_return.xr": "0d8013c86b2d99a50ca2ff8bc06d8237a320dc9cc1bd78999eb05544585bd9bf",
}
EXPECTED_POSITIVE_FIXTURE_SHA256 = {
    "closure_immediate_read_borrow.xr": (
        "71240f3b2e67684f6777d1818511370a6cd4a1a2b40156e3a81c7505fc2abbb8"
    ),
    "closure_move_adoption.xr": (
        "d841e991e73aa26c23bcb98984f42dcd591e38975b347c4c37a27a198b4b779c"
    ),
}
EXPECTED_MUTATION_DETAILS = {
    "domain_edge.mutation.json": (
        "A verified transferable root has one internal-owned edge before the mutation",
        (
            "Change the edge kind to plain-strong",
            "Change the target domain identity to a distinct execution domain",
            "Recompute the enclosing artifact checksum",
        ),
        (
            "Reject the cross-domain plain-strong edge", "Report the first edge witness",
            "Do not execute either VM or AOT",
        ),
    ),
    "read_foreign_unknown.mutation.json": (
        "A READ mutable-root argument has one complete FOREIGN_BORROWED edge before the mutation",
        (
            "Change the foreign edge kind to UNKNOWN",
            "Clear the completeness bit and set unknown reason to MISSING_FOREIGN_RETENTION_CONTRACT",
            "Recompute the enclosing artifact checksum",
        ),
        (
            "Reject the UNKNOWN foreign edge with incomplete provenance before execution",
            "Report the foreign call and first UNKNOWN edge witness",
            "Do not execute either VM or AOT",
        ),
    ),
    "unknown_domain_edge.mutation.json": (
        "A verified transferable root has a complete typed edge row before the mutation",
        (
            "Change the edge kind to unknown", "Clear the typed-contract completeness bit",
            "Recompute the enclosing artifact checksum",
        ),
        (
            "Reject incomplete edge provenance", "Report the unknown edge witness",
            "Do not execute either VM or AOT",
        ),
    ),
}
EXPECTED_DIAGNOSTIC_REGISTRY_SHA256 = (
    "c939590da4fe46af9f42774997c322b6c968e2c1b5621541f540ee40ec4f28b7"
)
EXPECTED_DIAGNOSTIC_EVIDENCE = {
    "receiver-read-write": (
        "write through a READ receiver", "first write through this",
        "method receiver declaration",
    ),
    "receiver-move-plain-lvalue": (
        "consume a MOVE receiver through a continuing lvalue", "plain-lvalue method call",
        "MOVE receiver declaration and caller binding",
    ),
    "borrow-origin-ambiguous": (
        "elide a borrowed-return origin with multiple candidates",
        "second eligible signature origin", "borrowed return signature",
    ),
    "borrow-origin-invalid": (
        "bind a borrowed return to an unknown origin", "unresolved origin name",
        "borrowed return signature",
    ),
    "read-escape": (
        "store a READ mutable input into an escaping field",
        "field assignment that retains the input", "READ parameter and destination field",
    ),
    "read-return": (
        "return a READ mutable input as an ordinary owning alias", "ordinary return statement",
        "READ parameter and return declaration",
    ),
    "read-capture": (
        "retain a READ mutable input in a stored or returned closure", "closure escape boundary",
        "READ parameter and capture scope",
    ),
    "read-cross-execution": (
        "capture a READ mutable input in an ordinary go closure",
        "go closure cross-execution boundary", "READ parameter and spawned closure",
    ),
    "read-foreign-unknown": (
        "accept a READ mutable-root foreign edge with UNKNOWN kind and incomplete provenance",
        "first foreign edge with UNKNOWN kind", "SemanticPlan foreign call edge",
    ),
    "read-unknown-target": (
        "pass a READ mutable input through an indirect target set with unknown suspend effect",
        "first missing or inconsistent non-suspending effect",
        "function-type parameter and indirect call",
    ),
    "read-suspend": (
        "hold a READ mutable input across suspension", "first suspension boundary",
        "READ parameter and enclosing function",
    ),
    "ref-escape": (
        "return a REF input", "return statement that escapes the exclusive loan",
        "REF parameter and call-bound loan",
    ),
    "ref-suspend": (
        "hold a REF loan across suspension", "first suspension boundary",
        "REF parameter and call-bound loan",
    ),
    "external-alias-after-scope": (
        "move a domain after an external strong alias existed", "first ordinary alias creation",
        "root ownership domain and alias scope",
    ),
    "view-after-owner-invalidation": (
        "use a view after owner invalidation", "owner mutation that first invalidated the view",
        "view version and owner root",
    ),
    "view-active-conflict": (
        "combine an active view and overlapping REF owner action in one call",
        "overlapping actual argument", "call expression and call-bound loans",
    ),
    "view-escaped": (
        "invalidate an owner while a derived view is escaped",
        "view store into an escaping field", "view origin set and owner root",
    ),
    "view-mutable-return": (
        "return a writable borrowed Slice", "mutable borrowed return type",
        "borrowed return declaration",
    ),
    "domain-edge": (
        "accept a cross-domain plain strong edge as transferable",
        "first cross-domain edge row", "ownership domain plan",
    ),
    "unknown-domain-edge": (
        "accept an incomplete ownership edge", "first edge with unknown kind or provenance",
        "ownership domain plan",
    ),
}

EXPECTED_CASE_DETAILS = {
    "receiver-read-write": (
        "accepted-by-body-inferred-mutates-receiver", "write through this",
        "declare the method with ref",
    ),
    "receiver-move-plain-lvalue": (
        "new-receiver-prefix-not-owned-by-parser", "plain lvalue receiver call",
        "write (move value).finish()",
    ),
    "borrow-origin-ambiguous": (
        "body-derived-origin-selects-one-scalar-source",
        "return declaration with two eligible inputs and no origin set", "declare from a | b",
    ),
    "borrow-origin-invalid": (
        "borrow-origin-syntax-not-owned-by-parser",
        "unknown origin name in the return declaration",
        "name an eligible READ input or return an owned copy",
    ),
    "read-escape": (
        "accepted-by-read-local-alias-retain-summary",
        "store of a READ mutable input into an escaping field",
        "use move, copy, or a call-bound read",
    ),
    "read-return": (
        "accepted-by-read-ordinary-alias-return",
        "ordinary owning return of a READ mutable input",
        "return an owned copy, use MOVE, or declare a const borrowed view origin",
    ),
    "read-capture": (
        "read-mutable-capture-can-outlive-the-call",
        "stored or returned closure capture of a READ mutable input",
        "capture an owned copy or keep the closure call-bound",
    ),
    "read-cross-execution": (
        "ordinary-go-closure-capture-uses-separate-legacy-authority",
        "ordinary go closure capture of a READ mutable input",
        "pass an explicit copy or MOVE argument, or capture typed const or sync data",
    ),
    "read-foreign-unknown": (
        "foreign-read-edge-completeness-row-not-yet-published",
        "first READ mutable-root foreign edge with UNKNOWN kind and incomplete provenance",
        "provide a complete typed foreign retention contract or pass an owned copy",
    ),
    "read-unknown-target": (
        "indirect-target-effect-is-not-a-complete-read-proof",
        "indirect call whose target set lacks one complete non-suspending effect",
        "use a function type with a complete non-suspending effect contract or pass an owned copy",
    ),
    "read-suspend": (
        "suspend-effect-and-read-capability-are-separate-authorities",
        "first suspend boundary crossed by a READ mutable input",
        "use const, copy, move, or a synchronous helper",
    ),
    "ref-escape": (
        "legacy-ref-return-escape-diagnostic", "return of a REF input",
        "finish mutation within the call",
    ),
    "ref-suspend": (
        "general-ref-no-suspend-contract-not-frozen",
        "first suspend boundary crossed by a REF input",
        "transfer ownership or use a non-suspending helper",
    ),
    "external-alias-after-scope": (
        "accepted-by-recoverable-local-alias",
        "move with the first external alias witness",
        "copy the root or construct one owned domain",
    ),
    "view-after-owner-invalidation": (
        "old-checker-rejects-owner-action-instead-of-later-view-use",
        "first view use after owner invalidation",
        "recreate the view after the invalidating action",
    ),
    "view-active-conflict": (
        "legacy-active-loan-checker-may-report-at-owner-argument",
        "call with overlapping owner REF and live view actuals",
        "finish the view read before the owner mutation",
    ),
    "view-escaped": (
        "body-summary-retains-the-view-without-a-canonical-escaped-state",
        "owner invalidation after the view escapes to a field",
        "keep the owner stable or avoid escaping the view",
    ),
    "view-mutable-return": (
        "borrow-origin-and-writable-view-return-syntax-not-owned-by-parser",
        "writable borrowed Slice return declaration",
        "return const Slice<u8> or an owned copy",
    ),
    "domain-edge": (
        "ownership-domain-edge-row-not-yet-published",
        "first cross-domain plain-strong edge in the verified plan",
        "use owned adoption, an ID, weak storage, or one execution domain",
    ),
    "unknown-domain-edge": (
        "ownership-domain-edge-completeness-row-not-yet-published",
        "first incomplete edge in the verified plan", "provide a complete typed contract",
    ),
}

REQUIRED_ROW_FIELDS = {
    "id", "current_owner", "replacement_owner", "deletion_phase", "deletion_boundary",
    "required_locks", "prerequisite_gates", "producers", "consumers", "residue", "witnesses",
}
REQUIRED_CASE_FIELDS = {
    "id", "oracle_kind", "fixture", "sha256", "baseline", "activation_reason", "diagnostic_site",
    "help", "required_locks",
}
EXPECTED_SHORTCUTS = {
    "expected-parser-error", "expected-runtime-gap", "skip", "allowlist", "fallback",
}
FORBIDDEN_MARKERS = (
    "xfail", "skip", "allowlist", "expected-runtime", "expected-parser-error", "fallback",
)
ALLOWED_LOCKS = {
    "LOCK-FRONTEND", "LOCK-SCHEMA", "LOCK-RUNTIME", "LOCK-BUILD-GEN", "LOCK-RESIDUE",
}
EXPECTED_STAGING_SHA256 = {
    "typed_contract_staging.json": "35e77f9a78b4469f8075833436caafbadac9ee3cad597c68b68c61cc67d228ba",
    "atomic_cut_staging.json": "8d9675b268b8d8e1b7a4a9125b6fcac89276a7bb256f84cdde19d2cb21df68fb",
    "activation_gate_staging.json": "45bc7035433457b8b8a185dc709b0cd904e238756ff82821f42987a2da5bf431",
}
EXPECTED_STAGING_FILES = {
    TYPED_CONTRACT_PATH.name, ATOMIC_CUT_PATH.name, ACTIVATION_GATE_PATH.name,
}
EXPECTED_TYPED_AUTHORITY = (
    "non-authoritative executable-oracle input; the integration train owns public activation "
    "after granting every required lock"
)
EXPECTED_CUT_AUTHORITY = (
    "non-authoritative dependency and deletion ledger for one integration-train activation batch"
)
EXPECTED_GATE_AUTHORITY = (
    "non-authoritative executable gate ledger; CURRENT identifies an existing entrypoint and "
    "never a Task 284 PASS"
)
EXPECTED_CURRENT_ENTRYPOINT_SEMANTICS = (
    "CURRENT means the repository has a complete command entrypoint; every gate remains UNRUN "
    "until the atomic public cut and its exact activated assets are present"
)
EXPECTED_GATE_METADATA_CANONICAL_SHA256 = (
    "891cfca521c3d6fe40603a588bdd461046de2eff5b0fa6527bc02c4e99b7ebd7"
)
EXPECTED_CURRENT_GATE_IDS = {
    "matching-default-release-binary",
    "meta-ownership-inventory",
    "contract-freeze-and-hostile-artifact",
    "live-refusal-schema3-row-binding",
}
EXPECTED_BORROW_ELISION = {
    "explicit_set": "bind and validate the declared set without running elision",
    "one_signature_candidate": "normalize to the sole candidate",
    "multiple_signature_candidates": "reject with OWN-E-VIEW-ORIGIN-AMBIGUOUS",
    "zero_signature_candidates": "reject with OWN-E-VIEW-ORIGIN-INVALID unless static is explicit",
}
EXPECTED_AXES = {
    "binding_use": ["UNINITIALIZED", "LIVE", "MOVED", "MAYBE_MOVED", "UNKNOWN"],
    "domain_transfer": ["CANDIDATE", "EXTERNAL_ALIASED", "ESCAPED", "UNKNOWN"],
    "capability": ["MUTABLE", "CONST", "SYNC", "BORROW_READ", "BORROW_WRITE", "UNKNOWN"],
    "loan": ["READ", "WRITE", "RAW", "VIEW", "CAPTURE", "CLEANUP"],
    "view_validity": ["LIVE", "INVALIDATED", "MAYBE_INVALIDATED", "UNKNOWN"],
}
EXPECTED_PARAMETER_MODES = [
    {"symbol": "READ", "value": 0, "maximum_capability": "call-bound readonly use"},
    {
        "symbol": "REF", "value": 1,
        "maximum_capability": "call-bound exclusive read/write place loan",
    },
    {"symbol": "MOVE", "value": 2, "maximum_capability": "unique owner transfer"},
]
EXPECTED_ORIGIN_KINDS = ["PARAM", "RECEIVER", "STATIC"]
EXPECTED_DOMAIN_STATES = [
    ("CANDIDATE", 0), ("EXTERNAL_ALIASED", 1), ("ESCAPED", 2), ("UNKNOWN", 3),
]
EXPECTED_DOMAIN_TRANSITIONS = {
    "CANDIDATE->EXTERNAL_ALIASED",
    "EXTERNAL_ALIASED->ESCAPED",
    "CANDIDATE->UNKNOWN",
    "EXTERNAL_ALIASED->UNKNOWN",
    "ESCAPED->UNKNOWN",
}
EXPECTED_EDGE_KINDS = [
    "INTERNAL_OWNED", "EXTERNAL_STRONG", "BORROW_READ", "BORROW_WRITE", "CONST_SHARED",
    "SYNC_SHARED", "WEAK", "FOREIGN_OWNED", "FOREIGN_BORROWED", "UNKNOWN",
]
EXPECTED_VIEW_STATES = [
    ("LIVE", 0), ("INVALIDATED", 1), ("MAYBE_INVALIDATED", 2), ("UNKNOWN", 3),
]
EXPECTED_VIEW_JOIN_TABLE = [
    {"left": "LIVE", "right": "LIVE", "result": "LIVE"},
    {"left": "LIVE", "right": "INVALIDATED", "result": "MAYBE_INVALIDATED"},
    {"left": "LIVE", "right": "MAYBE_INVALIDATED", "result": "MAYBE_INVALIDATED"},
    {"left": "LIVE", "right": "UNKNOWN", "result": "UNKNOWN"},
    {"left": "INVALIDATED", "right": "LIVE", "result": "MAYBE_INVALIDATED"},
    {"left": "INVALIDATED", "right": "INVALIDATED", "result": "INVALIDATED"},
    {"left": "INVALIDATED", "right": "MAYBE_INVALIDATED", "result": "MAYBE_INVALIDATED"},
    {"left": "INVALIDATED", "right": "UNKNOWN", "result": "UNKNOWN"},
    {"left": "MAYBE_INVALIDATED", "right": "LIVE", "result": "MAYBE_INVALIDATED"},
    {"left": "MAYBE_INVALIDATED", "right": "INVALIDATED", "result": "MAYBE_INVALIDATED"},
    {"left": "MAYBE_INVALIDATED", "right": "MAYBE_INVALIDATED", "result": "MAYBE_INVALIDATED"},
    {"left": "MAYBE_INVALIDATED", "right": "UNKNOWN", "result": "UNKNOWN"},
    {"left": "UNKNOWN", "right": "LIVE", "result": "UNKNOWN"},
    {"left": "UNKNOWN", "right": "INVALIDATED", "result": "UNKNOWN"},
    {"left": "UNKNOWN", "right": "MAYBE_INVALIDATED", "result": "UNKNOWN"},
    {"left": "UNKNOWN", "right": "UNKNOWN", "result": "UNKNOWN"},
]
EXPECTED_SEMANTIC_FACTS = {
    "declared parameter/receiver contract",
    "normalized borrowed-return origin set",
    "binding movedness proof",
    "domain transfer proof",
    "view origin/validity/invalidation plan",
    "call-bound loan scopes",
    "typed closure capture publication and escape action",
    "typed cleanup reachability and drop boundary",
    "ownership edge/domain closure",
    "allocation/storage/drop plan",
    "boundary transfer action",
    "proof/certificate ids",
}
EXPECTED_CUT_ORDER = [
    "source-contract",
    "strict-call-contract",
    "domain-contract",
    "view-contract",
    "signature-validation",
    "plan-and-xi-contract",
    "execution-contract",
    "retirement-contract",
]
EXPECTED_CUT_NODES = {
    "source-contract": (
        "P1", set(), {"LOCK-FRONTEND", "LOCK-SCHEMA", "LOCK-BUILD-GEN"}, set(), set(), {
            "OWN-E-RECEIVER-READ-WRITE", "OWN-E-RECEIVER-MOVE",
            "OWN-E-VIEW-ORIGIN-AMBIGUOUS", "OWN-E-VIEW-ORIGIN-INVALID",
            "OWN-E-VIEW-MUTABLE",
        },
    ),
    "strict-call-contract": (
        "P2", {"source-contract"}, {"LOCK-FRONTEND", "LOCK-SCHEMA"},
        {"read-retain-return-alias", "read-ref-suspend-authority"},
        set(),
        {"OWN-E-READ-ESCAPE", "OWN-E-READ-SUSPEND", "OWN-E-REF-ESCAPE", "OWN-E-REF-SUSPEND"},
    ),
    "domain-contract": (
        "P3", {"strict-call-contract"}, {"LOCK-SCHEMA", "LOCK-RUNTIME", "LOCK-BUILD-GEN"},
        {"recoverable-root-alias", "typed-domain-edge-authority"},
        set(),
        {"OWN-E-EXTERNAL-ALIAS", "OWN-E-DOMAIN-EDGE", "OWN-E-UNKNOWN-EDGE"},
    ),
    "view-contract": (
        "P4", {"domain-contract"}, {"LOCK-SCHEMA", "LOCK-RUNTIME", "LOCK-BUILD-GEN"},
        {"source-view-last-use", "closure-capture-authority"}, set(), {
            "OWN-E-READ-ESCAPE", "OWN-E-UNKNOWN-EDGE",
            "OWN-E-VIEW-INVALIDATED", "OWN-E-VIEW-ACTIVE-CONFLICT", "OWN-E-VIEW-ESCAPED",
        },
    ),
    "signature-validation": (
        "P5", {"source-contract", "strict-call-contract", "view-contract"},
        {"LOCK-FRONTEND", "LOCK-SCHEMA"},
        {"body-inferred-receiver", "body-inferred-borrow-origin"},
        set(),
        {"OWN-E-RECEIVER-READ-WRITE", "OWN-E-VIEW-ORIGIN-INVALID"},
    ),
    "plan-and-xi-contract": (
        "P6", {"signature-validation", "domain-contract", "view-contract"},
        {"LOCK-SCHEMA", "LOCK-BUILD-GEN"}, {"name-driven-ownership-permission"}, set(), {
            "OWN-E-DOMAIN-EDGE", "OWN-E-UNKNOWN-EDGE", "OWN-E-VIEW-INVALIDATED",
        },
    ),
    "execution-contract": (
        "P7", {"plan-and-xi-contract"}, {"LOCK-SCHEMA", "LOCK-RUNTIME", "LOCK-BUILD-GEN"},
        {"move-boundary-graph-retag"}, set(),
        {"OWN-E-DOMAIN-EDGE", "OWN-E-UNKNOWN-EDGE"},
    ),
    "retirement-contract": (
        "P8", set(EXPECTED_CUT_ORDER[:-1]), ALLOWED_LOCKS, set(), set(EXPECTED_ROWS), {
            case[2] for case in EXPECTED_CASES.values()
        },
    ),
}
EXPECTED_CUT_NODE_DETAILS = {
    "source-contract": (
        "receiver modes and BorrowOriginSet are declaration-owned source and type identity",
        [
            "parser and public AST", "function type and PSC",
            "formatter LSP MCP and API projection", "source corpus migration",
        ],
        [
            "receiver parser and formatter KATs", "BorrowOriginSet normalization and roundtrip",
            "interface import and function-value identity",
        ],
    ),
    "strict-call-contract": (
        "declared READ REF MOVE modes bound maximum call capability without body expansion",
        ["call checker", "effect summary", "call-bound loan validation"],
        ["READ non-retaining calls", "REF exclusive place mutation", "MOVE whole-binding transfer"],
    ),
    "domain-contract": (
        "a monotone domain transfer lattice and typed ownership edges decide transfer",
        ["ownership domain solver", "storage plan", "typed native and foreign edge registry"],
        ["candidate domain transfer", "owned DAG adoption", "owned cycle teardown"],
    ),
    "view-contract": (
        "forward view validity and typed owner effects decide source legality",
        [
            "view facts and owner reverse index", "CFG forward joins", "call-bound loan scopes",
            "closure capture facts and escape destinations",
            "cleanup reachability facts and drop boundaries",
        ],
        [
            "owner action invalidates intersecting local views", "branch and loop forward join",
            "view rebind creates a new version",
            "immediate retained adopted and cross-execution captures have one typed classification",
            "cleanup reachability blocks owner actions until its typed lifetime scope expires",
        ],
    ),
    "signature-validation": (
        "method bodies and return paths validate but never create receiver or origin authority",
        [
            "method and override validation", "borrowed return path verification",
            "import and dynamic target matching",
        ],
        [
            "READ REF MOVE receiver body validation",
            "every return provenance belongs to its declared origin set",
            "dynamic target sets have identical normalized contracts",
        ],
    ),
    "plan-and-xi-contract": (
        "SemanticPlan publishes complete ownership facts and Xi consumes them without source inference",
        [
            "SemanticPlan builder verifier and codec", "TargetPlan builder verifier and codec",
            "Xi lowering source-move verifier and ARC",
        ],
        [
            "SemanticPlan build check serialize load replay",
            "TargetPlan representation-only selection", "Xi source move and ARC path balance",
        ],
    ),
    "execution-contract": (
        "VM and AOT execute the same verified O(1) domain handoff and exactly-once drop plan",
        ["VM plan execution", "AOT generated C", "portable runtime headers", "artifact identity"],
        [
            "VM AOT ownership differential", "DAG cycle error and cancel teardown",
            "supported generated-C provider matrix",
        ],
    ),
    "retirement-contract": (
        "all covered legacy authorities and compatibility residue are absent after the typed owner is proven",
        [
            "legacy symbol and text inventory", "contract anchors and digests",
            "cache and artifact version", "completion baseline",
        ],
        [
            "all inventory residue reaches its declared zero replacement or bound",
            "contract freeze and hostile artifact checks",
            "schema-3 live refusal census has exact row-local diagnostic bindings and no evidence debt or ratchet drift",
            "full sanitizer differential and provider gates",
        ],
    ),
}
EXPECTED_ACTIVATION_GATES = [
    (
        "matching-default-release-binary", "before activation",
        "stdlib/sync/sync.xr XR_SEM_0019 blocks the current train binary",
    ),
    (
        "semantic-plan-build-check-replay", "plan-and-xi-contract",
        "typed ownership rows are not publicly activated",
    ),
    (
        "semantic-and-target-plan-hostile-input", "plan-and-xi-contract",
        "typed ownership codecs and loaders are not publicly activated",
    ),
    (
        "vm-aot-same-plan-differential", "execution-contract",
        "VM and AOT typed ownership consumers do not exist",
    ),
    (
        "o1-handoff-no-graph-walk", "execution-contract",
        "visited_node_count=0 and no-copy code shape require the activated runtime path",
    ),
    (
        "handoff-cleanup-drop-once", "execution-contract",
        "channel close full timeout cancel task-failure and drop-once paths are not activated",
    ),
    (
        "meta-ownership-inventory", "retirement-contract",
        "must rerun against the activated final tree",
    ),
    (
        "generated-c-w1-w4", "retirement-contract",
        "generated ownership paths are not activated",
    ),
    (
        "contract-freeze-and-hostile-artifact", "retirement-contract",
        "schema cache artifact and contract identities are not activated",
    ),
    (
        "live-refusal-schema3-row-binding", "retirement-contract",
        "requires clean current source and an exact Ninja Release VM-fastpaths-off matching "
        "binary and provider, schema 3 exclusively, exactly one registered diagnostic on every "
        "source-emitted raw-log row, zero evidence debt, and zero refusal-ratchet drift",
    ),
    (
        "asan-focused", "retirement-contract",
        "requires the independent asan_focused compiler ASan and UBSan build of the activated tree",
    ),
    (
        "aot-ubsan-generated-output", "retirement-contract",
        "requires aot_ubsan to instrument and execute real generated native output",
    ),
    (
        "lsan-strict", "retirement-contract",
        "requires the independent lsan_strict supported-host leak build of the activated tree",
    ),
    (
        "tsan-focused-if-applicable", "retirement-contract",
        "requires the independent tsan_focused build when the activated runtime path is supported",
    ),
    (
        "provider-appleclang", "retirement-contract",
        "requires real generated C on an AppleClang host",
    ),
    ("provider-clang", "retirement-contract", "requires real generated C on a Clang host"),
    ("provider-gcc", "retirement-contract", "requires real generated C on a GCC host"),
    ("provider-msvc", "retirement-contract", "requires real generated C on an MSVC Windows host"),
    ("provider-zig", "retirement-contract", "requires real generated C on a Zig provider host"),
    (
        "source-corpus-positive-negative", "retirement-contract",
        "staged source fixtures are not activated in the positive and compile-error corpora",
    ),
    (
        "parser-formatter-type-tooling-identity", "retirement-contract",
        "receiver and borrowed-origin syntax type identity formatter LSP MCP and API projections "
        "are not activated",
    ),
    (
        "legacy-oracle-differential", "retirement-contract",
        "the exhaustive pre-cut to post-cut observation classification manifest does not exist",
    ),
    (
        "ownership-performance-manifest", "retirement-contract",
        "Task 284 ownership counters benchmark manifest and approved W0 budgets are not activated",
    ),
    (
        "ownership-residue-zero", "retirement-contract",
        "production residue scanner and completion rows cannot qualify until the atomic public cut",
    ),
]
EXPECTED_EXTERNAL_DEPENDENCIES = [
    {
        "id": "i1-r4-checkpoint",
        "owner": "integration train 01a0510c-59b4-7471-9945-a396121e3019",
        "status": "WAITING",
    },
    {
        "id": "docs-sync", "owner": "docs task 01a0510d-17d5-7130-ac01-c45bd1c5dca8",
        "status": "WAITING",
    },
    {
        "id": "ports-downstream", "owner": "integration-train coordinated downstream lane",
        "status": "WAITING",
    },
]


class ValidationError(Exception):
    """A staged fact is incomplete, stale, or attempts to weaken the oracle."""


def load_json(path: Path) -> dict[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValidationError(f"cannot read {path.relative_to(ROOT)}: {exc}") from exc
    if not isinstance(value, dict):
        raise ValidationError(f"{path.relative_to(ROOT)} must contain one object")
    return value


def git_output(*args: str) -> str:
    result = subprocess.run(
        ["git", *args], cwd=ROOT, check=False, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True,
    )
    if result.returncode != 0:
        raise ValidationError(f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout.strip()


def validate_checkout(require_clean: bool) -> None:
    if git_output("rev-parse", f"{FROZEN_COMMIT}^{{tree}}") != FROZEN_TREE:
        raise ValidationError("frozen base commit no longer names the recorded tree")
    if git_output("rev-parse", f"{TRAIN_COMMIT}^{{tree}}") != TRAIN_TREE:
        raise ValidationError("train base commit no longer names the recorded tree")
    result = subprocess.run(
        ["git", "merge-base", "--is-ancestor", TRAIN_COMMIT, "HEAD"], cwd=ROOT,
        check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    if result.returncode != 0:
        raise ValidationError("train base is not an ancestor of the current checkout")
    if require_clean and git_output("status", "--porcelain", "--untracked-files=all"):
        raise ValidationError("current checkout is dirty")


def require_exact_ids(items: object, expected: set[str], label: str) -> list[dict[str, object]]:
    if not isinstance(items, list) or not all(isinstance(item, dict) for item in items):
        raise ValidationError(f"{label} must be an object list")
    typed_items = list(items)
    ids = [item.get("id") for item in typed_items]
    if any(not isinstance(item_id, str) or not item_id for item_id in ids):
        raise ValidationError(f"{label} contains an empty id")
    if len(ids) != len(set(ids)):
        raise ValidationError(f"{label} contains a duplicate id")
    actual = set(ids)
    if actual != expected:
        raise ValidationError(
            f"{label} id mismatch: missing={sorted(expected - actual)} extra={sorted(actual - expected)}"
        )
    return typed_items


def require_text(record: dict[str, object], field: str, label: str) -> str:
    value = record.get(field)
    if not isinstance(value, str) or not value.strip():
        raise ValidationError(f"{label}.{field} must be non-empty text")
    return value


def require_text_list(record: dict[str, object], field: str, label: str) -> list[str]:
    value = record.get(field)
    if not isinstance(value, list) or not value or not all(isinstance(x, str) and x for x in value):
        raise ValidationError(f"{label}.{field} must be a non-empty text list")
    return value


def require_exact_keys(record: dict[str, object], expected: set[str], label: str) -> None:
    actual = set(record)
    if actual != expected:
        raise ValidationError(
            f"{label} key mismatch: missing={sorted(expected - actual)} extra={sorted(actual - expected)}"
        )


def require_locks(record: dict[str, object], expected: set[str], label: str) -> None:
    locks = record.get("required_locks")
    if not isinstance(locks, list) or not all(isinstance(lock, str) for lock in locks):
        raise ValidationError(f"{label} has invalid shared locks")
    actual = set(locks)
    if len(locks) != len(actual) or not actual <= ALLOWED_LOCKS or actual != expected:
        raise ValidationError(
            f"{label} shared lock mismatch: expected={sorted(expected)} actual={sorted(actual)}"
        )


def validate_anchor(record: object, label: str) -> None:
    if not isinstance(record, dict):
        raise ValidationError(f"{label} must be an object")
    path_text = require_text(record, "path", label)
    anchor = require_text(record, "anchor", label)
    occurrences = record.get("occurrences")
    if not isinstance(occurrences, int) or occurrences <= 0:
        raise ValidationError(f"{label}.occurrences must be a positive integer")
    path = ROOT / path_text
    if not path.is_file():
        raise ValidationError(f"{label} path does not exist: {path_text}")
    actual = path.read_text(encoding="utf-8", errors="replace").count(anchor)
    if actual != occurrences:
        raise ValidationError(
            f"{label} anchor count changed: {path_text}: expected={occurrences} actual={actual}: {anchor}"
        )


def validate_residue(record: object, label: str) -> None:
    validate_anchor(record, label)
    assert isinstance(record, dict)
    disposition = require_text(record, "post_cut_disposition", label)
    if disposition not in {"delete-zero", "replace-with-new-owner", "retain-bounded"}:
        raise ValidationError(f"{label} has invalid post-cut disposition: {disposition}")
    require_text(record, "post_cut_expectation", label)
    if disposition in {"delete-zero", "replace-with-new-owner"}:
        require_exact_keys(record, {
            "path", "anchor", "occurrences", "post_cut_disposition",
            "post_cut_occurrences", "post_cut_expectation",
        }, label)
        if record.get("post_cut_occurrences") != 0:
            raise ValidationError(f"{label} must require zero old-anchor occurrences")
    else:
        require_exact_keys(record, {
            "path", "anchor", "occurrences", "post_cut_disposition",
            "post_cut_occurrences_max", "post_cut_expectation",
        }, label)
        bound = record.get("post_cut_occurrences_max")
        if not isinstance(bound, int) or bound < 0:
            raise ValidationError(f"{label} retained residue needs a non-negative bound")


def validate_witness(record: object, label: str) -> None:
    validate_anchor(record, label)
    assert isinstance(record, dict)
    require_exact_keys(record, {
        "path", "anchor", "occurrences", "baseline_expected", "post_cut_expected",
    }, label)
    require_text(record, "baseline_expected", label)
    require_text(record, "post_cut_expected", label)


def validate_inventory(data: dict[str, object]) -> None:
    if data.get("schema") != "ownership-source-contract-boundary/1":
        raise ValidationError("inventory schema is not exact")
    if data.get("activation") != "staging-only-before-i1-r4":
        raise ValidationError("inventory activation boundary changed")
    base = data.get("frozen_base")
    if not isinstance(base, dict) or base.get("commit") != FROZEN_COMMIT:
        raise ValidationError("inventory base commit changed")
    if base.get("tree") != FROZEN_TREE:
        raise ValidationError("inventory base tree changed")
    train_base = data.get("current_train_base")
    if not isinstance(train_base, dict) or train_base.get("commit") != TRAIN_COMMIT:
        raise ValidationError("inventory train base commit changed")
    if train_base.get("tree") != TRAIN_TREE:
        raise ValidationError("inventory train base tree changed")
    require_text(data, "owned_fact", "inventory")
    for field in ("allowed_layers", "shared_hotspots", "explicitly_unowned"):
        require_text_list(data, field, "inventory")

    surfaces = require_exact_ids(
        data.get("boundary_surfaces"), set(EXPECTED_SURFACES), "inventory.boundary_surfaces"
    )
    for surface in surfaces:
        surface_id = require_text(surface, "id", "inventory surface")
        require_exact_keys(surface, {
            "id", "current_owner", "replacement_owner", "single_writer", "activation_boundary",
        }, f"inventory surface {surface_id}")
        for field in ("current_owner", "replacement_owner", "single_writer", "activation_boundary"):
            require_text(surface, field, f"inventory surface {surface_id}")
        if surface.get("single_writer") != EXPECTED_SURFACES[surface_id]:
            raise ValidationError(f"inventory surface {surface_id} single-writer mapping changed")

    rows = require_exact_ids(data.get("rows"), set(EXPECTED_ROWS), "inventory.rows")
    for row in rows:
        row_id = require_text(row, "id", "inventory row")
        require_exact_keys(row, REQUIRED_ROW_FIELDS, f"inventory row {row_id}")
        phase, expected_locks = EXPECTED_ROWS[row_id]
        if row.get("deletion_phase") != phase:
            raise ValidationError(f"inventory row {row_id} deletion phase changed")
        require_locks(row, expected_locks, f"inventory row {row_id}")
        for field in ("current_owner", "replacement_owner", "deletion_boundary"):
            require_text(row, field, f"inventory row {row_id}")
        require_text_list(row, "prerequisite_gates", f"inventory row {row_id}")
        for group in ("producers", "consumers"):
            anchors = row.get(group)
            if not isinstance(anchors, list) or not anchors:
                raise ValidationError(f"inventory row {row_id} has no {group}")
            for index, anchor in enumerate(anchors):
                if isinstance(anchor, dict):
                    require_exact_keys(
                        anchor, {"path", "anchor", "occurrences"},
                        f"inventory row {row_id}.{group}[{index}]",
                    )
                validate_anchor(anchor, f"inventory row {row_id}.{group}[{index}]")
        residue = row.get("residue")
        if not isinstance(residue, list) or not residue:
            raise ValidationError(f"inventory row {row_id} has no residue")
        for index, anchor in enumerate(residue):
            validate_residue(anchor, f"inventory row {row_id}.residue[{index}]")
        witnesses = row.get("witnesses")
        if not isinstance(witnesses, list) or not witnesses:
            raise ValidationError(f"inventory row {row_id} has no witnesses")
        for index, witness in enumerate(witnesses):
            validate_witness(witness, f"inventory row {row_id}.witnesses[{index}]")
        if row_id == "source-view-last-use":
            retained_cleanup = {
                (
                    item.get("path"), item.get("anchor"), item.get("occurrences"),
                    item.get("post_cut_occurrences_max"),
                )
                for item in residue if isinstance(item, dict)
                and item.get("post_cut_disposition") == "retain-bounded"
            }
            if retained_cleanup != EXPECTED_RETAINED_CLEANUP_RESIDUE:
                raise ValidationError("cleanup reachability residue boundary changed")
            cleanup_witnesses = {
                (item.get("path"), item.get("anchor"), item.get("occurrences"))
                for item in witnesses if isinstance(item, dict)
            }
            if cleanup_witnesses != EXPECTED_CLEANUP_WITNESSES:
                raise ValidationError("cleanup reachability witness inventory changed")
        if row_id == "closure-capture-authority":
            for group in ("producers", "consumers", "witnesses"):
                records = row.get(group)
                assert isinstance(records, list)
                actual = {
                    (item.get("path"), item.get("anchor"), item.get("occurrences"))
                    for item in records if isinstance(item, dict)
                }
                if actual != EXPECTED_CLOSURE_ANCHORS[group]:
                    raise ValidationError(f"closure capture {group} inventory changed")
            closure_residue = {
                (
                    item.get("path"), item.get("anchor"), item.get("occurrences"),
                    item.get("post_cut_disposition"),
                )
                for item in residue if isinstance(item, dict)
            }
            if closure_residue != EXPECTED_CLOSURE_RESIDUE:
                raise ValidationError("closure capture residue inventory changed")


def validate_mutation_record(
    mutation: dict[str, object], reason: str, fixture_name: str, label: str,
) -> None:
    require_exact_keys(mutation, {
        "schema", "expected_reason", "source_precondition", "mutations",
        "required_verifier_checks",
    }, label)
    if mutation.get("schema") != "ownership-semantic-plan-mutation/1":
        raise ValidationError(f"{label} mutation schema changed")
    if mutation.get("expected_reason") != reason:
        raise ValidationError(f"{label} mutation reason changed")
    details = (
        require_text(mutation, "source_precondition", label),
        tuple(require_text_list(mutation, "mutations", label)),
        tuple(require_text_list(mutation, "required_verifier_checks", label)),
    )
    if details != EXPECTED_MUTATION_DETAILS[fixture_name]:
        raise ValidationError(f"{label} mutation contract changed")
    mutation_text = json.dumps(mutation, sort_keys=True).lower()
    for marker in FORBIDDEN_MARKERS:
        if marker in mutation_text:
            raise ValidationError(f"{label} mutation weakens the gate with {marker}")


def validate_mutation_fixture(path: Path, reason: str, label: str) -> None:
    validate_mutation_record(load_json(path), reason, path.name, label)


def validate_positive_fixtures() -> None:
    actual = {path.name for path in POSITIVE_FIXTURE_DIR.glob("*.xr")}
    if actual != set(EXPECTED_POSITIVE_FIXTURE_SHA256):
        raise ValidationError("positive ownership fixture inventory changed")
    for name, expected_sha256 in EXPECTED_POSITIVE_FIXTURE_SHA256.items():
        path = POSITIVE_FIXTURE_DIR / name
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected_sha256:
            raise ValidationError(f"positive ownership fixture content changed: {name}")
        source = path.read_text(encoding="utf-8")
        if not source.strip() or "fn main()" not in source or not source.rstrip().endswith("main()"):
            raise ValidationError(f"positive ownership fixture is incomplete: {name}")


def validate_oracles(data: dict[str, object]) -> None:
    require_exact_keys(data, {
        "schema", "activation", "forbidden_activation_shortcuts",
        "diagnostic_code_reservation", "diagnostic_evidence", "cases", "existing_harnesses",
    }, "negative oracles")
    if data.get("schema") != "ownership-negative-oracle-staging/1":
        raise ValidationError("oracle schema is not exact")
    if data.get("activation") != "not-runnable-before-public-contract-cut":
        raise ValidationError("oracle activation boundary changed")
    shortcuts = data.get("forbidden_activation_shortcuts")
    if not isinstance(shortcuts, list) or set(shortcuts) != EXPECTED_SHORTCUTS:
        raise ValidationError("oracle forbidden activation shortcuts changed")
    reservation = data.get("diagnostic_code_reservation")
    if not isinstance(reservation, dict):
        raise ValidationError("diagnostic code reservation must be an object")
    require_exact_keys(reservation, {
        "state", "registry", "registry_sha256", "required_locks", "reservation_rule",
        "projection_rule",
    }, "diagnostic code reservation")
    if reservation.get("state") != "UNRESERVED_UNTIL_SHARED_LOCKS":
        raise ValidationError("diagnostic code reservation claims authority before shared locks")
    registry_name = require_text(reservation, "registry", "diagnostic code reservation")
    if registry_name != "contracts/target-machine/diagnostic-codes.toml":
        raise ValidationError("diagnostic code registry owner changed")
    registry_path = ROOT / registry_name
    if hashlib.sha256(registry_path.read_bytes()).hexdigest() != EXPECTED_DIAGNOSTIC_REGISTRY_SHA256:
        raise ValidationError("diagnostic registry drift requires a fresh collision census")
    if reservation.get("registry_sha256") != EXPECTED_DIAGNOSTIC_REGISTRY_SHA256:
        raise ValidationError("diagnostic registry census hash changed")
    require_locks(
        reservation, {"LOCK-SCHEMA", "LOCK-BUILD-GEN"}, "diagnostic code reservation"
    )
    if reservation.get("reservation_rule") != (
        "allocate one non-conflicting numeric E code for every task-local reason in one locked batch"
    ):
        raise ValidationError("diagnostic numeric-code reservation rule changed")
    if reservation.get("projection_rule") != (
        "update the registry, generated projections, reason-to-code mapping, and contract anchors "
        "atomically"
    ):
        raise ValidationError("diagnostic code projection rule changed")

    evidence = data.get("diagnostic_evidence")
    if not isinstance(evidence, list) or not all(isinstance(row, dict) for row in evidence):
        raise ValidationError("diagnostic evidence must be an object list")
    evidence_by_case: dict[str, dict[str, object]] = {}
    for row in evidence:
        require_exact_keys(row, {
            "case", "rejected_action", "first_witness", "related_declaration_or_scope",
        }, "diagnostic evidence row")
        case_id = require_text(row, "case", "diagnostic evidence row")
        if case_id in evidence_by_case:
            raise ValidationError(f"duplicate diagnostic evidence case: {case_id}")
        evidence_by_case[case_id] = row
    if set(evidence_by_case) != set(EXPECTED_DIAGNOSTIC_EVIDENCE):
        raise ValidationError("diagnostic evidence case coverage changed")
    for case_id, expected in EXPECTED_DIAGNOSTIC_EVIDENCE.items():
        row = evidence_by_case[case_id]
        actual = (
            row.get("rejected_action"), row.get("first_witness"),
            row.get("related_declaration_or_scope"),
        )
        if actual != expected:
            raise ValidationError(f"diagnostic evidence contract changed: {case_id}")
    cases = require_exact_ids(data.get("cases"), set(EXPECTED_CASES), "oracle.cases")
    fixture_names: set[str] = set()
    for case in cases:
        case_id = require_text(case, "id", "oracle case")
        require_exact_keys(case, REQUIRED_CASE_FIELDS, f"oracle case {case_id}")
        expected_kind, expected_fixture, expected_reason, expected_locks = EXPECTED_CASES[case_id]
        kind = require_text(case, "oracle_kind", f"oracle case {case_id}")
        fixture_name = require_text(case, "fixture", f"oracle case {case_id}")
        reason = require_text(case, "activation_reason", f"oracle case {case_id}")
        if (kind, fixture_name, reason) != (expected_kind, expected_fixture, expected_reason):
            raise ValidationError(f"oracle case {case_id} binding changed")
        require_locks(case, expected_locks, f"oracle case {case_id}")
        if fixture_name in fixture_names:
            raise ValidationError(f"oracle fixture reused: {fixture_name}")
        fixture_names.add(fixture_name)
        fixture = HERE / fixture_name
        if fixture.parent != HERE or not fixture.is_file():
            raise ValidationError(f"oracle fixture is missing or escapes staging: {fixture_name}")
        expected_sha256 = EXPECTED_FIXTURE_SHA256.get(fixture_name)
        recorded_sha256 = require_text(case, "sha256", f"oracle case {case_id}")
        actual_sha256 = hashlib.sha256(fixture.read_bytes()).hexdigest()
        if recorded_sha256 != expected_sha256 or actual_sha256 != expected_sha256:
            raise ValidationError(f"oracle fixture content changed: {fixture_name}")
        if kind == "source":
            source = fixture.read_text(encoding="utf-8")
            if not source.strip() or "fn main()" not in source or not source.rstrip().endswith("main()"):
                raise ValidationError(f"oracle fixture is not a complete source case: {fixture_name}")
            lowered = source.lower()
            for marker in FORBIDDEN_MARKERS:
                if marker in lowered:
                    raise ValidationError(f"oracle fixture weakens the gate with {marker}: {fixture_name}")
        elif kind == "semantic-plan-mutation":
            validate_mutation_fixture(fixture, reason, f"oracle case {case_id}")
        else:
            raise ValidationError(f"oracle case {case_id} has unknown kind: {kind}")
        details = tuple(
            require_text(case, field, f"oracle case {case_id}")
            for field in ("baseline", "diagnostic_site", "help")
        )
        if details != EXPECTED_CASE_DETAILS[case_id]:
            raise ValidationError(f"oracle case {case_id} evidence text changed")
        case_text = json.dumps(case, sort_keys=True).lower()
        for marker in FORBIDDEN_MARKERS:
            if marker in case_text:
                raise ValidationError(f"oracle metadata weakens the gate with {marker}: {case_id}")

    actual_sources = {path.name for path in HERE.glob("*.xr")}
    actual_mutations = {path.name for path in HERE.glob("*.mutation.json")}
    if actual_sources | actual_mutations != fixture_names:
        raise ValidationError(
            "staged fixture registration mismatch: "
            f"missing={sorted((actual_sources | actual_mutations) - fixture_names)} "
            f"stale={sorted(fixture_names - (actual_sources | actual_mutations))}"
        )
    harnesses = data.get("existing_harnesses")
    if not isinstance(harnesses, list) or set(harnesses) != EXPECTED_HARNESSES:
        raise ValidationError("existing harness inventory changed")
    for harness in harnesses:
        if not isinstance(harness, str) or not (ROOT / harness).is_file():
            raise ValidationError(f"stale existing harness: {harness}")


def require_symbol_values(records: object, expected: list[tuple[str, int]], label: str) -> None:
    if not isinstance(records, list) or not all(isinstance(record, dict) for record in records):
        raise ValidationError(f"{label} must be an object list")
    actual = [(record.get("symbol"), record.get("value")) for record in records]
    if actual != expected:
        raise ValidationError(f"{label} symbolic values changed: expected={expected} actual={actual}")


def validate_staging_bytes(name: str, content: bytes, expected: str) -> None:
    if hashlib.sha256(content).hexdigest() != expected:
        raise ValidationError(f"typed staging content changed: {name}")


def validate_staging_hash_coverage(expected: dict[str, str]) -> None:
    if set(expected) != EXPECTED_STAGING_FILES:
        raise ValidationError("typed staging hash coverage changed")


def validate_staging_file_hashes() -> None:
    validate_staging_hash_coverage(EXPECTED_STAGING_SHA256)
    for name, expected in EXPECTED_STAGING_SHA256.items():
        path = HERE / name
        if not path.is_file():
            raise ValidationError(f"typed staging content is missing: {name}")
        validate_staging_bytes(name, path.read_bytes(), expected)


def validate_typed_contract(data: dict[str, object]) -> None:
    require_exact_keys(data, {
        "schema", "activation", "authority", "train_base", "public_activation_forbidden_without",
        "forbidden_compatibility", "axes", "parameter_modes", "receiver_contract",
        "borrow_origin_contract", "closure_contract", "domain_transfer_contract", "ownership_edge_contract",
        "view_validity_contract", "semantic_plan_contract", "target_plan_contract",
    }, "typed contract")
    if data.get("schema") != "ownership-typed-contract-staging/1":
        raise ValidationError("typed contract schema is not exact")
    if data.get("activation") != "staging-only-before-i1-r4":
        raise ValidationError("typed contract activation boundary changed")
    authority = require_text(data, "authority", "typed contract")
    if authority != EXPECTED_TYPED_AUTHORITY:
        raise ValidationError("typed contract claims public authority")
    train_base = data.get("train_base")
    if not isinstance(train_base, dict) or train_base != {"commit": TRAIN_COMMIT, "tree": TRAIN_TREE}:
        raise ValidationError("typed contract train base changed")
    activation_requirements = require_text_list(
        data, "public_activation_forbidden_without", "typed contract"
    )
    if set(activation_requirements) != ALLOWED_LOCKS | {"I1/R4 clean checkpoint"}:
        raise ValidationError("typed contract activation requirements changed")
    compatibility = set(require_text_list(data, "forbidden_compatibility", "typed contract"))
    required_compatibility = {
        "alias", "shim", "legacy schema reader", "dual write", "dual read", "fallback selector",
        "migration flag", "second planner", "second executor", "second cache", "transition facade",
    }
    if compatibility != required_compatibility:
        raise ValidationError("typed contract compatibility prohibition changed")

    axes = data.get("axes")
    if axes != EXPECTED_AXES:
        raise ValidationError("typed contract semantic axes changed")
    if data.get("parameter_modes") != EXPECTED_PARAMETER_MODES:
        raise ValidationError("parameter mode contract changed")

    receiver = data.get("receiver_contract")
    if not isinstance(receiver, dict) or receiver.get("modes") != ["READ", "REF", "MOVE"]:
        raise ValidationError("receiver contract modes changed")
    require_exact_keys(receiver, {
        "ast_field", "type_field", "modes", "default_mode", "identity_owner", "body_role",
        "fixed_forms", "identity_consumers", "override_rule", "move_invocation_rules",
        "value_type_move_rule", "forbidden_authorities",
    }, "receiver contract")
    if receiver.get("ast_field") != "AstMethodDecl.receiver_mode":
        raise ValidationError("receiver AST owner changed")
    if receiver.get("type_field") != "XrFunctionType.receiver_mode":
        raise ValidationError("receiver type owner changed")
    if receiver.get("default_mode") != "omitted receiver mode normalizes to READ":
        raise ValidationError("receiver default mode changed")
    if receiver.get("identity_owner") != "method or interface declaration and function type":
        raise ValidationError("receiver identity owner changed")
    if receiver.get("body_role") != "validate implementation effects do not exceed the declaration":
        raise ValidationError("receiver body role changed")
    if receiver.get("fixed_forms") != [
        "static and receiver mode are mutually exclusive",
        "constructor owns construction-only mutable this and declares no receiver mode",
        "computed-property getter is READ", "syntactic setter is REF",
        "sync-interior mutation is a sealed typed-registry capability and adds no public mode",
    ]:
        raise ValidationError("receiver fixed-form contract changed")
    if receiver.get("identity_consumers") != [
        "interface", "override", "method item/value", "API fingerprint",
    ]:
        raise ValidationError("receiver identity consumer set changed")
    if receiver.get("override_rule") != (
        "receiver mode must exactly match the base or interface declaration"
    ):
        raise ValidationError("receiver override rule changed")
    if receiver.get("move_invocation_rules") != [
        "fresh and explicit copy results may call a MOVE receiver directly",
        "an existing continuing lvalue must use (move value).method()",
        "a plain continuing lvalue call rejects with OWN-E-RECEIVER-MOVE",
        "a successful MOVE receiver call consumes the whole receiver binding exactly once",
    ]:
        raise ValidationError("MOVE receiver invocation contract changed")
    if receiver.get("value_type_move_rule") != (
        "Copy and value-struct receivers may not declare or invoke MOVE receiver mode"
    ):
        raise ValidationError("value receiver MOVE prohibition changed")
    receiver_forbidden = set(require_text_list(receiver, "forbidden_authorities", "receiver contract"))
    if receiver_forbidden != {
        "recursive body scan", "method name", "mutates_receiver boolean", "backend inference",
    }:
        raise ValidationError("receiver forbidden authority set changed")

    origins = data.get("borrow_origin_contract")
    if not isinstance(origins, dict):
        raise ValidationError("borrow origin contract must be an object")
    require_exact_keys(origins, {
        "syntax_field", "ast_origin_array", "function_type_named_params", "normalized_type_field",
        "elision_type_field", "syntax_states", "ast_origin_kinds", "canonical_origin_kinds",
        "canonical_identity", "kind_order", "eligibility", "ineligible", "elision",
        "identity_exclusions", "required_invariants", "return_surface_constraints",
        "caller_root_mapping", "multi_origin_invalidation", "result_lifetime_rules",
        "const_binding_orthogonality",
    }, "borrow origin contract")
    if origins.get("syntax_field") != "AstFunctionDecl.borrow_origin_syntax":
        raise ValidationError("borrow origin syntax owner changed")
    if origins.get("ast_origin_array") != "AstBorrowOriginRef[]":
        raise ValidationError("borrow origin AST row owner changed")
    if origins.get("function_type_named_params") != (
        "AstFunctionType.named_params are optional non-semantic names"
    ):
        raise ValidationError("borrow origin named-parameter role changed")
    if origins.get("normalized_type_field") != (
        "XrFunctionType.view_origin_set[] stores canonical (kind, parameter ordinal) rows"
    ):
        raise ValidationError("borrow origin normalized type field changed")
    if origins.get("elision_type_field") != (
        "XrFunctionType.view_origin_was_elided is diagnostic/source-map data and not type identity"
    ):
        raise ValidationError("borrow origin elision-field role changed")
    if origins.get("syntax_states") != ["OMITTED", "EXPLICIT_SET"]:
        raise ValidationError("borrow origin syntax states changed")
    if origins.get("ast_origin_kinds") != ["PARAM_NAME", "RECEIVER", "STATIC"]:
        raise ValidationError("borrow origin AST kinds changed")
    if origins.get("canonical_origin_kinds") != EXPECTED_ORIGIN_KINDS:
        raise ValidationError("borrow origin canonical kinds changed")
    if origins.get("kind_order") != EXPECTED_ORIGIN_KINDS:
        raise ValidationError("borrow origin canonical order changed")
    if origins.get("canonical_identity") != "sorted and deduplicated (kind, parameter ordinal) set":
        raise ValidationError("borrow origin identity changed")
    if origins.get("eligibility") != [
        "READ parameter whose input type is a total type-system match for the returned const Slice backing storage",
        "READ receiver whose input type is a total type-system match for the returned const Slice backing storage",
        "verified static immutable storage",
    ]:
        raise ValidationError("borrow origin eligibility judgement changed")
    if set(require_text_list(origins, "ineligible", "borrow origin contract")) != {
        "REF", "MOVE", "local", "temporary", "foreign unknown",
    }:
        raise ValidationError("borrow origin eligibility boundary changed")
    elision = origins.get("elision")
    if elision != EXPECTED_BORROW_ELISION:
        raise ValidationError("borrow origin elision outcomes changed")
    if set(require_text_list(origins, "identity_exclusions", "borrow origin contract")) != {
        "parameter name", "source spelling order", "view_origin_was_elided", "function body",
        "call-site value flow",
    }:
        raise ValidationError("borrow origin identity exclusions changed")
    if set(require_text_list(origins, "required_invariants", "borrow origin contract")) != {
        "borrowed return origin set is nonempty", "parameter ordinals are in range",
        "parameter and receiver origins retain READ mode",
        "each return-path provenance is a member of the normalized set",
        "static provenance creates no invalidatable caller root",
    }:
        raise ValidationError("borrow origin verifier invariants changed")
    if set(require_text_list(origins, "return_surface_constraints", "borrow origin contract")) != {
        "the first public cut permits only direct const Slice<T> borrowed returns",
        "tuple union enum and struct may not conceal a borrowed return",
        "local temporary and unknown provenance cannot impersonate static or input provenance",
        "static origin storage is readonly and lives for the program or module use period",
        "interface extern bodyless declaration function value import sidecar and dynamic target normalize from the signature",
        "every dynamic target has an exactly equal normalized origin set",
        "missing sidecar or inconsistent target set fails closed",
    }:
        raise ValidationError("borrowed return surface contract changed")
    if origins.get("caller_root_mapping") != [
        "map each PARAM origin ordinal to the corresponding caller actual RootId",
        "map RECEIVER to the caller receiver RootId",
        "expand a view actual to its canonical origin RootId set",
        "STATIC contributes no invalidatable caller RootId",
        "union then sort and deduplicate every mapped RootId",
        "unknown missing or moved actual provenance fails closed",
    ]:
        raise ValidationError("borrowed return caller-root mapping changed")
    if origins.get("multi_origin_invalidation") != (
        "an invalidating action on any mapped RootId invalidates the returned view; no flag branch runtime tag or observed path may narrow the normalized origin set"
    ):
        raise ValidationError("borrowed return multi-origin invalidation changed")
    if origins.get("result_lifetime_rules") != [
        "an unbound full-expression result becomes unreachable at full-expression end without source last-use inference",
        "binding a result creates a new view version managed by forward validity",
        "every later assignment or rebind creates another version and never lexically unions historical origins",
    ]:
        raise ValidationError("borrowed return result-lifetime contract changed")
    if origins.get("const_binding_orthogonality") != [
        "const Slice return capability is independent of caller binding mutability",
        "var may rebind a const Slice but may not mutate through it or drop const",
        "const may neither rebind nor mutate through the const Slice",
        "assigning a const Slice result to mutable Slice or otherwise dropping const rejects",
        "const Slice may be passed to READ but rejects REF MOVE and ordinary REF or MOVE receiver calls",
        "never-rebound var produces only a non-semantic preference hint",
    ]:
        raise ValidationError("borrowed return const-binding contract changed")

    closure = data.get("closure_contract")
    if not isinstance(closure, dict) or closure.get("type") != "XaClosureCaptureFact":
        raise ValidationError("closure capture owner changed")
    require_exact_keys(closure, {
        "type", "required_fields", "classification_rules", "syntax_rule", "plan_owner",
        "consumer_rule", "forbidden_authorities",
    }, "closure capture contract")
    if closure.get("required_fields") != [
        "closure value id", "captured source RootId", "capture capability", "storage domain",
        "escape destination set", "call-bound bit", "cross-execution bit",
        "creation and first escape sites", "complete and unknown reason",
    ]:
        raise ValidationError("closure capture required fields changed")
    if closure.get("classification_rules") != [
        "an immediate closure literal passed to a strict READ non-retaining formal creates a call-bound READ borrow that ends when the enclosing call returns",
        "assignment to a binding return container store retain formal and unknown formal each classify an ordinary mutable-root capture as an external strong alias and escape",
        "a consuming closure factory may adopt a MOVE formal into the closure domain and consumes the caller source exactly once",
        "an ordinary go closure rejects capture of an outer mutable var even when the body only reads it",
        "const and sync closure capture follows typed capability while explicit go task and channel argument transfer uses a MOVE boundary plan",
        "unknown retention target capability or storage fails closed",
    ]:
        raise ValidationError("closure capture classification changed")
    if closure.get("syntax_rule") != (
        "closure capture lists and capture-mode annotations are not public syntax"
    ):
        raise ValidationError("closure capture syntax boundary changed")
    if closure.get("plan_owner") != (
        "ProgramSemanticClosure and SemanticPlan publish the complete capture fact before Xi"
    ):
        raise ValidationError("closure capture plan owner changed")
    if closure.get("consumer_rule") != (
        "Xi VM AOT and runtime consume the verified capture fact without reclassifying ownership"
    ):
        raise ValidationError("closure capture consumer boundary changed")
    if set(require_text_list(closure, "forbidden_authorities", "closure capture contract")) != {
        "closure name", "callee body availability", "runtime retain count", "backend liveness",
        "scheduler topology",
    }:
        raise ValidationError("closure capture forbidden authority set changed")

    domain = data.get("domain_transfer_contract")
    if not isinstance(domain, dict) or domain.get("type") != "XaDomainTransferState":
        raise ValidationError("domain transfer owner changed")
    require_exact_keys(domain, {
        "type", "states", "allowed_transitions", "forbidden_recovery_triggers", "copy_semantics",
        "move_semantics",
    }, "domain transfer contract")
    if domain.get("states") != [
        {"symbol": symbol, "value": value} for symbol, value in EXPECTED_DOMAIN_STATES
    ]:
        raise ValidationError("domain transfer state records changed")
    require_symbol_values(domain.get("states"), EXPECTED_DOMAIN_STATES, "domain transfer states")
    transitions = domain.get("allowed_transitions")
    if not isinstance(transitions, list) or len(transitions) != len(set(transitions)):
        raise ValidationError("domain transfer transitions must be unique")
    if set(transitions) != EXPECTED_DOMAIN_TRANSITIONS:
        raise ValidationError("domain transfer transition set changed")
    if any(transition.split("->", 1)[1] == "CANDIDATE" for transition in transitions):
        raise ValidationError("domain transfer permits a recovery backedge")
    if set(require_text_list(domain, "forbidden_recovery_triggers", "domain contract")) != {
        "scope exit", "last use", "retain count drops to one", "container overwrite",
    }:
        raise ValidationError("domain recovery prohibition changed")
    if domain.get("copy_semantics") != "copy creates a new candidate domain":
        raise ValidationError("domain copy semantics changed")
    if domain.get("move_semantics") != "move transfers the root token of the same candidate domain":
        raise ValidationError("domain move semantics changed")

    edges = data.get("ownership_edge_contract")
    if not isinstance(edges, dict) or edges.get("type") != "XaOwnershipEdgeKind":
        raise ValidationError("ownership edge owner changed")
    require_exact_keys(edges, {
        "type", "kinds", "required_fields", "borrow_fields", "owned_foreign_fields",
        "internal_owned_criteria", "adoption_rules", "external_alias_triggers", "unknown_policy",
        "forbidden_classifiers",
    }, "ownership edge contract")
    if edges.get("kinds") != EXPECTED_EDGE_KINDS:
        raise ValidationError("ownership edge taxonomy changed")
    if set(require_text_list(edges, "required_fields", "ownership edge contract")) != {
        "source value/place", "target value/place", "source domain id", "target domain id", "kind",
        "provenance", "creation site", "complete", "unknown reason",
    }:
        raise ValidationError("ownership edge required fields changed")
    if set(require_text_list(edges, "borrow_fields", "ownership edge contract")) != {
        "loan boundary", "view fact id",
    }:
        raise ValidationError("ownership borrow edge fields changed")
    if set(require_text_list(edges, "owned_foreign_fields", "ownership edge contract")) != {
        "drop/finalizer policy",
    }:
        raise ValidationError("ownership drop policy fields changed")
    if set(require_text_list(edges, "internal_owned_criteria", "ownership edge contract")) != {
        "source and target already share one ownership domain or source enters by legal move adoption",
        "the domain ledger owns the edge lifetime",
        "the edge publishes no ordinary strong handle outside the domain",
        "drop/finalizer order is complete or domain teardown handles the internal strong cycle",
        "the edge carries no execution-affine unknown-foreign or non-transferable weak-table state",
    }:
        raise ValidationError("INTERNAL_OWNED criteria changed")
    if set(require_text_list(edges, "adoption_rules", "ownership edge contract")) != {
        "child source domain is CANDIDATE with no loan and a complete plan",
        "child binding becomes MOVED",
        "child allocation/drop ledger merges or attaches to the parent ownership domain",
        "the field edge is INTERNAL_OWNED", "the parent domain transfer state does not worsen",
        "adoption cannot cross const sync or foreign boundaries or change execution-affine finalizer constraints",
    }:
        raise ValidationError("ownership adoption rules changed")
    if set(require_text_list(edges, "external_alias_triggers", "ownership edge contract")) != {
        "ordinary root handle binding or assignment",
        "store into another ownership domain field container or closure",
        "return an ordinary managed alias", "dynamic or foreign call that may retain",
        "publish to module mutable global or unknown storage",
        "stored closure captures a mutable root by reference", "incomplete edge provenance",
    }:
        raise ValidationError("external alias trigger set changed")
    if edges.get("unknown_policy") != "UNKNOWN edge or provenance fails closed":
        raise ValidationError("unknown edge policy changed")
    if set(require_text_list(edges, "forbidden_classifiers", "ownership edge contract")) != {
        "field name", "method name", "function name", "runtime retain count",
    }:
        raise ValidationError("ownership edge classifier prohibition changed")

    views = data.get("view_validity_contract")
    if not isinstance(views, dict) or views.get("type") != "XaViewValidityState":
        raise ValidationError("view validity owner changed")
    require_exact_keys(views, {
        "type", "states", "required_fields", "transfer_rules", "join_table", "join_commutative",
        "unknown_precedence", "evaluation_order_rules", "owner_effects", "owner_effect_rules",
        "cleanup_reachability_rules", "source_legality_owner", "forbidden_legality_owners",
    }, "view validity contract")
    if views.get("states") != [
        {"symbol": symbol, "value": value} for symbol, value in EXPECTED_VIEW_STATES
    ]:
        raise ValidationError("view validity state records changed")
    require_symbol_values(views.get("states"), EXPECTED_VIEW_STATES, "view validity states")
    if views.get("join_table") != EXPECTED_VIEW_JOIN_TABLE or views.get("join_commutative") is not True:
        raise ValidationError("view validity join contract changed")
    if views.get("unknown_precedence") != (
        "UNKNOWN is the top state and absorbs every other join input"
    ):
        raise ValidationError("view validity UNKNOWN precedence changed")
    if views.get("owner_effects") != [
        "PRESERVES_VIEW", "INVALIDATES_VIEW", "UNKNOWN_VIEW_EFFECT",
    ]:
        raise ValidationError("view effect taxonomy changed")
    if views.get("owner_effect_rules") != {
        "PRESERVES_VIEW": (
            "proves backing address extent and lifetime remain valid for every origin root"
        ),
        "INVALIDATES_VIEW": (
            "includes owner move rebind destroy maybe-reallocation and backing-extent shortening"
        ),
        "UNKNOWN_VIEW_EFFECT": (
            "invalidates a tracked local view and blocks the owner action when the view is active escaped captured or foreign-unknown"
        ),
        "raw_foreign_policy": (
            "incomplete raw or foreign effects fail closed and cannot be inferred from helper names"
        ),
    }:
        raise ValidationError("view owner-effect contract changed")
    if views.get("cleanup_reachability_rules") != [
        "a defer cleanup publishes typed RootId and lifetime-scope reachability before source legality checks",
        "move return rebind or destroy rejects while a reachable cleanup can observe the root",
        "scope exit may expire the cleanup fact but never grants legality from future-use or textual last-use",
        "the cleanup fact feeds allocation storage drop planning and exactly-once teardown",
    ]:
        raise ValidationError("cleanup reachability contract changed")
    if set(require_text_list(views, "required_fields", "view validity contract")) != {
        "view value/binding id", "canonical origin RootId set", "validity state",
        "creation/rebind site", "first invalidation site", "invalidation reason", "escape bit",
        "capture bit", "call-active bit", "complete", "unknown reason",
    }:
        raise ValidationError("view validity required fields changed")
    if set(require_text_list(views, "transfer_rules", "view validity contract")) != {
        "create or rebind from an owner or proven view creates a LIVE version with a canonical origin set",
        "read or project a LIVE view preserves its state and origin set",
        "use of INVALIDATED, MAYBE_INVALIDATED, or UNKNOWN rejects",
        "owner-preserving action leaves view state unchanged",
        "owner-invalidating action atomically invalidates every tracked local view with an intersecting origin set",
        "overwriting or leaving scope makes only that view version unreachable",
        "an active, escaped, captured, or foreign-unknown view blocks owner invalidation",
    }:
        raise ValidationError("view validity transfer rules changed")
    if views.get("evaluation_order_rules") != [
        "data.push(len(view)) completes the READ call before receiver invalidation and is allowed",
        "mutate(ref data, view) holds overlapping REF and view facts in one call and is rejected",
        "owner move rebind or destroy invalidates tracked views before owner state commits",
        "any failed check commits neither owner state nor view state",
        "READ REF and immediate non-retaining closure loans end at the enclosing call return",
    ]:
        raise ValidationError("view evaluation-order contract changed")
    if views.get("source_legality_owner") != "forward validity state machine and typed invalidation effect":
        raise ValidationError("view source-legality owner changed")
    if set(require_text_list(views, "forbidden_legality_owners", "view validity contract")) != {
        "AST last-use", "CFG future-use scan", "fixed lexical block lifetime", "runtime capacity",
    }:
        raise ValidationError("view source-legality prohibition changed")

    semantic_plan = data.get("semantic_plan_contract")
    if not isinstance(semantic_plan, dict):
        raise ValidationError("SemanticPlan contract must be an object")
    require_exact_keys(semantic_plan, {
        "required_facts", "domain_plan_required_facts", "required_types", "atomicity_invariant",
        "independent_checker_obligations", "fingerprint_role",
    }, "SemanticPlan contract")
    if set(require_text_list(semantic_plan, "required_facts", "SemanticPlan contract")) != EXPECTED_SEMANTIC_FACTS:
        raise ValidationError("SemanticPlan required facts changed")
    if set(require_text_list(semantic_plan, "domain_plan_required_facts", "SemanticPlan contract")) != {
        "domain id", "root value id", "transfer state and first witness", "allocation sites",
        "internal edge closure", "outbound const/sync/weak/foreign edges", "drop/finalizer ledger",
        "storage domain", "transfer capability", "complete and unknown reasons",
    }:
        raise ValidationError("ownership domain plan facts changed")
    if semantic_plan.get("required_types") != [
        "XaDomainTransferState", "XaOwnershipEdgeKind", "XaOwnershipEdge[]",
        "XaOwnershipDomainPlan", "XaDomainAdoptionPlan", "XaViewFact", "XaOwnerViewIndex",
        "XaCallBoundLoan", "XaClosureCaptureFact", "XaCleanupReachabilityFact",
    ]:
        raise ValidationError("ownership plan type set changed")
    if semantic_plan.get("independent_checker_obligations") != [
        "borrowed return origin set is nonempty sorted deduplicated and ordinal-bounded",
        "parameter and receiver origins are READ and type eligible",
        "every return provenance is a member of the normalized origin set",
        "each invalidation and its owner mutation or move share one plan node",
        "every intersecting local view has a validity transition",
        "active escaped and unknown views are never silently marked dead",
        "every view join is a row in the frozen validity join table",
        "every closure capture has one complete call-bound escape or adoption classification",
        "every reachable cleanup RootId has one lifetime scope and drop boundary",
    ]:
        raise ValidationError("SemanticPlan independent checker obligations changed")
    if semantic_plan.get("atomicity_invariant") != (
        "owner invalidation and every intersecting local-view transition are one semantic action"
    ):
        raise ValidationError("ownership invalidation atomicity changed")
    if semantic_plan.get("fingerprint_role") != (
        "byte integrity only; structural and semantic verification remains mandatory"
    ):
        raise ValidationError("SemanticPlan fingerprint role changed")
    target_plan = data.get("target_plan_contract")
    if not isinstance(target_plan, dict):
        raise ValidationError("TargetPlan contract must be an object")
    require_exact_keys(target_plan, {"allowed_choices", "forbidden_inference"}, "TargetPlan contract")
    if target_plan.get("allowed_choices") != [
        "target-specific representation", "layout", "slot/call convention", "materialization",
    ]:
        raise ValidationError("TargetPlan choice boundary changed")
    forbidden_target_inference = set(
        require_text_list(target_plan, "forbidden_inference", "TargetPlan contract")
    )
    if forbidden_target_inference != {
        "alias versus owned edge", "receiver mode", "borrowed origin",
        "domain transfer eligibility", "storage domain", "view validity or invalidation",
        "call-bound loan scope", "closure retention escape adoption or cross-execution classification",
        "allocation drop or boundary-transfer action",
        "source legality from backend liveness",
        "ownership from VM, C, or helper names",
    }:
        raise ValidationError("TargetPlan inference boundary changed")


def validate_atomic_cut(data: dict[str, object], inventory: dict[str, object]) -> None:
    require_exact_keys(data, {
        "schema", "activation", "authority", "train_base", "activation_batch", "preparation_order",
        "nodes", "activation_gates", "external_dependencies", "delivery_state",
        "remaining_external_boundary",
    }, "atomic cut")
    if data.get("schema") != "ownership-atomic-cut-staging/1":
        raise ValidationError("atomic cut schema is not exact")
    if data.get("activation") != "staging-only-before-i1-r4":
        raise ValidationError("atomic cut activation boundary changed")
    authority = require_text(data, "authority", "atomic cut")
    if authority != EXPECTED_CUT_AUTHORITY:
        raise ValidationError("atomic cut claims independent activation authority")
    train_base = data.get("train_base")
    if not isinstance(train_base, dict) or train_base != {"commit": TRAIN_COMMIT, "tree": TRAIN_TREE}:
        raise ValidationError("atomic cut train base changed")
    batch = data.get("activation_batch")
    if not isinstance(batch, dict):
        raise ValidationError("atomic activation batch must be an object")
    require_exact_keys(batch, {
        "atomic", "independently_activatable_nodes", "checkpoint", "required_locks",
        "required_outcomes", "forbidden_shortcuts",
    }, "atomic activation batch")
    if batch.get("atomic") is not True or batch.get("independently_activatable_nodes") is not False:
        raise ValidationError("atomic activation was weakened")
    if batch.get("checkpoint") != "I1/R4 clean":
        raise ValidationError("atomic activation checkpoint changed")
    require_locks(batch, ALLOWED_LOCKS, "atomic activation batch")
    if set(require_text_list(batch, "required_outcomes", "atomic activation batch")) != {
        "only the new parser and source spelling remain",
        "stdlib tests examples ports and tooling consumers migrate in the same batch",
        "function type PSC SemanticPlan TargetPlan Xi VM AOT and runtime consume one typed ownership fact chain",
        "cache schema artifact identity contracts generated projections and completion residue update together",
        "every covered old owner is deleted in the same batch after positive and negative evidence passes",
    }:
        raise ValidationError("atomic activation outcome set changed")
    shortcuts = set(require_text_list(batch, "forbidden_shortcuts", "atomic activation batch"))
    if shortcuts != {
        "compatibility parser", "body-inferred receiver", "body-inferred borrowed origin",
        "last-use source legality", "dual summary", "runtime fallback", "schema alias",
        "migration flag", "skip", "allowlist", "disabled verifier",
    }:
        raise ValidationError("atomic activation shortcut prohibition changed")
    if data.get("preparation_order") != EXPECTED_CUT_ORDER:
        raise ValidationError("atomic cut preparation order changed")

    nodes = require_exact_ids(data.get("nodes"), set(EXPECTED_CUT_NODES), "atomic cut nodes")
    node_by_id = {str(node["id"]): node for node in nodes}
    preparation_coverage: list[str] = []
    deletion_coverage: list[str] = []
    negative_coverage: set[str] = set()
    seen: set[str] = set()
    for node_id in EXPECTED_CUT_ORDER:
        node = node_by_id[node_id]
        (
            expected_phase, expected_prerequisites, expected_locks, expected_preparations,
            expected_deletions, expected_reasons,
        ) = EXPECTED_CUT_NODES[node_id]
        require_exact_keys(node, {
            "id", "task_phase", "owned_fact", "prerequisites", "required_locks",
            "protected_surfaces", "positive_gates", "negative_reasons",
            "prepares_replacement_for_rows", "deletes_inventory_rows",
            "required_activation_gate_ids",
        }, f"atomic cut node {node_id}")
        if node.get("task_phase") != expected_phase:
            raise ValidationError(f"atomic cut node {node_id} task phase changed")
        expected_fact, expected_surfaces, expected_gates = EXPECTED_CUT_NODE_DETAILS[node_id]
        if require_text(node, "owned_fact", f"atomic cut node {node_id}") != expected_fact:
            raise ValidationError(f"atomic cut node {node_id} owned fact changed")
        if require_text_list(
            node, "protected_surfaces", f"atomic cut node {node_id}"
        ) != expected_surfaces:
            raise ValidationError(f"atomic cut node {node_id} protected surfaces changed")
        if require_text_list(node, "positive_gates", f"atomic cut node {node_id}") != expected_gates:
            raise ValidationError(f"atomic cut node {node_id} positive gates changed")
        require_text_list(node, "negative_reasons", f"atomic cut node {node_id}")
        prerequisites = node.get("prerequisites")
        if not isinstance(prerequisites, list) or set(prerequisites) != expected_prerequisites:
            raise ValidationError(f"atomic cut node {node_id} prerequisites changed")
        if not set(prerequisites) <= seen:
            raise ValidationError(f"atomic cut node {node_id} has a forward prerequisite")
        require_locks(node, expected_locks, f"atomic cut node {node_id}")
        preparations = node.get("prepares_replacement_for_rows")
        if not isinstance(preparations, list) or set(preparations) != expected_preparations:
            raise ValidationError(f"atomic cut node {node_id} replacement preparation changed")
        deletions = node.get("deletes_inventory_rows")
        if not isinstance(deletions, list) or set(deletions) != expected_deletions:
            raise ValidationError(f"atomic cut node {node_id} deletion coverage changed")
        if set(node["negative_reasons"]) != expected_reasons:
            raise ValidationError(f"atomic cut node {node_id} negative coverage changed")
        required_gate_ids = node.get("required_activation_gate_ids")
        expected_gate_ids = (
            [gate[0] for gate in EXPECTED_ACTIVATION_GATES]
            if node_id == "retirement-contract" else []
        )
        if required_gate_ids != expected_gate_ids:
            raise ValidationError(f"atomic cut node {node_id} activation-gate dependency changed")
        preparation_coverage.extend(str(item) for item in preparations)
        deletion_coverage.extend(str(item) for item in deletions)
        negative_coverage.update(str(item) for item in node["negative_reasons"])
        seen.add(node_id)
    if len(preparation_coverage) != len(set(preparation_coverage)):
        raise ValidationError("an old owner is assigned to multiple replacement preparation nodes")
    if set(preparation_coverage) != set(EXPECTED_ROWS):
        raise ValidationError("atomic cut does not prepare every inventoried replacement exactly once")
    if len(deletion_coverage) != len(set(deletion_coverage)):
        raise ValidationError("an old owner is assigned to multiple deletion nodes")
    if set(deletion_coverage) != set(EXPECTED_ROWS):
        raise ValidationError("atomic cut does not delete every inventoried old owner exactly once")
    expected_reasons = {case[2] for case in EXPECTED_CASES.values()}
    if negative_coverage != expected_reasons:
        raise ValidationError("atomic cut does not cover every staged negative reason")

    inventory_rows_value = inventory.get("rows")
    if not isinstance(inventory_rows_value, list):
        raise ValidationError("inventory rows are unavailable for atomic-cut cross-check")
    inventory_rows = {
        str(row.get("id")): row for row in inventory_rows_value if isinstance(row, dict)
    }
    preparation_node_by_row = {
        row_id: node_id for node_id, node in node_by_id.items()
        for row_id in node["prepares_replacement_for_rows"]
    }
    deletion_node_by_row = {
        row_id: node_id for node_id, node in node_by_id.items()
        for row_id in node["deletes_inventory_rows"]
    }
    for row_id, row in inventory_rows.items():
        preparation_node = node_by_id[preparation_node_by_row[row_id]]
        deletion_node = node_by_id[deletion_node_by_row[row_id]]
        if preparation_node.get("task_phase") != row.get("deletion_phase"):
            raise ValidationError(f"atomic cut row {row_id} preparation phase disagrees with inventory")
        row_locks = row.get("required_locks")
        if not isinstance(row_locks, list):
            raise ValidationError(f"atomic cut row {row_id} inventory locks are invalid")
        preparation_locks = set(preparation_node["required_locks"])
        if not (set(row_locks) - {"LOCK-RESIDUE"}) <= preparation_locks:
            raise ValidationError(f"atomic cut row {row_id} replacement preparation lacks a shared lock")
        if not set(row_locks) <= set(deletion_node["required_locks"]):
            raise ValidationError(f"atomic cut row {row_id} deletion barrier lacks a shared lock")
        if deletion_node.get("id") != "retirement-contract":
            raise ValidationError(f"atomic cut row {row_id} bypasses the retirement deletion barrier")

    expected_gate_records = [
        {
            "id": gate_id, "required_stage": stage, "status": "UNRUN", "evidence": [],
            "blocking_reason": reason,
        }
        for gate_id, stage, reason in EXPECTED_ACTIVATION_GATES
    ]
    if data.get("activation_gates") != expected_gate_records:
        raise ValidationError("activation gate inventory or UNRUN boundary changed")
    if data.get("external_dependencies") != EXPECTED_EXTERNAL_DEPENDENCIES:
        raise ValidationError("atomic cut external dependency boundary changed")
    if data.get("delivery_state") != "NOT_READY":
        raise ValidationError("staging must remain NOT_READY before activation")
    if data.get("remaining_external_boundary") != (
        "I1/R4 clean checkpoint, explicit lock grants, matching default binary, downstream "
        "coordination, and every activation gate still marked UNRUN"
    ):
        raise ValidationError("atomic cut external boundary changed")


def validate_activation_gate_metadata(
    data: dict[str, object], atomic_cut: dict[str, object],
) -> None:
    require_exact_keys(data, {
        "schema", "activation", "authority", "train_base",
        "current_entrypoint_semantics", "gates", "delivery_state",
    }, "activation gate metadata")
    canonical = json.dumps(
        data, sort_keys=True, separators=(",", ":"), ensure_ascii=False,
    ).encode("utf-8")
    if hashlib.sha256(canonical).hexdigest() != EXPECTED_GATE_METADATA_CANONICAL_SHA256:
        raise ValidationError("activation gate metadata content changed")
    if data.get("schema") != "ownership-activation-gate-staging/1":
        raise ValidationError("activation gate metadata schema changed")
    if data.get("activation") != "staging-only-before-i1-r4":
        raise ValidationError("activation gate metadata boundary changed")
    if data.get("authority") != EXPECTED_GATE_AUTHORITY:
        raise ValidationError("activation gate metadata claims qualification authority")
    if data.get("train_base") != {"commit": TRAIN_COMMIT, "tree": TRAIN_TREE}:
        raise ValidationError("activation gate metadata train base changed")
    if data.get("current_entrypoint_semantics") != EXPECTED_CURRENT_ENTRYPOINT_SEMANTICS:
        raise ValidationError("CURRENT activation entrypoint semantics changed")
    if data.get("delivery_state") != "NOT_READY":
        raise ValidationError("activation gate metadata must remain NOT_READY")

    expected_ids = [gate[0] for gate in EXPECTED_ACTIVATION_GATES]
    gates = require_exact_ids(data.get("gates"), set(expected_ids), "activation gate metadata")
    if [gate.get("id") for gate in gates] != expected_ids:
        raise ValidationError("activation gate metadata order changed")
    required_keys = {
        "id", "owner", "entrypoint_state", "commands", "cwd", "environment",
        "required_host_provider", "skip_policy", "pass_criteria", "evidence_artifacts",
        "future_assets",
    }
    for gate in gates:
        gate_id = str(gate["id"])
        require_exact_keys(gate, required_keys, f"activation gate metadata {gate_id}")
        expected_state = "CURRENT" if gate_id in EXPECTED_CURRENT_GATE_IDS else "NEEDS_ACTIVATED_CASE"
        if gate.get("entrypoint_state") != expected_state:
            raise ValidationError(f"activation gate metadata {gate_id} entrypoint state changed")
        for field in (
            "owner", "cwd", "environment", "required_host_provider", "skip_policy",
            "pass_criteria",
        ):
            require_text(gate, field, f"activation gate metadata {gate_id}")
        if gate.get("cwd") != "repository-root":
            raise ValidationError(f"activation gate metadata {gate_id} cwd changed")
        commands = require_text_list(gate, "commands", f"activation gate metadata {gate_id}")
        if any(
            "--config" in command or command.lstrip().lower().startswith("make ")
            for command in commands
        ):
            raise ValidationError(f"activation gate metadata {gate_id} violates Ninja rules")
        require_text_list(
            gate, "evidence_artifacts", f"activation gate metadata {gate_id}"
        )
        require_text_list(gate, "future_assets", f"activation gate metadata {gate_id}")

    atomic_records = atomic_cut.get("activation_gates")
    if not isinstance(atomic_records, list):
        raise ValidationError("atomic activation gate records are unavailable")
    if [record.get("id") for record in atomic_records if isinstance(record, dict)] != expected_ids:
        raise ValidationError("activation metadata and atomic gate ids disagree")
    if any(
        not isinstance(record, dict) or record.get("status") != "UNRUN"
        or record.get("evidence") != [] for record in atomic_records
    ):
        raise ValidationError("activation metadata cannot qualify an atomic gate")


def validate(
    inventory: dict[str, object], oracles: dict[str, object], typed_contract: dict[str, object],
    atomic_cut: dict[str, object], activation_gates: dict[str, object] | None = None,
) -> None:
    if activation_gates is None:
        activation_gates = load_json(ACTIVATION_GATE_PATH)
    validate_staging_file_hashes()
    validate_positive_fixtures()
    validate_inventory(inventory)
    validate_oracles(oracles)
    validate_typed_contract(typed_contract)
    validate_atomic_cut(atomic_cut, inventory)
    validate_activation_gate_metadata(activation_gates, atomic_cut)


def self_test(
    inventory: dict[str, object], oracles: dict[str, object], typed_contract: dict[str, object],
    atomic_cut: dict[str, object], activation_gates: dict[str, object],
) -> None:
    for name, expected in EXPECTED_STAGING_SHA256.items():
        try:
            validate_staging_bytes(name, b"altered staging bytes", expected)
        except ValidationError:
            continue
        raise ValidationError(f"self-test byte mutation was accepted: {name}")
    incomplete_hashes = dict(EXPECTED_STAGING_SHA256)
    incomplete_hashes.pop(TYPED_CONTRACT_PATH.name)
    try:
        validate_staging_hash_coverage(incomplete_hashes)
    except ValidationError:
        pass
    else:
        raise ValidationError("self-test incomplete staging hash coverage was accepted")
    mutation_reason_by_fixture = {
        fixture: reason for kind, fixture, reason, _locks in EXPECTED_CASES.values()
        if kind == "semantic-plan-mutation"
    }
    mutation_fixture = "read_foreign_unknown.mutation.json"
    mutation_record = load_json(HERE / mutation_fixture)
    hostile_metadata_mutations: list[tuple[str, dict[str, object]]] = []
    extra_mutation_status = copy.deepcopy(mutation_record)
    extra_mutation_status["status"] = "PASS"
    hostile_metadata_mutations.append(("hostile mutation pass status", extra_mutation_status))
    extra_mutation_code = copy.deepcopy(mutation_record)
    extra_mutation_code["diagnostic_code"] = "XR_OWN_3007"
    hostile_metadata_mutations.append(("hostile mutation numeric code", extra_mutation_code))
    false_precondition = copy.deepcopy(mutation_record)
    false_precondition["source_precondition"] = "arbitrary valid plan"
    hostile_metadata_mutations.append(("hostile mutation false precondition", false_precondition))
    benign_mutation = copy.deepcopy(mutation_record)
    benign_mutation["mutations"][0] = "Preserve the complete valid edge"
    hostile_metadata_mutations.append(("hostile mutation benign edit", benign_mutation))
    execute_hostile_plan = copy.deepcopy(mutation_record)
    execute_hostile_plan["required_verifier_checks"][-1] = "Execute VM and AOT"
    hostile_metadata_mutations.append(("hostile mutation executes", execute_hostile_plan))
    for label, record in hostile_metadata_mutations:
        try:
            validate_mutation_record(
                record, mutation_reason_by_fixture[mutation_fixture], mutation_fixture, label,
            )
        except ValidationError:
            continue
        raise ValidationError(f"self-test mutation was accepted: {label}")
    mutations: list[
        tuple[str, dict[str, object], dict[str, object], dict[str, object], dict[str, object]]
    ] = []
    missing_row = copy.deepcopy(inventory)
    missing_row["rows"] = missing_row["rows"][:-1]
    mutations.append((
        "missing inventory row", missing_row, copy.deepcopy(oracles), copy.deepcopy(typed_contract),
        copy.deepcopy(atomic_cut),
    ))
    stale_anchor = copy.deepcopy(inventory)
    stale_anchor["rows"][0]["producers"][0]["anchor"] = "definitely-not-a-source-anchor"
    mutations.append((
        "stale source anchor", stale_anchor, copy.deepcopy(oracles), copy.deepcopy(typed_contract),
        copy.deepcopy(atomic_cut),
    ))
    weak_lock = copy.deepcopy(inventory)
    weak_lock["rows"][2]["required_locks"] = ["LOCK-SCHEMA"]
    mutations.append((
        "weakened inventory lock", weak_lock, copy.deepcopy(oracles), copy.deepcopy(typed_contract),
        copy.deepcopy(atomic_cut),
    ))
    missing_cleanup_residue = copy.deepcopy(inventory)
    cleanup_row = next(
        row for row in missing_cleanup_residue["rows"] if row["id"] == "source-view-last-use"
    )
    cleanup_row["residue"] = [
        item for item in cleanup_row["residue"] if item["anchor"] != "XA_LOAN_CLEANUP_READ"
    ]
    mutations.append((
        "missing cleanup reachability residue", missing_cleanup_residue, copy.deepcopy(oracles),
        copy.deepcopy(typed_contract), copy.deepcopy(atomic_cut),
    ))
    missing_cleanup_witness = copy.deepcopy(inventory)
    cleanup_witness_row = next(
        row for row in missing_cleanup_witness["rows"] if row["id"] == "source-view-last-use"
    )
    cleanup_witness_row["witnesses"] = [
        item for item in cleanup_witness_row["witnesses"]
        if item["path"] != "tests/compile_errors/ownership/180_defer_cleanup_blocks_move.xr"
    ]
    mutations.append((
        "missing cleanup reachability witness", missing_cleanup_witness, copy.deepcopy(oracles),
        copy.deepcopy(typed_contract), copy.deepcopy(atomic_cut),
    ))
    missing_closure_anchor = copy.deepcopy(inventory)
    closure_row = next(
        row for row in missing_closure_anchor["rows"] if row["id"] == "closure-capture-authority"
    )
    closure_row["producers"] = closure_row["producers"][1:]
    mutations.append((
        "missing closure authority anchor", missing_closure_anchor, copy.deepcopy(oracles),
        copy.deepcopy(typed_contract), copy.deepcopy(atomic_cut),
    ))
    missing_closure_runtime_consumer = copy.deepcopy(inventory)
    closure_runtime_row = next(
        row for row in missing_closure_runtime_consumer["rows"]
        if row["id"] == "closure-capture-authority"
    )
    closure_runtime_row["consumers"] = [
        item for item in closure_runtime_row["consumers"]
        if item["path"] != "src/vm/xvm_coro_backend.c"
    ]
    mutations.append((
        "missing closure runtime consumer", missing_closure_runtime_consumer,
        copy.deepcopy(oracles), copy.deepcopy(typed_contract), copy.deepcopy(atomic_cut),
    ))
    missing_closure_aot_residue = copy.deepcopy(inventory)
    closure_aot_row = next(
        row for row in missing_closure_aot_residue["rows"]
        if row["id"] == "closure-capture-authority"
    )
    closure_aot_row["residue"] = [
        item for item in closure_aot_row["residue"]
        if not (
            item["path"] == "src/aot/xaot_storage_plan.h"
            and item["anchor"] == "typedef struct XaotCapturePlan {"
        )
    ]
    mutations.append((
        "missing closure AOT planner residue", missing_closure_aot_residue,
        copy.deepcopy(oracles), copy.deepcopy(typed_contract), copy.deepcopy(atomic_cut),
    ))
    missing_case = copy.deepcopy(oracles)
    missing_case["cases"] = missing_case["cases"][:-1]
    mutations.append((
        "missing oracle case", copy.deepcopy(inventory), missing_case, copy.deepcopy(typed_contract),
        copy.deepcopy(atomic_cut),
    ))
    wrong_reason = copy.deepcopy(oracles)
    wrong_reason["cases"][0]["activation_reason"] = "OWN-E-WRONG"
    mutations.append((
        "wrong oracle reason", copy.deepcopy(inventory), wrong_reason, copy.deepcopy(typed_contract),
        copy.deepcopy(atomic_cut),
    ))
    swapped_fixture = copy.deepcopy(oracles)
    swapped_fixture["cases"][0]["fixture"] = swapped_fixture["cases"][1]["fixture"]
    mutations.append((
        "swapped oracle fixture", copy.deepcopy(inventory), swapped_fixture,
        copy.deepcopy(typed_contract), copy.deepcopy(atomic_cut),
    ))
    weakened_case = copy.deepcopy(oracles)
    weakened_case["cases"][0]["baseline"] = "expected-parser-error with fallback"
    mutations.append((
        "weakened oracle", copy.deepcopy(inventory), weakened_case, copy.deepcopy(typed_contract),
        copy.deepcopy(atomic_cut),
    ))
    false_baseline = copy.deepcopy(oracles)
    false_baseline["cases"][0]["baseline"] = "arbitrary nonempty baseline claim"
    mutations.append((
        "false oracle baseline", copy.deepcopy(inventory), false_baseline,
        copy.deepcopy(typed_contract), copy.deepcopy(atomic_cut),
    ))
    false_site = copy.deepcopy(oracles)
    false_site["cases"][0]["diagnostic_site"] = "unrelated nonempty site"
    mutations.append((
        "false oracle diagnostic site", copy.deepcopy(inventory), false_site,
        copy.deepcopy(typed_contract), copy.deepcopy(atomic_cut),
    ))
    false_help = copy.deepcopy(oracles)
    false_help["cases"][0]["help"] = "meaningless nonempty help"
    mutations.append((
        "false oracle help", copy.deepcopy(inventory), false_help,
        copy.deepcopy(typed_contract), copy.deepcopy(atomic_cut),
    ))
    extra_numeric_code = copy.deepcopy(oracles)
    extra_numeric_code["cases"][0]["diagnostic_code"] = "XR_OWN_3007"
    mutations.append((
        "unlocked numeric diagnostic claim", copy.deepcopy(inventory), extra_numeric_code,
        copy.deepcopy(typed_contract), copy.deepcopy(atomic_cut),
    ))
    extra_pass_status = copy.deepcopy(oracles)
    extra_pass_status["cases"][0]["status"] = "PASS"
    mutations.append((
        "false oracle pass status", copy.deepcopy(inventory), extra_pass_status,
        copy.deepcopy(typed_contract), copy.deepcopy(atomic_cut),
    ))
    weakened_fixture = copy.deepcopy(oracles)
    weakened_fixture["cases"][0]["sha256"] = "0" * 64
    mutations.append((
        "weakened fixture content", copy.deepcopy(inventory), weakened_fixture,
        copy.deepcopy(typed_contract), copy.deepcopy(atomic_cut),
    ))
    missing_harnesses = copy.deepcopy(oracles)
    missing_harnesses["existing_harnesses"] = []
    mutations.append((
        "missing harness inventory", copy.deepcopy(inventory), missing_harnesses,
        copy.deepcopy(typed_contract), copy.deepcopy(atomic_cut),
    ))
    missing_diagnostic_evidence = copy.deepcopy(oracles)
    missing_diagnostic_evidence["diagnostic_evidence"].pop()
    mutations.append((
        "missing diagnostic evidence", copy.deepcopy(inventory), missing_diagnostic_evidence,
        copy.deepcopy(typed_contract), copy.deepcopy(atomic_cut),
    ))
    false_diagnostic_reservation = copy.deepcopy(oracles)
    false_diagnostic_reservation["diagnostic_code_reservation"]["state"] = "RESERVED"
    mutations.append((
        "false diagnostic reservation", copy.deepcopy(inventory), false_diagnostic_reservation,
        copy.deepcopy(typed_contract), copy.deepcopy(atomic_cut),
    ))
    recovery_backedge = copy.deepcopy(typed_contract)
    recovery_backedge["domain_transfer_contract"]["allowed_transitions"].append(
        "EXTERNAL_ALIASED->CANDIDATE"
    )
    mutations.append((
        "domain recovery backedge", copy.deepcopy(inventory), copy.deepcopy(oracles),
        recovery_backedge, copy.deepcopy(atomic_cut),
    ))
    missing_receiver_mode = copy.deepcopy(typed_contract)
    missing_receiver_mode["receiver_contract"]["modes"] = ["READ", "REF"]
    mutations.append((
        "missing receiver mode", copy.deepcopy(inventory), copy.deepcopy(oracles),
        missing_receiver_mode, copy.deepcopy(atomic_cut),
    ))
    read_can_escape = copy.deepcopy(typed_contract)
    read_can_escape["parameter_modes"][0]["maximum_capability"] = "retain escape and suspend"
    mutations.append((
        "expanded READ capability", copy.deepcopy(inventory), copy.deepcopy(oracles),
        read_can_escape, copy.deepcopy(atomic_cut),
    ))
    body_authority = copy.deepcopy(typed_contract)
    body_authority["receiver_contract"]["body_role"] = "infer receiver authority"
    mutations.append((
        "receiver body authority", copy.deepcopy(inventory), copy.deepcopy(oracles),
        body_authority, copy.deepcopy(atomic_cut),
    ))
    worker_activation = copy.deepcopy(typed_contract)
    worker_activation["authority"] = (
        "non-authoritative integration train optional; worker may activate after local checks"
    )
    mutations.append((
        "worker activation authority", copy.deepcopy(inventory), copy.deepcopy(oracles),
        worker_activation, copy.deepcopy(atomic_cut),
    ))
    body_elision = copy.deepcopy(typed_contract)
    body_elision["borrow_origin_contract"]["elision"]["one_signature_candidate"] = (
        "scan the body and normalize to its origin"
    )
    mutations.append((
        "body-inferred origin elision", copy.deepcopy(inventory), copy.deepcopy(oracles),
        body_elision, copy.deepcopy(atomic_cut),
    ))
    concealed_return = copy.deepcopy(typed_contract)
    concealed_return["borrow_origin_contract"]["return_surface_constraints"].pop()
    mutations.append((
        "weakened borrowed return surface", copy.deepcopy(inventory), copy.deepcopy(oracles),
        concealed_return, copy.deepcopy(atomic_cut),
    ))
    missing_closure_contract = copy.deepcopy(typed_contract)
    del missing_closure_contract["closure_contract"]
    mutations.append((
        "missing closure capture contract", copy.deepcopy(inventory), copy.deepcopy(oracles),
        missing_closure_contract, copy.deepcopy(atomic_cut),
    ))
    weak_capture_classification = copy.deepcopy(typed_contract)
    weak_capture_classification["closure_contract"]["classification_rules"].pop()
    mutations.append((
        "weakened closure capture classification", copy.deepcopy(inventory),
        copy.deepcopy(oracles), weak_capture_classification, copy.deepcopy(atomic_cut),
    ))
    weak_internal_edge = copy.deepcopy(typed_contract)
    weak_internal_edge["ownership_edge_contract"]["internal_owned_criteria"] = []
    mutations.append((
        "weakened internal owned edge", copy.deepcopy(inventory), copy.deepcopy(oracles),
        weak_internal_edge, copy.deepcopy(atomic_cut),
    ))
    active_view_dies = copy.deepcopy(typed_contract)
    active_view_dies["view_validity_contract"]["transfer_rules"][-1] = (
        "an escaped captured or foreign-unknown view blocks owner invalidation"
    )
    mutations.append((
        "active view silently invalidated", copy.deepcopy(inventory), copy.deepcopy(oracles),
        active_view_dies, copy.deepcopy(atomic_cut),
    ))
    ambiguous_join = copy.deepcopy(typed_contract)
    ambiguous_join["view_validity_contract"]["join_table"][11]["result"] = "MAYBE_INVALIDATED"
    mutations.append((
        "ambiguous UNKNOWN view join", copy.deepcopy(inventory), copy.deepcopy(oracles),
        ambiguous_join, copy.deepcopy(atomic_cut),
    ))
    weak_plan_checker = copy.deepcopy(typed_contract)
    weak_plan_checker["semantic_plan_contract"]["independent_checker_obligations"] = []
    mutations.append((
        "weakened SemanticPlan checker", copy.deepcopy(inventory), copy.deepcopy(oracles),
        weak_plan_checker, copy.deepcopy(atomic_cut),
    ))
    target_reinfers = copy.deepcopy(typed_contract)
    target_reinfers["target_plan_contract"]["forbidden_inference"].remove("receiver mode")
    mutations.append((
        "TargetPlan receiver inference", copy.deepcopy(inventory), copy.deepcopy(oracles),
        target_reinfers, copy.deepcopy(atomic_cut),
    ))
    extra_typed_owner = copy.deepcopy(typed_contract)
    extra_typed_owner["dual_read"] = True
    mutations.append((
        "extra typed authority", copy.deepcopy(inventory), copy.deepcopy(oracles),
        extra_typed_owner, copy.deepcopy(atomic_cut),
    ))
    missing_deletion = copy.deepcopy(atomic_cut)
    missing_deletion["nodes"][-1]["deletes_inventory_rows"].pop()
    mutations.append((
        "missing old-owner deletion", copy.deepcopy(inventory), copy.deepcopy(oracles),
        copy.deepcopy(typed_contract), missing_deletion,
    ))
    early_deletion = copy.deepcopy(atomic_cut)
    early_deletion["nodes"][-1]["deletes_inventory_rows"].remove("read-retain-return-alias")
    early_deletion["nodes"][1]["deletes_inventory_rows"].append("read-retain-return-alias")
    mutations.append((
        "early old-owner deletion", copy.deepcopy(inventory), copy.deepcopy(oracles),
        copy.deepcopy(typed_contract), early_deletion,
    ))
    missing_residue_lock = copy.deepcopy(atomic_cut)
    missing_residue_lock["nodes"][-1]["required_locks"].remove("LOCK-RESIDUE")
    mutations.append((
        "deletion barrier without residue lock", copy.deepcopy(inventory), copy.deepcopy(oracles),
        copy.deepcopy(typed_contract), missing_residue_lock,
    ))
    wrong_preparation_phase = copy.deepcopy(atomic_cut)
    wrong_preparation_phase["nodes"][5]["task_phase"] = "P7"
    mutations.append((
        "wrong replacement preparation phase", copy.deepcopy(inventory), copy.deepcopy(oracles),
        copy.deepcopy(typed_contract), wrong_preparation_phase,
    ))
    weakened_node_reason = copy.deepcopy(atomic_cut)
    weakened_node_reason["nodes"][0]["negative_reasons"] = ["OWN-E-READ-ESCAPE"]
    mutations.append((
        "weakened node-specific reason", copy.deepcopy(inventory), copy.deepcopy(oracles),
        copy.deepcopy(typed_contract), weakened_node_reason,
    ))
    placeholder_gate = copy.deepcopy(atomic_cut)
    placeholder_gate["nodes"][6]["positive_gates"] = ["placeholder"]
    mutations.append((
        "placeholder execution gate", copy.deepcopy(inventory), copy.deepcopy(oracles),
        copy.deepcopy(typed_contract), placeholder_gate,
    ))
    false_gate_pass = copy.deepcopy(atomic_cut)
    false_gate_pass["activation_gates"][0]["status"] = "PASS"
    false_gate_pass["activation_gates"][0]["evidence"] = ["stale binary"]
    mutations.append((
        "false activation gate pass", copy.deepcopy(inventory), copy.deepcopy(oracles),
        copy.deepcopy(typed_contract), false_gate_pass,
    ))
    missing_retirement_gate = copy.deepcopy(atomic_cut)
    missing_retirement_gate["nodes"][-1]["required_activation_gate_ids"].remove(
        "aot-ubsan-generated-output"
    )
    mutations.append((
        "retirement without generated-output UBSan", copy.deepcopy(inventory),
        copy.deepcopy(oracles), copy.deepcopy(typed_contract), missing_retirement_gate,
    ))
    missing_live_refusal_gate = copy.deepcopy(atomic_cut)
    missing_live_refusal_gate["nodes"][-1]["required_activation_gate_ids"].remove(
        "live-refusal-schema3-row-binding"
    )
    mutations.append((
        "retirement without schema-3 live-refusal qualification", copy.deepcopy(inventory),
        copy.deepcopy(oracles), copy.deepcopy(typed_contract), missing_live_refusal_gate,
    ))
    dual_runtime = copy.deepcopy(atomic_cut)
    dual_runtime["activation_batch"]["forbidden_shortcuts"].remove("runtime fallback")
    mutations.append((
        "runtime fallback shortcut", copy.deepcopy(inventory), copy.deepcopy(oracles),
        copy.deepcopy(typed_contract), dual_runtime,
    ))
    partial_activation = copy.deepcopy(atomic_cut)
    partial_activation["activation_batch"]["independently_activatable_nodes"] = True
    mutations.append((
        "partial activation", copy.deepcopy(inventory), copy.deepcopy(oracles),
        copy.deepcopy(typed_contract), partial_activation,
    ))
    node_activation_authority = copy.deepcopy(atomic_cut)
    node_activation_authority["authority"] = (
        "non-authoritative integration-train ledger; nodes may activate independently"
    )
    mutations.append((
        "independent node authority", copy.deepcopy(inventory), copy.deepcopy(oracles),
        copy.deepcopy(typed_contract), node_activation_authority,
    ))
    for label, mutated_inventory, mutated_oracles, mutated_typed, mutated_cut in mutations:
        try:
            validate(mutated_inventory, mutated_oracles, mutated_typed, mutated_cut)
        except ValidationError:
            continue
        raise ValidationError(f"self-test mutation was accepted: {label}")

    gate_mutations: list[tuple[str, dict[str, object]]] = []
    missing_gate_metadata = copy.deepcopy(activation_gates)
    missing_gate_metadata["gates"] = missing_gate_metadata["gates"][:-1]
    gate_mutations.append(("missing activation gate metadata", missing_gate_metadata))
    reordered_gate_metadata = copy.deepcopy(activation_gates)
    reordered_gate_metadata["gates"][0], reordered_gate_metadata["gates"][1] = (
        reordered_gate_metadata["gates"][1], reordered_gate_metadata["gates"][0]
    )
    gate_mutations.append(("reordered activation gate metadata", reordered_gate_metadata))
    false_current_gate = copy.deepcopy(activation_gates)
    false_current_gate["gates"][1]["entrypoint_state"] = "CURRENT"
    gate_mutations.append(("false current activation entrypoint", false_current_gate))
    downgraded_current_gate = copy.deepcopy(activation_gates)
    downgraded_current_gate["gates"][0]["entrypoint_state"] = "NEEDS_ACTIVATED_CASE"
    gate_mutations.append(("downgraded current activation entrypoint", downgraded_current_gate))
    empty_gate_command = copy.deepcopy(activation_gates)
    empty_gate_command["gates"][0]["commands"] = []
    gate_mutations.append(("empty activation gate command", empty_gate_command))
    false_gate_command = copy.deepcopy(activation_gates)
    false_gate_command["gates"][0]["commands"] = ["true"]
    gate_mutations.append(("false activation gate command", false_gate_command))
    weakened_skip_policy = copy.deepcopy(activation_gates)
    weakened_skip_policy["gates"][11]["skip_policy"] = "all skips pass"
    gate_mutations.append(("weakened sanitizer skip policy", weakened_skip_policy))
    substituted_provider = copy.deepcopy(activation_gates)
    substituted_provider["gates"][17]["required_host_provider"] = "anything"
    gate_mutations.append(("substituted provider requirement", substituted_provider))
    substituted_future_asset = copy.deepcopy(activation_gates)
    substituted_future_asset["gates"][11]["future_assets"] = ["placeholder"]
    gate_mutations.append(("substituted future activation asset", substituted_future_asset))
    deleted_future_asset = copy.deepcopy(activation_gates)
    deleted_future_asset["gates"][11]["future_assets"] = []
    gate_mutations.append(("deleted future activation asset", deleted_future_asset))
    qualified_gate_metadata = copy.deepcopy(activation_gates)
    qualified_gate_metadata["gates"][0]["status"] = "PASS"
    gate_mutations.append(("qualified activation gate metadata", qualified_gate_metadata))
    non_ninja_command = copy.deepcopy(activation_gates)
    non_ninja_command["gates"][0]["commands"][1] = "cmake --build build --config Release"
    gate_mutations.append(("multi-config activation command", non_ninja_command))
    for label, mutated_gates in gate_mutations:
        try:
            validate_activation_gate_metadata(mutated_gates, atomic_cut)
        except ValidationError:
            continue
        raise ValidationError(f"self-test mutation was accepted: {label}")


def main(argv: list[str]) -> int:
    try:
        validate_checkout("--require-clean" in argv)
        inventory = load_json(INVENTORY_PATH)
        oracles = load_json(ORACLES_PATH)
        typed_contract = load_json(TYPED_CONTRACT_PATH)
        atomic_cut = load_json(ATOMIC_CUT_PATH)
        activation_gates = load_json(ACTIVATION_GATE_PATH)
        validate(inventory, oracles, typed_contract, atomic_cut, activation_gates)
        if "--self-test" in argv:
            self_test(inventory, oracles, typed_contract, atomic_cut, activation_gates)
    except ValidationError as exc:
        print(f"ownership source-contract staging: FAIL: {exc}", file=sys.stderr)
        return 1
    suffix = " with mutation self-test" if "--self-test" in argv else ""
    print(
        f"ownership source-contract staging: PASS: {len(EXPECTED_ROWS)} owner rows, "
        f"{len(EXPECTED_CASES)} negative oracles, {len(EXPECTED_CUT_ORDER)} atomic-cut nodes, "
        f"{len(EXPECTED_ACTIVATION_GATES)} executable activation gates{suffix}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
