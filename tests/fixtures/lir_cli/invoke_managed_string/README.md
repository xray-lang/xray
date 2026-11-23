# Owned String results from direct error calls

Run the cases with `run_lir_invoke_try_assert.py --fixtures tests/fixtures/lir_cli/invoke_managed_string`.
The runner compares binary stdout and stderr from the register VM and the compiled native executable.

- `success` transfers a dynamically concatenated String through `try` and `try!`. The byte oracle includes an embedded NUL between UTF-8 characters.
- `propagate_error` forwards `TextFailure.Refused(-23)` through the same String-returning function. The relay marker and the following print are absent.
- `optional` alternates two successes and two business failures through the same call site. Each failure runs one fallback. The always-throwing String function adds a third fallback without a normal return.
- `panic` runs an ordinary division-by-zero panic inside a throwing String function. The fallback and the following print are absent, and the runner requires the original E0420.
- `plain` uses the same ordinary String construction and division functions. This control pins the UTF-8 and NUL bytes and the original panic on a path without a typed error call.

On Windows Release `41109fe60` with a clean build, all five cases pass `check`.
The ordinary control builds and executes in both backends with identical bytes and exit 1.
The other four enter LIR lowering and report the same `INVOKE` diagnostic in the VM, C emission and native build.
Their runtime expectations are recorded in `cases.json`.
