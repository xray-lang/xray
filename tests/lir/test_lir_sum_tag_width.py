"""Check that stored tags keep two byte and four byte variant indices distinct.

A tag narrower than the variant count would make the last variant alias the
first one, so the matching arm for `V0` would answer for the last variant. The
programs are generated because 65537 variants cannot live in the tree.
"""
import argparse
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_lir_sum_types as harness  # noqa: E402

CASES = {257: "two-byte tag", 65537: "four-byte tag"}
EXPECTED = ("\n".join(("1", "2", "3")) + "\n").encode()


def program(count):
    lines = ["enum Big {"]
    lines.extend(f"    V{index}," for index in range(count))
    lines.extend(("}", ""))
    lines.extend(("fn pick(e: Big) -> i64 {", "    return match (e) {",
                  "        Big.V0 => 1,", f"        Big.V{count - 1} => 2,", "        _ => 3,", "    }", "}", ""))
    lines.extend(("print(pick(Big.V0))", f"print(pick(Big.V{count - 1}))", "print(pick(Big.V1))", ""))
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser()
    for name in ("vm-driver", "source-driver", "compiler", "include", "source-root", "work"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--variants", type=int, choices=tuple(CASES), required=True)
    parser.add_argument("--library", type=Path, nargs="+", required=True)
    parser.add_argument("--kind", choices=("msvc", "gnu"), required=True)
    parser.add_argument("--msvc-runtime", default="MultiThreaded")
    parser.add_argument("--asan", action="store_true")
    parser.add_argument("--command-timeout", type=int, default=120,
                        help="seconds allowed for one compiler, link or run step")
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    count = args.variants
    source = (args.work / f"sum-tag-{count}.xr").resolve()
    source.write_text(program(count), encoding="utf-8", newline="\n")
    harness.run_case(args, source, EXPECTED, CASES[count])


if __name__ == "__main__":
    main()
