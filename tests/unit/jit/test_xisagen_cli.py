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
        xi_ops_missing_lowering = write(
            tmp / "xi_ops_missing_lowering.def",
            "(define-xi-op xi.add\n"
            "  :class arithmetic\n"
            "  :arity 2\n"
            "  :effects ()\n"
            "  :requires ()\n"
            "  :observable ()\n"
            "  :targets (vm-bytecode jit-xm aot-c)\n"
            "  :jit-policy required)\n"
            "(define-xi-op xi.sub\n"
            "  :class arithmetic\n"
            "  :arity 2\n"
            "  :effects ()\n"
            "  :requires ()\n"
            "  :observable ()\n"
            "  :targets (vm-bytecode jit-xm aot-c)\n"
            "  :jit-policy required)\n",
        )
        xi_ops_special_policy = write(
            tmp / "xi_ops_special_policy.def",
            "(define-xi-op xi.add\n"
            "  :class arithmetic\n"
            "  :arity 2\n"
            "  :effects ()\n"
            "  :requires ()\n"
            "  :observable ()\n"
            "  :targets (vm-bytecode jit-xm aot-c)\n"
            "  :jit-policy required)\n"
            "(define-xi-op xi.phi\n"
            "  :class pure\n"
            "  :arity variadic\n"
            "  :effects ()\n"
            "  :requires ()\n"
            "  :observable ()\n"
            "  :targets (vm-bytecode jit-xm aot-c)\n"
            "  :jit-policy none\n"
            "  :lowering-policy special)\n",
        )
        xi_ops_target_mismatch = write(
            tmp / "xi_ops_target_mismatch.def",
            "(define-xi-op xi.add\n"
            "  :class arithmetic\n"
            "  :arity 2\n"
            "  :effects ()\n"
            "  :requires ()\n"
            "  :observable ()\n"
            "  :targets (vm-bytecode aot-c)\n"
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
        good_xi_verifier = write(
            tmp / "good_xi_verifier.def",
            "(verify-xi-op xi.add\n"
            "  :checks (bool-result))\n",
        )
        bad_xi_verifier = write(
            tmp / "bad_xi_verifier.def",
            "(verify-xi-op xi.add\n"
            "  :checks (shape-shift))\n",
        )
        bad_aot_rep = write(
            tmp / "bad_aot_rep.def",
            "(define-aot-rep i8\n"
            "  :c-type \"int8_t\"\n"
            "  :size 1\n"
            "  :align 1\n"
            "  :signed yes\n"
            "  :integer yes\n"
            "  :boxed no\n"
            "  :dynamic-kind scalar\n"
            "  :native-type native_i8\n"
            "  :storage-rep XR_REP_I64)\n",
        )
        good_aot_rep = write(
            tmp / "good_aot_rep.def",
            "(define-aot-rep i8\n"
            "  :c-type \"int8_t\"\n"
            "  :size 1\n"
            "  :align 1\n"
            "  :signed yes\n"
            "  :integer yes\n"
            "  :boxed no\n"
            "  :dynamic-kind scalar\n"
            "  :native-type XR_NATIVE_I8\n"
            "  :storage-rep XR_REP_I64)\n"
            "(define-aot-rep i64\n"
            "  :c-type \"int64_t\"\n"
            "  :size 8\n"
            "  :align 8\n"
            "  :signed yes\n"
            "  :integer yes\n"
            "  :boxed no\n"
            "  :dynamic-kind scalar\n"
            "  :native-type XR_NATIVE_I64\n"
            "  :storage-rep XR_REP_I64)\n"
            "(define-aot-rep tagged\n"
            "  :c-type \"XrValue\"\n"
            "  :size 16\n"
            "  :align 8\n"
            "  :signed no\n"
            "  :integer no\n"
            "  :boxed yes\n"
            "  :dynamic-kind tagged\n"
            "  :native-type none\n"
            "  :storage-rep XR_REP_TAGGED)\n",
        )
        bad_aot_abi = write(
            tmp / "bad_aot_abi.def",
            "(define-aot-abi int\n"
            "  :type-kind XR_KIND_INT\n"
            "  :nullable no\n"
            "  :abi-class scalar\n"
            "  :default-rep missing\n"
            "  :native-width yes\n"
            "  :typed-boundary yes)\n",
        )
        good_aot_abi = write(
            tmp / "good_aot_abi.def",
            "(define-aot-abi int\n"
            "  :type-kind XR_KIND_INT\n"
            "  :nullable no\n"
            "  :abi-class scalar\n"
            "  :default-rep i64\n"
            "  :native-width yes\n"
            "  :typed-boundary yes)\n"
            "(define-aot-abi array\n"
            "  :type-kind XR_KIND_ARRAY\n"
            "  :nullable no\n"
            "  :abi-class tagged\n"
            "  :default-rep tagged\n"
            "  :native-width no\n"
            "  :typed-boundary no)\n",
        )
        bad_aot_layout = write(
            tmp / "bad_aot_layout.def",
            "(define-aot-layout i8\n"
            "  :native-type XR_NATIVE_I8\n"
            "  :field-kind scalar\n"
            "  :rep missing\n"
            "  :c-type \"int8_t\"\n"
            "  :heap-field yes\n"
            "  :ref-tag none)\n",
        )
        good_aot_layout = write(
            tmp / "good_aot_layout.def",
            "(define-aot-layout i8\n"
            "  :native-type XR_NATIVE_I8\n"
            "  :field-kind scalar\n"
            "  :rep i8\n"
            "  :c-type \"int8_t\"\n"
            "  :heap-field yes\n"
            "  :ref-tag none)\n"
            "(define-aot-layout array-ref\n"
            "  :native-type XR_NATIVE_ARRAY_REF\n"
            "  :field-kind pointer-ref\n"
            "  :rep tagged\n"
            "  :c-type \"xrt_array_t *\"\n"
            "  :heap-field yes\n"
            "  :ref-tag XR_TAG_ARRAY)\n",
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
        expect_fail(
            ns.xisagen,
            ["xi-lowering", str(xi_ops_missing_lowering), str(good_xi_lowering), str(tmp)],
            "missing lowering entry for generated Xi op(s): xi.sub",
        )
        expect_fail(
            ns.xisagen,
            ["xi-lowering", str(xi_ops_target_mismatch), str(good_xi_lowering), str(tmp)],
            "target mismatch with ops.def",
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
            out_root / "src/aot/xi_to_c_stmt_dispatch_gen.h",
            out_root / "tests/unit/ir/test_xi_lowering_gen.c",
        ]
        missing_outputs = [str(path) for path in expected_outputs if not path.exists()]
        if missing_outputs:
            raise AssertionError(f"xi-lowering did not generate: {missing_outputs}")
        vm_dispatch = (out_root / "src/ir/xi_emit_vm_gen.h").read_text()
        if "X(ADD, xi_emit_arith)" not in vm_dispatch:
            raise AssertionError(f"missing VM lowering driver entry: {vm_dispatch}")
        expect_fail(
            ns.xisagen,
            ["xi-verify", str(xi_ops), str(bad_xi_verifier), str(tmp / "xi_verify_gen.h")],
            "unknown check",
        )
        proc = subprocess.run(
            [sys.executable, str(ns.xisagen), "xi-verify", str(xi_ops),
             str(good_xi_verifier), str(tmp / "xi_verify_gen.h")],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if proc.returncode != 0:
            raise AssertionError(f"xi-verify success path failed: {proc.stderr}")
        xi_verify_header = (tmp / "xi_verify_gen.h").read_text()
        if "XI_VERIFY_CHECK_BOOL_RESULT" not in xi_verify_header:
            raise AssertionError(f"missing Xi verifier check flag: {xi_verify_header}")
        special_out_root = tmp / "xi_lowering_special_out"
        proc = subprocess.run(
            [sys.executable, str(ns.xisagen), "xi-lowering", str(xi_ops_special_policy),
             str(good_xi_lowering), str(special_out_root)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if proc.returncode != 0:
            raise AssertionError(f"xi-lowering special-policy path failed: {proc.stderr}")
        expect_fail(
            ns.xisagen,
            ["aot-rep", str(bad_aot_rep), str(tmp / "xaot_rep_gen.h")],
            ":native-type must be XR_NATIVE_* or none",
        )
        proc = subprocess.run(
            [sys.executable, str(ns.xisagen), "aot-rep", str(good_aot_rep),
             str(tmp / "xaot_rep_gen.h")],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if proc.returncode != 0:
            raise AssertionError(f"aot-rep success path failed: {proc.stderr}")
        aot_rep_header = (tmp / "xaot_rep_gen.h").read_text()
        if "XAOT_REP_I8" not in aot_rep_header:
            raise AssertionError(f"missing AOT rep enum entry: {aot_rep_header}")
        if "xaot_c_type_for_native_int_type" not in aot_rep_header:
            raise AssertionError(f"missing AOT native int query: {aot_rep_header}")
        if "xaot_elem_name_for_native_type" not in aot_rep_header:
            raise AssertionError(f"missing AOT native element query: {aot_rep_header}")
        expect_fail(
            ns.xisagen,
            ["aot-abi", str(good_aot_rep), str(bad_aot_abi), str(tmp / "xaot_abi_gen.h")],
            "unknown :default-rep",
        )
        proc = subprocess.run(
            [sys.executable, str(ns.xisagen), "aot-abi", str(good_aot_rep),
             str(good_aot_abi), str(tmp / "xaot_abi_gen.h")],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if proc.returncode != 0:
            raise AssertionError(f"aot-abi success path failed: {proc.stderr}")
        aot_abi_header = (tmp / "xaot_abi_gen.h").read_text()
        if "xaot_abi_type_can_use_typed_boundary" not in aot_abi_header:
            raise AssertionError(f"missing AOT ABI typed-boundary query: {aot_abi_header}")
        if "XAOT_ABI_CLASS_SCALAR" not in aot_abi_header:
            raise AssertionError(f"missing AOT ABI class enum: {aot_abi_header}")
        expect_fail(
            ns.xisagen,
            [
                "aot-layout", str(good_aot_rep), str(bad_aot_layout),
                str(tmp / "xaot_layout_gen.h"),
            ],
            "unknown :rep",
        )
        proc = subprocess.run(
            [sys.executable, str(ns.xisagen), "aot-layout", str(good_aot_rep),
             str(good_aot_layout), str(tmp / "xaot_layout_gen.h")],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if proc.returncode != 0:
            raise AssertionError(f"aot-layout success path failed: {proc.stderr}")
        aot_layout_header = (tmp / "xaot_layout_gen.h").read_text()
        if "xaot_layout_ref_tag_name_for_native_type" not in aot_layout_header:
            raise AssertionError(f"missing AOT layout ref-tag query: {aot_layout_header}")
        if "XR_TAG_ARRAY" not in aot_layout_header:
            raise AssertionError(f"missing AOT layout tag metadata: {aot_layout_header}")
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
