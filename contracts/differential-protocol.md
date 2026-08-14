# Differential and oracle protocol contract

Status: re-frozen by tasks 256 and 257. Manifest, argument, and baseline
sidecars are decoded explicitly as UTF-8 on every host; subprocess capture and
the program-output comparison remain byte-native and are not normalized.

1. Backend differential equivalence compares observable exit status and stdout
   byte-for-byte. Normalized stderr is compared only when the lane explicitly
   enables that channel; build logs are not program output. The harness must not
   decode any form's program output before this comparison.
2. Three execution forms are subject to this contract, and a lane names the ones
   it compares: `xray run` (VM), `xray build --native` (AOT), and the default
   `xray build`, whose binary carries the program as bytecode serialized into
   the constant-pool container. The default build is a compared form, not a
   packaging detail: it reaches the program through a serialize/deserialize
   round trip no other form performs.
3. A checked-in `.expected` sidecar is an exact stdout oracle and requires a
   zero exit status. A `.stdin` sidecar supplies identical bytes to each form.
   A directory `xray.toml` is part of the case identity and supplies the same
   NativePackagePlan to every lane.
4. KAT/oracle data records its upstream version or generator, uses deterministic
   inputs, and is evaluated independently of the implementation under test.
5. Fuzz failures preserve seed/corpus evidence sufficient for exact replay.
6. A tombstone classifies a known failing verification asset; it does not alter
   semantic equivalence. Tombstones may only shrink, and a newly passing case is
   removed rather than reclassified.
7. Changing compared channels, compared forms, normalization, expected-output
   behavior, oracle provenance, or tombstone meaning is a contract change and
   requires all dependent suites/ports to be revalidated.

## Digest anchors

anchor-sha256: tests/diff/run_backend_diff.py 012deedc8224cfca08f674c61f495fd0e46cb1a95e1c39f6fdb39822e39b65f2
anchor-sha256: tests/aot/TOMBSTONES.tsv 1ad7d280093c5a3aedecdf490fe88dc9c48f79215de9ea1d1c8216373cd56eb7