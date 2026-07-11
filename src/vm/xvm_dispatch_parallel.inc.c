/*
 * xvm_dispatch_parallel.inc.c — structured parallel batch dispatch
 *
 * Included inside xvm.c's vmcase scope. The helper owns the blocking/replay
 * protocol; the opcode remains VM-internal and is emitted only from XI_PAR_*.
 */

vmcase(OP_PAR_FOR) {
    TRACE_EXECUTION();
    XrDispatchAction _action = vm_par_for_dispatch(isolate, vm_ctx, i, base, frame, pc);
    VM_DISPATCH(_action);
}

vmcase(OP_PAR_MAP) {
    TRACE_EXECUTION();
    XrDispatchAction _action = vm_par_map_dispatch(isolate, vm_ctx, i, base, frame, pc);
    VM_DISPATCH(_action);
}

vmcase(OP_PAR_REDUCE) {
    TRACE_EXECUTION();
    XrDispatchAction _action = vm_par_reduce_dispatch(isolate, vm_ctx, i, base, frame, pc);
    VM_DISPATCH(_action);
}
