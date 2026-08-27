# Array sort semantic contract

`Array.sort` is an in-place, unstable sort. The implementation may reorder
elements that compare equal and does not expose comparator call count or pivot
selection as language semantics.

The default order is fixed as follows:

- integers use mathematical signed or unsigned order for their typed lane;
- floating-point values use numeric order, `-0.0` and `+0.0` compare equal,
  and every NaN compares equal after every non-NaN value;
- mixed tagged integer/float values use the same floating-point rule after
  conversion of the integer;
- strings use lexicographic UTF-8 byte order, runes use Unicode scalar value,
  and booleans order `false` before `true`;
- values outside a common ordered category compare equal on the dynamic path.

A custom comparator result is interpreted only by sign. Integer and finite
floating-point results below, equal to, or above zero mean less, equal, or
greater. NaN and nonnumeric results mean equal. A thrown comparator error
propagates through the active backend; the sort does not catch it.

The canonical introsort in `src/shared/xr_sort_core.h` owns ordering and
partition semantics. VM and AOT files are representation adapters only. A
default typed sort operates directly on native storage without boxing or heap
allocation. A custom comparator over typed storage uses one boxed scratch
array because the language callback observes values. Dynamic tagged storage is
sorted in place.

## Digest anchors

anchor-sha256: src/shared/xr_sort_core.h 12d05da0942da491ae99d49729a27f0dfc71e978f0d3992666624fc150d10cdf
anchor-sha256: src/runtime/object/xarray_vm.c 82685c3f19e9ea25f34fec818c2a2eceaca4aac05357f2115c74fb826505a8fc
anchor-sha256: src/aot/xrt_sort.inc.c cf6f34970c83984758e2950a3edf425f7a52a5aaa401fd757539a9c9f6983f42
anchor-sha256: tests/diff/cases/semantics/collections/array_sort_shared_core.xr e37060c9121881428f5950646cc95747c82ecf3b82455001b37124a91fcbefd1
anchor-sha256: tests/unit/stdlib/test_array_core.c 8a916119ac2438423428887b26b9f40db5a52d6f3b778e330b7b7bda276a44fa