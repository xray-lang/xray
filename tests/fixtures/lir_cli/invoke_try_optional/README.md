# Optional error calls

The cases use the byte oracles of `run_lir_invoke_try_assert.py` with this fixture
directory. Their error enums hold only scalar fields or a unit variant.

- `scalar` produces a non-optional integer and a business failure. An ordinary
  function then consumes two different nominal errors in one `try?` expression.
  Its markers distinguish the all-success order, failure of the first operand,
  and failure of the middle operand. The last operand has a visible effect.
- `flatten` preserves the `i64?` result type and exercises an existing null, an
  existing value, and a null produced from a business failure.
- `unit` uses distinct fallback markers to check that successful `Unit` skips
  its fallback and a business failure runs the other fallback once.
- `state` preserves a live local scalar binding on both exits and checks the
  selected fallible-call and fallback effects before reading that binding.
- `panic` divides by zero during the throwing call. The ordinary panic keeps
  its root diagnostic and the fallback and following print stay unexecuted.
- `redundant` pins E0336 for an ordinary call. `unparenthesized` pins the parser
  diagnostic for the required parentheses before `??`.

On the clean Windows baseline `12aff181d`, the runtime cases pass `check` and
stop at `INVOKE` in the VM, C-only emission, and native build. Their runtime
outputs in `cases.json` are the test contract for the optional-call slice. Both
Source refusals are already identical across `check` and both execution paths.
