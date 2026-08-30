# AOT Filetests

This directory is a minimal scaffold for 098 M8 AOT plan/file tests. It is
separate from `tests/aot/run_aot_tests.py`, which remains the VM-vs-AOT runtime
diff suite.

Run:

```sh
tests/aot/run_aot_filetests.py
tests/aot/run_aot_filetests.py --mode rep
tests/aot/run_aot_filetests.py --mode all ./build/xray
```

Modes:

- `rep`: representation decisions.
- `layout`: native struct/class/container layout decisions.
- `abi`: native/tagged function ABI decisions.
- `boundary`: adapters and tagged/native crossing points.
- `link`: cross-module AOT plan/link facts.
- `cgen`: generated-C shape facts.
- `all`: every mode, used by default.

Current behavior:

- The runner first probes `xray build --native --dump-xaot-plan`.
- A missing compiler binary, a failed dump hook probe, or an empty selected mode
  is an infrastructure failure and exits nonzero; it is never reported as a
  skipped suite.
- `_*.xr` files are helper modules and are not collected as tests.
- `.expect` files with no active directives are treated as pending and skipped.
- Individual deliberate skips remain visible, but a run with no passing or
  failing case verdicts exits nonzero because it produced no measurement.

Expectation directives:

```text
args=extra xray build arguments for this test, for example --target x86_64-linux-musl
status=pass or fail for the primary dump/verify command; the default is pass
product_status=fail to require the product build to fail after a successful contract check
product_contains=literal text that must appear in product-command stdout/stderr
artifact=absent to require that the requested product path was never materialized
contains=literal text that must appear
not_contains=literal text that must not appear
regex=extended regular expression that must match
not_regex=extended regular expression that must not match
c_contains=literal text that must appear in generated C
c_not_contains=literal text that must not appear in generated C
c_regex=extended regular expression that must match generated C
c_not_regex=extended regular expression that must not match generated C
c_count=N:literal text that must occur exactly N times in generated C
c_syntax=pass to require the generated C to compile
skip=reason for a deliberate temporary skip
```

Plan directives inspect stdout/stderr from
`xray build --native --dump-xaot-plan --dump-link-manifest`.
`c_*` directives inspect the generated `.c` output.

The directive language is closed and fail-closed. Unknown keys, empty values,
duplicate singleton controls, duplicate `product_contains` values, and invalid
enum values are parse failures before any compiler process starts. Assertion
directives may repeat with distinct values. `product_contains` and
`artifact=absent` require `product_status=fail`; that mode cannot be combined
with top-level `status=fail`, and its absent artifact cannot be combined with
`c_*` or `c_syntax` assertions.

An expected product failure bypasses the successful dump/C cache. Any contract
first has to verify successfully, after which the product command runs once
under a finite per-case timeout. It must fail, every `product_contains` token
must occur in that command's own stdout/stderr, and the output path must be
absent both before and after execution. Regular files, directories, live
symlinks, dangling symlinks, and filesystem classification races all fail the
artifact assertion.
