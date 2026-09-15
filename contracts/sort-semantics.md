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

## Verification

verification-test: test_array_core
