# Blocker: `Array<i64>(n)` hands back reused, un-zeroed storage

- **Lane**: 6-6 (compress → Xray)
- **Status**: `WORKED AROUND in compress.xr; the defect itself is open`
- **Requested owner**: whoever owns array construction lowering
- **Severity**: silent wrong answers. A function that builds a histogram or any
  other zero-initialised accumulator returns different results on its second
  call than on its first, with no error and no diagnostic. Two *different*
  functions that both construct an array of the same length share the storage.

## Exact source identity

| item | value |
|---|---|
| base commit | `34be0379c` |
| worker branch | `work/6-6-compress-xray-34be0379c` |
| build | `cmake -S . -B build-nofp -G Ninja -DCMAKE_BUILD_TYPE=Release -DXRAY_STDLIB_VM_FASTPATHS=OFF` |
| command | `./build-nofp/xray run <file>` |

## Minimal case

```xray
fn bump() -> i64 {
    var a = Array<i64>(8)
    a[3] = a[3] + 1
    return a[3]
}

print(bump().toString() + " " + bump().toString() + " " + bump().toString())
```

Prints `1 2 3`. Each call must print `1`: `Array<i64>(8)` is specified to
produce eight zeros.

## Which forms are affected

Probed side by side in one program:

| construction | 3 consecutive calls | verdict |
|---|---|---|
| `Array<i64>(8)` | `1 2 3` | **broken** |
| `Array<i64>(8, 0)` | `1 1 1` | ok |
| `Array<u8>(8)` | `1 1 1` | ok |
| `Array<u8>(8, 0)` | `1 1 1` | ok |
| `Array<i64>()` + `push` | `1 1 1` | ok |
| `Array<i64>([0,0,0,0])` | `1 1 1` | ok |
| `Array.withCapacity(8)` | `1 1 1` | ok |

Only the single-argument `i64` form. The size does not matter -- a literal
`17`, a `const`-derived `N + 1` and a runtime parameter all reproduce it.

## The storage is shared across functions, not just across calls

```xray
const N: i64 = 16

fn histA(values: Array<i64>) -> i64 {
    var counts = Array<i64>(N + 1)
    for (var i = 0; i < len(values); i++) { counts[values[i]] = counts[values[i]] + 1 }
    return counts[7]
}

fn histB(values: Array<i64>) -> i64 {   // byte-identical body, different name
    var counts = Array<i64>(N + 1)
    for (var i = 0; i < len(values); i++) { counts[values[i]] = counts[values[i]] + 1 }
    return counts[7]
}

var sevens = Array<i64>(5, 7)
print(histA(sevens).toString() + " " + histB(sevens).toString() + " " + histA(sevens).toString())
```

Prints `5 10 15`. `histB` sees what `histA` wrote.

## How it surfaced

compress.xr builds two Huffman code tables: the encoder derives canonical codes
from a length vector, and the decoder derives count/symbol vectors from one.
Both start with `var counts = Array<i64>(MAX_BITS + 1)`. Compressing anything
before decompressing anything left the decoder's table pre-loaded with the
encoder's histogram, so `buildHuffman` rejected the standard fixed Huffman code
as over-subscribed and every decode threw `CompressionError.InvalidData`.

It reproduced only in that order. `inflate` alone was correct; `deflate` then
`inflate` was not, which reads exactly like a decoder bug and is why it took a
probe of the construction forms to find.

The counts made the sharing unmistakable. Expected `0 0 0 0 0 0 0 24 152 112`;
observed `0 0 0 0 0 30 0 48 304 224` -- the 30 is the distance table's thirty
five-bit codes, and 48/304/224 are 24/152/112 doubled, i.e. the encoder's two
`canonicalCodes` calls plus the decoder's, all accumulated into one array.

## Why base64 never hit it

`base64.xr` uses `Array<u8>(n)`, which is unaffected. Nothing else in the tree
appeared to build an `Array<i64>(n)` accumulator.

## Workaround in place

Every `Array<i64>(n)` in `stdlib/compress/compress.xr` spells its fill:
`Array<i64>(n, 0)`. That is correct but it is a workaround -- the single-argument
form is documented as zero-filled in `stdlib/types/array.xr` and reads as such
at every call site in the tree.
