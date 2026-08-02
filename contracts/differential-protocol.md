# Differential and oracle protocol contract

Status: re-frozen by tasks 256 and 257. Manifest, argument, and baseline
sidecars are decoded explicitly as UTF-8 on every host; subprocess capture and
the VM/AOT program-output comparison remain byte-native and are not normalized.

1. Backend differential equivalence compares observable exit status and stdout
   byte-for-byte. Normalized stderr is compared only when the lane explicitly
   enables that channel; backend build logs are not program output. The harness
   must not decode either backend's program output before this comparison.
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

anchor-sha256: tests/diff/run_backend_diff.sh 2065d507708a4ab8d3f967b78abd49cd387327fac44f7b40131dc4c5675d5dfb
anchor-sha256: tests/diff/run_backend_diff_fast.py fdcd4f78e214d28e8b946ef07fad4c7c782a15ad3dcbfe661d481be117ff8f33
anchor-sha256: tests/aot/TOMBSTONES.tsv f9c9208800fbef44b4802b27e903f0cf95f48d0b7e50c5c9818ec2ddd2802016
