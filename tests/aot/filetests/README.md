# AOT Filetests

This directory is a minimal scaffold for 098 M8 AOT plan/file tests. It is
separate from `tests/aot/run_aot_tests.sh`, which remains the VM-vs-AOT runtime
diff suite.

Run:

```sh
tests/aot/run_aot_filetests.sh
tests/aot/run_aot_filetests.sh --mode rep
tests/aot/run_aot_filetests.sh --mode all ./build/xray
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
- If that dump hook is not available yet, it prints `SKIP`/`待实现` and exits 0.
- `_*.xr` files are helper modules and are not collected as tests.
- `.expect` files with no active directives are treated as pending and skipped.

Expectation directives:

```text
args=extra xray build arguments for this test, for example --target x86_64-linux-musl
contains=literal text that must appear
not_contains=literal text that must not appear
regex=extended regular expression that must match
not_regex=extended regular expression that must not match
c_contains=literal text that must appear in generated C
c_not_contains=literal text that must not appear in generated C
c_regex=extended regular expression that must match generated C
c_not_regex=extended regular expression that must not match generated C
skip=reason for a deliberate temporary skip
```

Plan directives inspect stdout/stderr from
`xray build --native --dump-xaot-plan --dump-link-manifest`.
`c_*` directives inspect the generated `.c` output.
