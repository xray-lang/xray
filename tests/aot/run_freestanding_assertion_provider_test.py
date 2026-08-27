#!/usr/bin/env python3
"""Generate, compile, link, and run the canonical assertion hook profile."""

from __future__ import annotations

import argparse
import os
import shutil
import sys
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, toolchain, workspace  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent
CASE_DIR = SCRIPT_DIR / "filetests" / "link"
SOURCE = CASE_DIR / "freestanding_assert_hook.xr"
MANIFEST = CASE_DIR / "freestanding_assert_hook.toml"
HOOKS = SCRIPT_DIR / "provider" / "freestanding_assertion" / "hooks.c"
REJECT_SOURCE = (SCRIPT_DIR / "provider" / "freestanding_assertion" /
                 "rejected.xr")
REJECTING_HOOKS = (SCRIPT_DIR / "provider" / "freestanding_assertion" /
                   "rejecting_hooks.c")
BUILTIN_PROVIDER_ID = "xray-freestanding-hooks-v1"


def fail(detail: str, output: str = "") -> int:
    sys.stderr.write(detail + "\n")
    if output:
        sys.stderr.write(output[:12000])
    return 1


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("xray", nargs="?", default=None)
    args = parser.parse_args(argv[1:])
    xray = Path(args.xray or os.environ.get("XRAY_BIN") or
                (PROJECT_DIR / "build" / "xray"))
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300)
    compiler = (toolchain.find_c_compiler() or
                toolchain.find_c_syntax_compiler())
    if compiler is None:
        print("SKIP: no C compiler found")
        return 77

    with workspace.Workspace("xray-freestanding-assertion-provider") as ws:
        project = ws.subdir("project")
        shutil.copy2(SOURCE, project / SOURCE.name)
        shutil.copy2(MANIFEST, project / "xray.toml")
        generated = ws.path("assertion.c")
        generation = proc.run(
            [xray, "build", "--native", "--target", "native",
             "--profile", "freestanding", "--dump-xaot-plan",
             "--dump-link-manifest", "-c", "-o", generated,
             project / SOURCE.name],
            cwd=project,
            timeout=timeout,
        )
        if not generation.ok:
            return fail("freestanding assertion C generation failed",
                        generation.combined_text())
        generated_text = generated.read_text(encoding="utf-8")
        required = (
            "xrt_freestanding_assertion_condition(",
            "xrt_freestanding_assertion_equal(",
            "#include \"xrt_core_freestanding.h\"",
        )
        for spelling in required:
            if spelling not in generated_text:
                return fail(f"generated C lacks exact assertion adapter: {spelling}")
        forbidden = ("xrt_assertion_condition(", "xrt_assertion_equal(",
                     "xrt_assert_condition_failed(")
        for spelling in forbidden:
            if spelling in generated_text:
                return fail(f"generated C retained hosted/retired adapter: {spelling}")

        executable = ws.path("assertion-provider" + platform.EXE_SUFFIX)
        include_dirs = (PROJECT_DIR / "src" / "aot", PROJECT_DIR / "src",
                        PROJECT_DIR / "include")
        if compiler.driver == toolchain.CC_DRIVER_MSVC:
            compile_argv = [compiler.path, "/nologo", "/TC", "/std:c11",
                            "/experimental:c11atomics", "/utf-8", "/GS-"]
            compile_argv.extend(f"/I{path}" for path in include_dirs)
            compile_argv.extend([str(generated), str(HOOKS), f"/Fe:{executable}"])
        else:
            compile_argv = [compiler.path]
            if compiler.driver == toolchain.CC_DRIVER_ZIG:
                compile_argv.append("cc")
            compile_argv.extend(["-std=c11", "-ffreestanding",
                                 "-fno-stack-protector"])
            compile_argv.extend(f"-I{path}" for path in include_dirs)
            compile_argv.extend([str(generated), str(HOOKS), "-o",
                                 str(executable)])
        compile_link = proc.run(
            compile_argv,
            cwd=generated.parent,
            timeout=timeout,
        )
        if not compile_link.ok:
            return fail("freestanding assertion generated C did not compile/link",
                        compile_link.combined_text())
        execution = proc.run([executable], timeout=timeout)
        if not execution.ok:
            return fail("freestanding assertion provider executable failed",
                        execution.combined_text())

        # The compiler burns the resolved source path into the generated C, so
        # the expected failure bytes below must be built from the same spelling.
        # A temporary directory reaches us through a symlinked prefix on macOS.
        rejecting = ws.subdir("provider-rejects-report").resolve()
        rejecting_source = rejecting / REJECT_SOURCE.name
        rejecting_manifest = rejecting / "xray.toml"
        shutil.copy2(REJECT_SOURCE, rejecting_source)
        rejecting_manifest.write_text(
            MANIFEST.read_text(encoding="utf-8").replace(
                SOURCE.name, REJECT_SOURCE.name),
            encoding="utf-8",
        )
        rejecting_generated = ws.path("rejecting-assertion.c")
        generation = proc.run(
            [xray, "build", "--native", "--target", "native",
             "--profile", "freestanding", "-c", "-o",
             rejecting_generated, rejecting_source],
            cwd=rejecting,
            timeout=timeout,
        )
        if not generation.ok:
            return fail("rejecting freestanding assertion C generation failed",
                        generation.combined_text())
        rejecting_executable = ws.path(
            "rejecting-assertion-provider" + platform.EXE_SUFFIX)
        if compiler.driver == toolchain.CC_DRIVER_MSVC:
            rejecting_compile_argv = [
                compiler.path, "/nologo", "/TC", "/std:c11",
                "/experimental:c11atomics", "/utf-8", "/GS-",
            ]
            rejecting_compile_argv.extend(
                f"/I{path}" for path in include_dirs)
            rejecting_compile_argv.extend(
                [str(rejecting_generated), str(REJECTING_HOOKS),
                 f"/Fe:{rejecting_executable}"])
        else:
            rejecting_compile_argv = [compiler.path]
            if compiler.driver == toolchain.CC_DRIVER_ZIG:
                rejecting_compile_argv.append("cc")
            rejecting_compile_argv.extend(
                ["-std=c11", "-ffreestanding", "-fno-stack-protector"])
            rejecting_compile_argv.extend(
                f"-I{path}" for path in include_dirs)
            rejecting_compile_argv.extend(
                [str(rejecting_generated), str(REJECTING_HOOKS), "-o",
                 str(rejecting_executable)])
        rejecting_compile = proc.run(
            rejecting_compile_argv,
            cwd=rejecting_generated.parent,
            timeout=timeout,
        )
        if not rejecting_compile.ok:
            return fail("rejecting assertion provider did not compile/link",
                        rejecting_compile.combined_text())
        rejection = proc.run([rejecting_executable], timeout=timeout)
        line_break = "\r\n" if os.name == "nt" else "\n"
        expected_rejection = (
            "AssertionFailure[condition-false] at " +
            str(rejecting_source) +
            ":1:1" + line_break + "  message: provider rejection" +
            line_break +
            "TRAP:freestanding assertion provider rejected failure bytes"
        )
        if (rejection.timed_out or rejection.returncode != 73 or
                rejection.stdout_text(errors="replace") != expected_rejection or
                rejection.stderr):
            return fail("freestanding rejecting provider did not trap with "
                        "the exact failure bytes",
                        rejection.combined_text())

        standalone = ws.subdir("standalone-without-provider")
        standalone_source = standalone / SOURCE.name
        shutil.copy2(SOURCE, standalone_source)
        rejected_output = standalone / "assertion.c"
        rejected = proc.run(
            [xray, "build", "--native", "--target", "native",
             "--profile", "freestanding", "-c", "-o", rejected_output,
             standalone_source],
            cwd=standalone,
            timeout=timeout,
        )
        if rejected.ok:
            return fail("freestanding assertion without an explicit provider "
                        "was accepted")
        if rejected_output.exists():
            return fail("freestanding assertion emitted C before rejecting the "
                        "missing provider")
        if "canonical freestanding hook provider contract is invalid" not in \
                rejected.combined_text():
            return fail("freestanding assertion missing-provider diagnostic "
                        "was not exact", rejected.combined_text())

        wrong_identity = ws.subdir("wrong-provider-identity")
        wrong_source = wrong_identity / SOURCE.name
        wrong_manifest = wrong_identity / "xray.toml"
        shutil.copy2(SOURCE, wrong_source)
        manifest_text = MANIFEST.read_text(encoding="utf-8")
        if BUILTIN_PROVIDER_ID not in manifest_text:
            return fail("canonical fixture does not declare the frozen provider "
                        "identity")
        wrong_manifest.write_text(
            manifest_text.replace(BUILTIN_PROVIDER_ID,
                                  "unvalidated-provider-v1"),
            encoding="utf-8",
        )
        wrong_output = wrong_identity / "assertion.c"
        rejected = proc.run(
            [xray, "build", "--native", "--target", "native",
             "--profile", "freestanding", "-c", "-o", wrong_output,
             wrong_source],
            cwd=wrong_identity,
            timeout=timeout,
        )
        if rejected.ok or wrong_output.exists():
            return fail("freestanding assertion accepted an unvalidated provider "
                        "identity")
        expected = ("expected exact built-in identity '" +
                    BUILTIN_PROVIDER_ID + "'")
        if expected not in rejected.combined_text():
            return fail("freestanding assertion wrong-provider diagnostic was "
                        "not exact", rejected.combined_text())

    print("freestanding assertion provider generate/compile/link/run, "
          "exact rejection trap, and plan-time rejection checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
