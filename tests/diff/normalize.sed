# normalize.sed - canonicalize non-deterministic output before three-backend diff.
#
# Only filter fields that are genuinely non-deterministic across runs/backends
# (addresses, generated identifiers tied to pointers). NEVER use this to mask a
# real semantic difference: if outputs differ for any other reason, fix the
# backend, do not add a rule here. See tasks/107.
#
# Used by run_backend_diff.sh on stderr (and optionally stdout) of each backend.

# Hex pointer addresses: 0xdeadbeef -> 0xPTR
s/0x[0-9a-fA-F][0-9a-fA-F]*/0xPTR/g

# Anonymous object/enum renderings carrying an address: <enum@0xPTR> -> <enum@PTR>
s/@0xPTR/@PTR/g
