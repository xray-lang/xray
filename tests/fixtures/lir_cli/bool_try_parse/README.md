# bool.tryParse fixtures

`bool_try_parse_values.xr` parses whole boolean texts on both backends: the
admitted spellings are exactly `true` and `false`, surrounding spaces, tabs,
line breaks and carriage returns are allowed, the word is case-sensitive, and
every other text yields `null`. Its trailing lines fold one `Some` and one
`None` at compile time, so the CTFE path is pinned separately from the default
operator.

`bool_try_parse_next_to_aggregate.xr` keeps three optional bools beside an
`Array<i64>` in the same aggregate region, because a `bool?` home is two bytes
with a one-byte payload at offset one.

`bool_try_parse_rejects_bool_call.xr` pins the removed `bool(x)` conversion:
conditions take bool values, and text becomes a bool only through
`bool.tryParse`.

Run the cases with
`run_lir_invoke_try_assert.py --fixtures tests/fixtures/lir_cli/bool_try_parse --compiler <cc>`.
