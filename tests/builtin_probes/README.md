# Built-in symbol probes

One minimal program per language-surface symbol in
`stdlib/prelude/builtin_symbols.def`. `scripts/check_builtin_symbol_registry.py`
(rule R3) requires that **both** `xray check` and `xray run` succeed for every
probe here, and that no registry symbol lacks a probe.

Both modes are checked because `check` alone is not sufficient: a generic
instance used as an enum payload passed `xray check` and only failed in the
post-monomorphization analysis pass, so the symbol looked usable while every
real build of it was red.

A probe proves the symbol **is nameable**: that its spelling resolves at the
arity the registry records. Where a value is cheap to build the probe also
constructs and uses one; where it is not (`NetConn`, `Ptr<T>`, `CFn<...>`, and
the other handle types), naming the type in a signature is the whole point and
the probe stops there. Behavioural coverage for those types lives in
`tests/regression/` and `tests/diff/`, not here.

The file name is the lowercased symbol name; the gate derives it that way.
