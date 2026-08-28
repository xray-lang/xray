# Blocker: the stdlib_data fuzzer crashes on a committed seed, in VM teardown

- **Lane**: 8 (test discipline wiring), round 3
- **Status**: `OPEN` — found, not fixed. The defect is in `src/`, which is
  outside this lane's allowed file set.
- **Severity**: a use of uninitialized heap state during `xray_vm_delete`,
  reached by a 17-byte CSV that has been in the repository as a fuzz seed.

## What happens

`tests/fuzz/fuzz_stdlib_data` aborts on its **second** execution unit, before
libFuzzer has mutated anything (`MS: 0`, stack frame
`ReadAndExecuteSeedCorpora`). The reproducer is byte-identical to the committed
seed `tests/fuzz/corpus/stdlib_data/00_csv_basic.txt`
(md5 `ca9801035d47c2e5da2be670e64ad3bd`) — an ordinary CSV.

```
src/mem/xcoro_heap.h:346:11: runtime error: member access within misaligned
address 0xbebebebebebebebe for type 'XrCoroHeap', which requires 8 byte alignment

==ERROR: AddressSanitizer: SEGV on unknown address 0x17d7d857d7d9d7df
    #0 xr_coro_heap_sub_external   src/mem/xcoro_heap.h:346
    #1 xr_obj_destroy_array        src/runtime/object/xarray.c:1081
    #2 xr_fixed_heap_finalize      src/runtime/mem/xfixed_heap.c:47
    #3 xray_vm_delete              src/api/xisolate.c:165
    #4 run_source                  tests/fuzz/fuzz_stdlib_data.c:182
```

`0xbebebebebebebebe` is ASan's malloc fill pattern, so the pointer being
dereferenced was never initialized. The crash is in **teardown**, not in the
CSV parser: parsing succeeds and `xray_vm_delete` then walks an array whose
coroutine-heap back-reference is garbage.

## What is and is not new

Being explicit, because it would be easy to overstate this:

- The crash is **not** caused by the instrumentation added in this branch. An
  uninstrumented build of the same harness crashes too, as
  `SEGV in xr_obj_destroy_array+0x1bc` — a bare address and no explanation.
- What the instrumentation changed is legibility: ASan and UBSan turn that bare
  address into `xcoro_heap.h:346`, the fill-pattern signature, and the four
  frames above.
- What kept it hidden is neither of those. It is that **nothing had ever run
  this harness**. `grep -rn fuzz .github/` matched no workflow line before this
  branch; the four harnesses in `tests/fuzz` had a documented build recipe, a
  45-file seed corpus, and zero callers.

A defect reachable from a checked-in seed, in a teardown path every embedder
executes, sat undetected for as long as its only detector was unwired.

## Reproduction

```bash
cmake -B build-fuzz -G Ninja -DENABLE_FUZZING=ON \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-fuzz -j --target fuzz_stdlib_data
./build-fuzz/tests/fuzz/fuzz_stdlib_data tests/fuzz/corpus/stdlib_data/00_csv_basic.txt
```

It reproduces on the first run; no mutation and no timing window is involved.

## Why the nightly lane is informational

`.github/workflows/nightly.yml`'s `fuzz-smoke` job carries
`continue-on-error: true` with this blocker named in its comment. That is
deliberate and temporary: the lane is red on arrival for a defect that predates
it, and a lane that is red from its first night teaches people to ignore it —
the exact failure the `full-suite` comment in the same file warns about. The
removal condition is this file: fix `xcoro_heap.h:346`, delete the
`continue-on-error` line.

This is unlike `linux-msan`, whose `continue-on-error` is permanent because
GitHub's Ubuntu images ship uninstrumented system libraries. Nothing here is
unfixable.

## Adjacent facts worth keeping

- `fuzz_xtp_decode` runs at about **2 executions per second** (against 282/s
  for the parser and 40,000/s for the lexer), because each input builds a full
  valid XTP artifact and decodes it twice. Its 120s budget therefore buys a few
  hundred executions. Raising the timeout is not the lever; persisting the
  corpus between nights is.
- Before the global instrumentation this branch adds, `libxray_core.a`
  contained **zero** `__asan_*` references. ASan was not watching the library
  at all — only each harness's own hundred lines of glue.
