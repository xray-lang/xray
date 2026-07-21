# Differential and oracle protocol contract

Status: frozen by task 220.

1. Backend differential equivalence compares observable exit status and stdout
   byte-for-byte. Normalized stderr is compared only when the lane explicitly
   enables that channel; backend build logs are not program output.
2. A checked-in `.expected` sidecar is an exact stdout oracle and requires a
   zero exit status. A `.stdin` sidecar supplies identical bytes to each backend.
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

anchor-sha256: tests/diff/run_backend_diff.sh aafc77b058ff221fd9795b92547ad1120119049f093b7bbf01724e2417a23423
anchor-sha256: tests/diff/run_backend_diff_fast.py d11d910b6a5105b050fd68e9f0c7d3f278c37cb6b9ce9501993d620d8d5b7f8e
anchor-sha256: tests/aot/TOMBSTONES.tsv 01db981e6f0742672435ae3871e0f072f6a73f6f17d1ba3d09fdfeee0983a2a7
