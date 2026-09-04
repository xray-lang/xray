# Process byte-stream and Windows Unicode contract

Status: frozen by task 257.

Xray makes encoding a property of a declared text protocol, not a property of
an operating-system pipe.  The contract is an atomic clean-slate boundary:

1. Subprocess stdin, stdout, and stderr are byte streams. Capture APIs expose
   byte storage, an explicit length, and independent truncation state for each
   stream. Embedded NUL and invalid UTF-8 bytes are ordinary captured data.
2. A consumer may decode only after naming a stable protocol. Xray-owned JSON,
   source, and line protocols use strict UTF-8; narrow machine protocols may
   use strict ASCII. Provider banners, compiler diagnostics, and arbitrary
   program output remain bytes and are inspected with bounded ASCII markers or
   escaped into an ASCII diagnostic representation. Replacement decoding is
   not a valid error policy.
3. Xray language strings remain valid UTF-8. This invariant does not imply that
   inherited process output is UTF-8 and does not add a global output codec.
4. Windows process input is Unicode. Compiler and embedded AOT process shims
   convert Xray UTF-8 arguments, cwd, and environment overrides strictly to
   UTF-16 and use `CreateProcessW` with a Unicode environment block. Invalid
   UTF-8 fails before process creation. Windows environment enumeration uses
   the wide API and converts to Xray UTF-8 explicitly.
5. Default provider execution retains inherited stdout and stderr. `xray build`
   does not add a pipe, capture buffer, decoder, UTF-8 scan, console rewrite,
   or output copy. Runtime print paths write already-valid Xray string bytes
   directly. Text classification is confined to explicit tooling/probe paths.
6. Python subprocess calls are binary by default. Text mode is permitted only
   with a literal `encoding="ascii"` or `encoding="utf-8"` and
   `errors="strict"`; repository gates inventory and enforce every call site.
   `Path.read_text` and `Path.write_text` likewise name ASCII or UTF-8
   explicitly and never depend on the host locale or a lossy error policy.
7. Process-spawn performance is checked separately from structural zero-cost
   invariants. A regression is actionable only when its paired 95% interval is
   above parity and its median delta exceeds 1% or p95 delta exceeds 3%.

Changing byte ownership, capture length/truncation semantics, accepted text
protocols, Windows process API family, or default provider/print-path work is a
contract change.

## Digest anchors

anchor-sha256: src/os/win/proc_win.c 9b80d5863a693d820cc384395255e2561d9f8ec05d3cb0ff892ad793d342962d
anchor-sha256: src/aot/xrt_sys.h 98bb64c88ae42a291311c6f5b4a63649999ec00d7f81f46880ac1810c0b9bfad
anchor-sha256: src/aot/xrt_os.h 172494e5ff814e2b50b9b7691038b3b527711b02082ef75df394021ac59c68b4
anchor-sha256: src/app/toolchain/xtc_process.h 767193396e0e59c7d35a2121f0a9fcd225987f039aa0d305a5b98d8ef28fd6dc
anchor-sha256: src/app/toolchain/xtc_process.c a9fc712a024d642f2f90ed2ebd959fdd08845f26a3fba0fa020d51e3179b2d41
anchor-sha256: scripts/check_subprocess_text_boundaries.py 5815773c084b6eecb845acc160b32a2841090e2bac6643fbe0708ec4897e2a81
anchor-sha256: scripts/check_process_zero_cost.py 464e7cdf40fd04fe7c6fcd1d5e256c807d254ad7994f6383c4603c8c26669bf5
anchor-sha256: tests/probes/rc/check_execution_arena_l2.py 7a14cf923ba7e67933a3c067afcd25ad7c3bba36fceb1f7861620500117b78c9
anchor-sha256: tests/probes/rc/check_mutable_capture_cell_rss.py 8434351cbe242e73ef1d3b7d19992d1996764d8534dab9f57a5ffbd5402be380
anchor-sha256: tests/unit/cli/test_cli_toolchain.c 49b2dd41ed2668cfce6c2d799c398bfc52da774356cf91d909588f3cea6940f7
