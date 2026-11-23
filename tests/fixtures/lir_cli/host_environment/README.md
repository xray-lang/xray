# Hosted environment fixtures

Apply each row's `unset_env` removals and `set_env` assignments to the child
process environment for VM execution and for the compiled native executable.
Set empty entries through the process environment so Windows receives the
variable with a zero-length value. Keep the compiler's environment intact,
including its temporary directory.

Each executable case expects exit 0, empty stderr, and the stated stdout
bytes. The `lookup` and `same_name` rows depend on the environment-read leaf.
The `temp_root_*` rows also require the platform result used by the source
fallback. They pin `TMPDIR`, `TMP`, `TEMP`, and the host-default order.
