"""Build and run an explicit L1 manifest, rejecting absent or skipped tests."""

from collections import Counter
import json
import os
from pathlib import Path
import shutil
import subprocess
import xml.etree.ElementTree as ET

from xray_process import run_child


def command(arguments, directory, environment, log):
    with log.open("a", encoding="utf-8") as output:
        output.write("\n$ " + json.dumps([str(a) for a in arguments]) + "\n")
        output.flush()
        result = run_child([str(a) for a in arguments], cwd=directory, env=environment,
                           stdout=output, stderr=subprocess.STDOUT)
        output.write(f"\nexitCode: {result}\n")
    if result:
        raise subprocess.CalledProcessError(result, arguments)


def msvc_environment(temporary: Path) -> dict:
    environment = os.environ.copy()
    temporary.mkdir(parents=True, exist_ok=True)
    environment.update(TEMP=str(temporary), TMP=str(temporary), VSLANG="1033", PYTHONUTF8="1")
    if os.name != "nt":
        return environment
    locator = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / (
        "Microsoft Visual Studio/Installer/vswhere.exe")
    install = subprocess.check_output([str(locator), "-latest", "-products", "*", "-requires",
                                       "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                       "-property", "installationPath"], text=True).strip()
    if not install:
        raise RuntimeError("MSVC Build Tools are required")
    setup = Path(install) / "Common7/Tools/VsDevCmd.bat"
    # Use a batch file so Python's Windows argv quoting cannot alter cmd syntax.
    batch = temporary / "msvc-environment.cmd"
    batch.write_text('@echo off\nchcp 65001 >nul\ncall "' + str(setup)
                     + '" -arch=x64 -host_arch=x64 >nul\nif errorlevel 1 exit /b %errorlevel%\nset\n',
                     encoding="utf-8")
    result = subprocess.run(["cmd.exe", "/d", "/c", str(batch)],
                            env=environment, capture_output=True, encoding="utf-8", errors="strict", check=True)
    for line in result.stdout.splitlines():
        name, separator, value = line.partition("=")
        if separator and name:
            environment[name] = value
    if not shutil.which("cl", path=environment.get("Path", environment.get("PATH"))):
        raise RuntimeError("MSVC environment did not provide cl.exe")
    return environment


def manifest(content: bytes) -> dict:
    value = json.loads(content.decode("utf-8"))
    if value.get("schema") != 1:
        raise ValueError("Unsupported L1 manifest schema")
    for key in ("targets", "tests", "versionCommand"):
        items = value.get(key)
        if not isinstance(items, list) or not items or any(
                not isinstance(item, str) or not item or "\n" in item or "\r" in item for item in items):
            raise ValueError(f"Invalid L1 {key}")
        if key != "versionCommand" and len(items) != len(set(items)):
            raise ValueError(f"Duplicate L1 {key}")
    return value


def tests(names: list[str], build: Path, logs: Path, label: str, environment: dict) -> None:
    listing = logs / (label + ".tests.txt")
    report = logs / (label + ".xml")
    listing.write_text("\n".join(names) + "\n", encoding="utf-8")
    if report.exists():
        report.unlink()
    command(["ctest", "--test-dir", build, "--tests-from-file", listing, "--no-tests=error",
             "--output-on-failure", "--output-junit", report, "-j1"], build, environment,
            logs / (label + ".log"))
    cases = list(ET.parse(report).getroot().iter("testcase"))
    if Counter(case.get("name") for case in cases) != Counter(names):
        raise RuntimeError(f"{label}: requested tests differ from executed tests")
    if any(case.get("status") != "run" or case.find("failure") is not None
           or case.find("skipped") is not None or case.find("error") is not None for case in cases):
        raise RuntimeError(f"{label}: failed, skipped or unexecuted test")


def configure(source: Path, build: Path, logs: Path, environment: dict) -> None:
    args = ["cmake", "-S", source, "-B", build, "-G", "Ninja", "-DBUILD_TESTS=ON",
            "-DCMAKE_BUILD_TYPE=Release"]
    if os.name == "nt":
        args.append("-DCMAKE_C_COMPILER=cl")
    command(args, source, environment, logs / "configure.log")


def build_targets(build: Path, targets: list[str], logs: Path, environment: dict) -> None:
    command(["cmake", "--build", build, "--target", *targets, "-j8"], build, environment,
            logs / "build.log")


def identity(plan: dict, source: Path, build: Path, expected: str, logs: Path, environment: dict) -> dict:
    args = [item.replace("{build}", str(build)).replace("{source}", str(source))
            for item in plan["versionCommand"]]
    output = logs / "identity.json"
    with output.open("w", encoding="utf-8") as capture:
        result = run_child(args, cwd=source, env=environment, stdout=capture, stderr=subprocess.STDOUT)
    if result:
        raise subprocess.CalledProcessError(result, args)
    value = json.loads(output.read_text(encoding="utf-8-sig"))
    if value.get("commit") != expected or value.get("dirty") is not False:
        raise RuntimeError("Built executable does not identify the clean candidate commit")
    return value
