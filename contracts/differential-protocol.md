# Differential and oracle protocol contract

Status: re-frozen by task 237.

1. Backend differential equivalence compares observable exit status and stdout
   byte-for-byte. Normalized stderr is compared only when the lane explicitly
   enables that channel; backend build logs are not program output.
2. A checked-in `.expected` sidecar is an exact stdout oracle and requires a
   zero exit status. A `.stdin` sidecar supplies identical bytes to each backend.
   A directory `xray.toml` is part of the case identity and supplies the same
   NativePackagePlan to both VM and AOT lanes.
3. KAT/oracle data records its upstream version or generator, uses deterministic
   inputs, and is evaluated independently of the implementation under test.
4. Fuzz failures preserve seed/corpus evidence sufficient for exact replay.
5. A tombstone classifies a known failing verification asset; it does not alter
   semantic equivalence. Tombstones may only shrink, and a newly passing case is
   removed rather than reclassified.
6. Changing compared channels, normalization, expected-output behavior, oracle
   provenance, or tombstone meaning is a contract change and requires all
   dependent suites/ports to be revalidated.

## Digest anchors

anchor-sha256: tests/diff/run_backend_diff.sh 820a553a08ccb19aa5e4e1d6ba2dc0679870e82d2b7e7206525a9f5c8af3d45f
anchor-sha256: tests/diff/run_backend_diff_fast.py f7bd789fda47949ca80e24dc2cebe23a901bf2858b18e1afd8125ee658a9fc8e
anchor-sha256: tests/aot/TOMBSTONES.tsv 01db981e6f0742672435ae3871e0f072f6a73f6f17d1ba3d09fdfeee0983a2a7
