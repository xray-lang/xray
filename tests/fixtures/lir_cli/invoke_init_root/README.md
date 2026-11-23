# Startup error propagation

The entry in `entry.xr` imports a chain from `prerequisite.xr` through `alpha.xr`,
`beta.xr` and `dependent.xr`. The initializer at each step consumes the previous
module's exported value. The last value and the printed markers establish the
actual startup order.

Build `entry.xr` once. Set `XRAY_INIT_ROOT_FIXTURE_FAIL` from the selected
`cases.json` row only when running the VM or the native executable. The selector
consumes the nullable environment String in an ordinary function and returns a
Bool before a throwing initializer call.

`alpha_failure` observes the successful prerequisite and the start of Alpha.
The rest of Alpha, Beta, the dependent module and the entry are absent from the
expected stdout. `beta_failure` also observes all of Alpha and the start of Beta.
The rest of Beta, the dependent module and the entry are absent. The two variants
have the same spelling, but the exact nominal types and their `i64` payloads
are different. Each initializer uses its own concrete error enum. The `success`
row reaches the entry after both initializers finish. `control_entry.xr` follows
the same dependency order with ordinary, nonthrowing calls.

`slotless.xr` declares and throws its own enum at the entry. It has no module
slots or throwing helper functions. Its terminal result retains the nominal
identity and scalar payload and leaves the following print unexecuted.

`test_lir_cli_invoke_init_root` runs these cases with the same byte checks as the
other typed error gates. Pass `--case control` to the runner to run only the
ordinary control.

On the clean Windows baseline `5c7ff13e4`, all entry sources pass `check`. Rows
using `entry.xr` reach `alpha.xr:14:7` in VM, C-only and native build and report
`error-capable CALL without INVOKE is not supported by LIR lowering yet`.
The slotless entry reports `THROW is not supported by LIR lowering yet` at
`slotless.xr:4:1` across the same commands. Their stdout is empty. The runtime
outcomes in `cases.json` describe the result after startup propagation is
implemented. The baseline's control emits C, builds with MSVC and matches the
VM's stdout and stderr byte for byte.
