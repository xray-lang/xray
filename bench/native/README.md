# Native execution benchmarks

The C, Xray and CPython programs perform the same work and print one checked
integer result. These workloads establish a new baseline; they do not reuse
timings from earlier benchmark implementations.

| Case | Full size | Work |
|---|---:|---|
| `fib` | 32 | Recursive Fibonacci |
| `integer_loop` | 100,000,000 | Sum `i % 97` to preserve a real arithmetic loop |
| `push_sum` | 10,000,000 | Append to a growing array, then sum by index |

Build `native_bench_reference` in the normal Ninja Release tree. Build `xray`
before measuring the current VM. The runner uses Python 3.12 and launches
commands without a shell. Run in an initialized MSVC UTF-8 console on Windows.

```powershell
cmake --build D:\xray-build\c-rel --target native_bench_reference xray -j4
python bench/native/run.py --reference D:\xray-build\c-rel\native_bench_reference.exe --xray D:\xray-build\c-rel\xray.exe --cpython --output D:\xray-build\c-tmp\bench
```

Every run creates a new directory containing CSV medians, JSON metadata and
samples, generated source inputs, and each process's unmodified stdout/stderr.
Metadata includes the machine, compiler version, source commit and dirty state,
binary SHA-256 identities, and the fresh CLI version JSON. Changing a binary
during measurement invalidates every affected candidate row and clears its
median while preserving the original samples. Metadata records the final
`run_status`, before/after identities and the affected candidates. Provisional
successful rows remain `pending-identity` with no median until final identity
verification. The resolved, hashed absolute executable path is also the path
used for execution, including when launched from another directory.
Five samples are the default, interleaving
candidates within each repetition. Process startup is included; no warm-up is
hidden. Timing includes process-tree setup and cleanup as part of end-to-end
wall-clock execution. A median is written only if every sample returns the exact expected
bytes. Text output explicitly uses LF, including on Windows; observed output
is never normalized. Python workload source is copied into each run directory
and its hash is recorded. A failure returns a nonzero status even if other candidates pass.

`quota-failed`, `failed`, `launch-failed`, `timeout`, `wrong-output` and
`binary-changed` retain their original
diagnostics and have an empty median. A quota failure is not a passing run.
Samples use the shared owned-process runner with a timeout. Windows Job
ownership is established before resuming the driver; cleanup terminates its
descendants and waits for retained member handles to become signaled. Output
goes directly to retained files, so inherited pipes cannot hold the runner open.
If cleanup cannot be verified, the run stops, records `cleanup-failed`, and
invalidates all medians before any further sample can run. POSIX uses an owned
process group; the current validation evidence is Windows only.
`--quick` runs small functional workloads. `--samples 1` is useful for diagnosis;
neither is a performance measurement. The default `measurement=coarse` means
results cannot establish a performance gate. Use `--measurement isolated` only
in the team's exclusive measurement window with no other builds, full sizes,
and five samples; store formal output under `D:\xray-build\bench\c`.
The annotation records the operator's measurement context; it does not acquire
a build slot or establish isolation by itself.

`native_bench_runner` is a small L1 test: it checks real C/CPython outputs and
ensures failures never produce medians. Full CTest performance cases require
explicit configure option `-DXRAY_REGISTER_BENCHMARKS=ON`; they carry only the
`bench` label and are serial. Leave that option off for an ordinary L1 tree.

Additional drivers, such as `lir_c_run` and `lir_vm_run`, can be supplied with
`--candidates` using a JSON list. Each entry contains a unique lowercase `name`
and a tokenized `command`, for example:

```json
[{"name":"lir-vm","command":["D:/xray-build/c-rel/lir_vm_run.exe","{source}"]}]
```

Only `{source}`, `{case}` and `{n}` placeholders are substituted. Driver
availability and real end-to-end output must be verified before interpreting
their rows as backend coverage. The scalar bytecode prototype is not a source
driver and is not represented as one by this runner.
