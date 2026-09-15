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

## Verification

verification-test: test_execution_error_channel
verification-test: test_vm_exception
