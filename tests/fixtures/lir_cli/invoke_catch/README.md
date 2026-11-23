# Catching one nominal error type

Run the cases with `run_lir_invoke_try_assert.py --fixtures tests/fixtures/lir_cli/invoke_catch`.
The runner compares exact stdout, stderr and exit status from the register VM and the native executable.

- `typed_direct` uses two fallible calls with the same error type and a plain `Never` call in one try body. The all-success input runs the whole body. On failure the typed handler reads the original payload and execution resumes after the try.
- `ordered_propagation` tests a range pattern before a broader pattern for the same variant. An unmatched variant escapes with its original name and payload, and the final print is unreachable.
- `same_name_modules` puts the other module's equally named error type first. Only the handler for the defining module runs.
- `panic` checks that division by zero keeps E0420 and bypasses the business handler and subsequent prints.
- `unused_binding` runs the catch-all with an unread Error binding.
- `state` reads the latest scalar binding from both of the actual failure predecessors.
- `startup` lets a matching case continue module initialization, then reports an unmatched error with its original payload before later code can run.
- `bare_name` pins E0334 for a bare uppercase catch binding.

At clean Windows Release `f24409284` the first five runtime cases passed Source checking. Each stopped at the same `INVOKE` diagnostic in the VM, C emission and native build. The E0334 refusal matched in all four commands.
