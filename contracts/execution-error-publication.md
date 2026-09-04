# Execution error publication contract

Status: frozen.

`XrExecutionContext` is the sole target-neutral owner of value-error
publication.  Publication never discovers a VM context, coroutine, worker, or
AOT recipe.  It observes the exact TLS-active execution context and accepts a
publication only when that context belongs to the expected `XrRuntimeCore` and
has an executor-bound error channel.

Each executor lends one `XrValue` pending-error slot to its execution context.
The execution context owns neither the slot nor its lifetime.  Binding the same
slot is idempotent; rebinding to a different slot fails.  Exact unbinding ends
the borrow before executor storage is reclaimed.  Binding or unbinding an
active context fails so a running publication can never observe a replaced or
dangling slot.  The root and module execution contexts borrow the isolate VM
slot; each VM coroutine execution context borrows its own VM-state slot.  An
executor with no value-error channel remains explicitly unbound.

A pooled VM coroutine may retain its target-owned backend state, including the
physical pending-error slot, but the borrow never crosses an execution
lifetime.  The recycle/reset boundary exact-unbinds before exposing the shell
to a pool.  Reuse initializes a new execution identity and then exact-binds the
retained slot.  Zeroing or reinitializing the execution context is not an
unbind operation.

`xr_exec_context_publish_error_owned` is a move operation.  On success, the
empty destination receives the exact value and the caller's owning source slot
is cleared.  It does not retain, release, clone, or reinterpret the value.  A
null argument/value, no active context, wrong runtime, unbound channel, or
occupied channel is rejected without changing either source or destination.
The first published error therefore wins.  A consumer that chooses to discard
a later rejected error must release it through the storage-domain owner that
provided that token and clear that owner's slot; it may not retry indefinitely
or overwrite the first diagnosis.

Builtin enum construction is independent from publication.  Construction may
produce an immutable sticky module value, but each adapter must still close its
caller-owned token on rejected publication.  Hosted-fragment signals expose a
mutable owning error slot so the adapter can move or release it exactly once.
Generator rejection is released through the producer coroutine heap; thread
terminal observations are released through their recorded transferable/shared
owner.  A missing active context never falls back to `isolate->vm_ctx`.

The independent runtime test fixes active identity, runtime identity,
non-overwrite, move-on-success, rejection stability, and exact borrow lifetime.
VM publication tests additionally prove that executor bindings reach the
existing pending-error consumer without restoring a VM-private setter.

## Digest anchors

anchor-sha256: src/runtime/core/xr_exec_context.h af6e7cdc5e3a00c5e91de9d57fa828fb4b662f670fe8b235e4009fff8b9a0377
anchor-sha256: src/runtime/core/xr_exec_context.c fc953b234bf24e26ef953f9da946603e5f274485ec28a2136417e6d0e0438eec
anchor-sha256: src/coro/xcoro.c 55602c9d05121142e1e69b84caa5946f86b28d15e24df3ca9675fd668209256c
anchor-sha256: src/api/xvm_exec.c a4e92536fec225013dcc807654bd93d346129c1f97fafd59f86e3aa430ec4db3
anchor-sha256: src/vm/xvm_coro_backend.c 0648b8205f74e3fcb0820cfb03d443c0a5f88f3a4b9e327501433368476c0419
anchor-sha256: src/vm/xvm.h c2c4e3c70a464c86be9697c80751c7bf6282cc12e1a1473c15e2cfb7ea0ec7f2
anchor-sha256: src/vm/xvm_api.c e302889a5ee8b5131de99a50b7278edbd20153446e8f12f96b8bc7cdbf2ef883
anchor-sha256: src/coro/xthread_obj.c c289b005477750eaf9ce12d080b40e31b6ae7693c6252feb26bfa1ee33938d4f
anchor-sha256: src/runtime/object/xiterator.c 3e592afd2b7c739222a6c07845a8c559695d6593646010dc6130967e9c35c49e
anchor-sha256: src/runtime/object/xstring_methods.c f6fdadd8b5a8cbd9180159b3a302dcf32b24b0e700848bf7e2c0d129eaab89d5
anchor-sha256: src/stdlib/xstdlib_vm_fastpath.c db6c8ad16cf3939bed1a62923abb735d504270c2f9c9a29ef0b4d8ecccb294db
anchor-sha256: include/xray_hosted_fragment_runtime.h d9d219298d8b4c7ebd0aef72c3becfa7f6224f6686582556a31e4ca65f0834bc
anchor-sha256: tools/stdlibgen/generate_vm_fastpaths.py d4d84f3cb3c19bcc7f12e6ff912a2c1ca15fb2a757bdcd018e98eb4d761e6936
anchor-sha256: stdlib/net/net.c 68e8e1eed3650f6ebfbdbb39e20817e766b62415db1091df53c467011136e4aa
anchor-sha256: tests/unit/runtime/test_execution_error_channel.c 6402bf9a867f5b4a107fb180344b63becd6b142e677d14fb6f421d83185f03b0
anchor-sha256: tests/unit/vm/test_vm_exception.c c7f010047fd2a8b733e8bafc29b527543ba0a750de474cc0b2c63e3c4541bf86
