#!/usr/bin/env python3
"""Keep task 199 P3 internal until scoped grapheme provenance is available."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]


def main() -> int:
    public_string = (ROOT / "stdlib" / "types" / "string.xr").read_text(encoding="utf-8")
    native_type_snapshot = (
        ROOT / "src" / "frontend" / "analyzer" / "xnative_type_defs.inc.c"
    ).read_text(encoding="utf-8")
    vm_methods = (ROOT / "src" / "runtime" / "object" / "xstring_methods.c").read_text(
        encoding="utf-8"
    )
    aot_methods = (ROOT / "src" / "aot" / "xrt_method.h").read_text(encoding="utf-8")
    generic_iterator_h = (ROOT / "src" / "runtime" / "object" / "xiterator.h").read_text(
        encoding="utf-8"
    )
    generic_iterator_c = (ROOT / "src" / "runtime" / "object" / "xiterator.c").read_text(
        encoding="utf-8"
    )
    failures = []
    if "graphemes" in public_string:
        failures.append("stdlib/types/string.xr publishes graphemes before provenance integration")
    if "graphemes" in native_type_snapshot:
        failures.append("native type snapshot publishes graphemes before provenance integration")
    if '"graphemes"' in vm_methods:
        failures.append("xstring_methods.c registers public graphemes before provenance integration")
    if "graphemes" in aot_methods:
        failures.append("xrt_method.h exposes graphemes before direct-loop lowering")
    if "grapheme" in generic_iterator_h or "grapheme" in generic_iterator_c:
        failures.append("generic XrIterator contains grapheme fallback instead of stack cursor")
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}")
        return 1
    print("OK: task 199 grapheme iterator remains VM-internal")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
