#!/usr/bin/env python3
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path


def write(path: Path, text: str) -> Path:
    path.write_text(text)
    return path


def expect_fail(xisagen: Path, args: list[str], expected: str) -> None:
    proc = subprocess.run(
        [sys.executable, str(xisagen), *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if proc.returncode == 0:
        raise AssertionError(f"expected failure for {' '.join(args)}")
    if "xisagen: error:" not in proc.stderr:
        raise AssertionError(f"missing xisagen error prefix for {' '.join(args)}: {proc.stderr}")
    if expected not in proc.stderr:
        raise AssertionError(
            f"missing expected diagnostic {expected!r} for {' '.join(args)}: {proc.stderr}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--xisagen", required=True, type=Path)
    ns = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="xray_xisagen_cli_") as tmp_name:
        tmp = Path(tmp_name)
        bad_ops = write(tmp / "bad_ops.def", "NOT_A_XM_DEF(ADD)\n")
        bad_helpers = write(
            tmp / "bad_helpers.def",
            "XM_HELPER(bad_helper, 0, BADREP, NONE, NONE, NONE)\n",
        )
        # Helper that declares it can THROW but forgets to ask the call site
        # to apply the THROW post-call protocol. Must fail.
        helpers_throw_no_post = write(
            tmp / "throw_no_post.def",
            "XM_HELPER(broken_throw, 0, TAGGED, THROW, NONE, NONE)\n",
        )
        bad_isa = write(
            tmp / "bad.isa",
            "(define-mcinsn x64.bad\n"
            "  :operands (($dst reg:gpr64))\n"
            "  :encoding (rex.w 0x90)\n"
            "  :flags ()\n"
            "  :max-bytes 1)\n",
        )
        # Schema-completeness rejection paths: each fixture is otherwise
        # well-formed but omits or corrupts one required schema field.
        isa_no_golden_asm = write(
            tmp / "no_golden_asm.isa",
            "(define-mcinsn x64.no_asm\n"
            "  :operands () :constraints () :flags ()\n"
            "  :min-bytes 1 :max-bytes 1 :encoding (0x90)\n"
            "  :golden-bytes ((() \"90\"))\n"
            "  :external-reference llvm-mc)\n",
        )
        isa_no_external = write(
            tmp / "no_external.isa",
            "(define-mcinsn x64.no_ext\n"
            "  :operands () :constraints () :flags ()\n"
            "  :min-bytes 1 :max-bytes 1 :encoding (0x90)\n"
            "  :golden-bytes ((() \"90\"))\n"
            "  :golden-asm ((() \"nop\")))\n",
        )
        isa_bad_external = write(
            tmp / "bad_external.isa",
            "(define-mcinsn x64.bad_ext\n"
            "  :operands () :constraints () :flags ()\n"
            "  :min-bytes 1 :max-bytes 1 :encoding (0x90)\n"
            "  :golden-bytes ((() \"90\"))\n"
            "  :golden-asm ((() \"nop\"))\n"
            "  :external-reference some-blog-post)\n",
        )
        ops = write(tmp / "ops.def", "XM_DEF(ADD, ARITH, 2, I64, NONE)\n")
        bad_isel = write(tmp / "bad_isel.def", "ISEL(BOGUS, x64, GP_RR, x64.add.rr)\n")
        # An isel entry that names a real op but a bogus mcinsn that is not
        # in the supplied .isa fixture. Triggers the new arch-xref check.
        good_isel_bad_mcinsn = write(
            tmp / "good_isel_bad_mcinsn.def",
            "ISEL(ADD, x64, GP_RR_COMM, x64.does_not_exist)\n",
        )
        tiny_isa = write(
            tmp / "tiny.isa",
            "(define-mcinsn x64.add.rr\n"
            "  :operands () :constraints () :flags ()\n"
            "  :min-bytes 3 :max-bytes 3 :encoding (rex.w 0x01 0xc0)\n"
            "  :golden-bytes ((() \"48 01 c0\"))\n"
            "  :golden-asm ((() \"add rax, rax\"))\n"
            "  :external-reference llvm-mc)\n",
        )

        expect_fail(ns.xisagen, ["ops", str(bad_ops), str(tmp / "ops.h")], "no ops parsed")
        expect_fail(
            ns.xisagen,
            ["helpers", str(bad_helpers), str(tmp / "helpers.h")],
            "invalid return rep",
        )
        expect_fail(
            ns.xisagen,
            ["helpers", str(helpers_throw_no_post), str(tmp / "helpers.h")],
            "flag THROW requires post_call",
        )
        expect_fail(ns.xisagen, ["isa", str(bad_isa)], "missing :min-bytes")
        expect_fail(
            ns.xisagen,
            ["isa", str(isa_no_golden_asm)],
            ":golden-asm is required",
        )
        expect_fail(
            ns.xisagen,
            ["isa", str(isa_no_external)],
            ":external-reference is required",
        )
        expect_fail(
            ns.xisagen,
            ["isa", str(isa_bad_external)],
            "documented set",
        )
        expect_fail(ns.xisagen, ["isel", str(bad_isel), str(ops)], "unknown op")
        expect_fail(
            ns.xisagen,
            ["isel", str(good_isel_bad_mcinsn), str(ops),
             "x64=" + str(tiny_isa)],
            "is not defined in xisa/arch/x64.isa",
        )
        expect_fail(
            ns.xisagen,
            [
                "patch-ranges",
                str(tmp / "patch_ranges.h"),
                "x64=" + str(tiny_isa),
                "arm64=" + str(tiny_isa),
                "riscv64=" + str(tiny_isa),
            ],
            "x64.call.rel32 is not defined",
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
