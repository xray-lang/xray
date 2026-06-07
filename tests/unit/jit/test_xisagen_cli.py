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
        xi_ops = write(
            tmp / "xi_ops.def",
            "(define-xi-op xi.add\n"
            "  :class arithmetic\n"
            "  :arity 2\n"
            "  :effects ()\n"
            "  :requires ()\n"
            "  :observable ()\n"
            "  :targets (vm-bytecode jit-xm aot-c)\n"
            "  :jit-policy required)\n",
        )
        bad_xi_lowering = write(
            tmp / "bad_xi_lowering.def",
            "(lower xi.add\n"
            "  :required-targets (vm-bytecode jit-xm)\n"
            "  :vm-bytecode (driver xi_emit_add))\n",
        )
        good_xi_lowering = write(
            tmp / "good_xi_lowering.def",
            "(lower xi.add\n"
            "  :required-targets (vm-bytecode jit-xm aot-c)\n"
            "  :vm-bytecode (driver xi_emit_arith)\n"
            "  :jit-xm (driver xi2xm_add)\n"
            "  :aot-c (driver xicgen_add))\n",
        )
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
            ["xi-lowering", str(xi_ops), str(bad_xi_lowering), str(tmp)],
            "missing required target",
        )
        out_root = tmp / "xi_lowering_out"
        proc = subprocess.run(
            [sys.executable, str(ns.xisagen), "xi-lowering", str(xi_ops),
             str(good_xi_lowering), str(out_root)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if proc.returncode != 0:
            raise AssertionError(f"xi-lowering success path failed: {proc.stderr}")
        expected_outputs = [
            out_root / "src/ir/xi_lowering_coverage_gen.h",
            out_root / "src/ir/xi_emit_vm_gen.h",
            out_root / "src/jit/xi_to_xm_dispatch_gen.h",
            out_root / "src/aot/xi_to_c_dispatch_gen.h",
        ]
        missing_outputs = [str(path) for path in expected_outputs if not path.exists()]
        if missing_outputs:
            raise AssertionError(f"xi-lowering did not generate: {missing_outputs}")
        vm_dispatch = (out_root / "src/ir/xi_emit_vm_gen.h").read_text()
        if "X(ADD, xi_emit_arith)" not in vm_dispatch:
            raise AssertionError(f"missing VM lowering driver entry: {vm_dispatch}")
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
