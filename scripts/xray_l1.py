"""Build and run an explicit L1 manifest, rejecting absent or skipped tests."""

from collections import Counter
from copy import deepcopy
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
    if "progressPolicy" in value:
        required = {"smoke", "smoke_runner", "test_cli_usability", "usability_progress_runner"}
        if value["progressPolicy"] != "original-smoke-and-deletion-only-legacy" or not required <= set(value["tests"]):
            raise ValueError("L1 usability policy requires complete smoke and real CLI behavior tests")
    return value


def tests(names: list[str], build: Path, logs: Path, label: str, environment: dict) -> None:
    if not test_groups({label: names}, build, logs, environment)[label]:
        raise RuntimeError(f"{label}: failed, skipped or unexecuted test")


def test_groups(groups: dict[str, list[str]], build: Path, logs: Path,
                environment: dict) -> dict[str, bool]:
    """Execute the union once, preserving each group's exact report and verdict."""
    if not groups or any(not names or len(names) != len(set(names)) for names in groups.values()):
        raise ValueError("Test groups must be nonempty and contain unique names")
    label = next(iter(groups))
    stem = label + "-combined" if len(groups) > 1 else label
    names = list(dict.fromkeys(name for group in groups.values() for name in group))
    listing, report = logs / (stem + ".tests.txt"), logs / (stem + ".xml")
    for group, members in groups.items():
        (logs / (group + ".tests.txt")).write_text("\n".join(members) + "\n", encoding="utf-8")
        (logs / (group + ".xml")).unlink(missing_ok=True)
    listing.write_text("\n".join(names) + "\n", encoding="utf-8")
    if report.exists():
        report.unlink()
    failed_command = False
    try:
        command(["ctest", "--test-dir", build, "--tests-from-file", listing, "--no-tests=error",
                 "--output-on-failure", "--output-junit", report, "-j1"], build, environment,
                logs / (label + ".log"))
    except subprocess.CalledProcessError:
        failed_command = True
    for group in groups:
        if group != label:
            (logs / (group + ".log")).write_bytes((logs / (label + ".log")).read_bytes())
    root = ET.parse(report).getroot()
    cases = list(root.iter("testcase"))
    if Counter(case.get("name") for case in cases) != Counter(names):
        raise RuntimeError(f"{label}: requested tests differ from executed tests")
    def passed(case):
        return case.get("status") == "run" and all(
            case.find(tag) is None for tag in ("failure", "skipped", "error"))
    results = {}
    for group, members in groups.items():
        selected = [case for case in cases if case.get("name") in members]
        results[group] = all(passed(case) for case in selected)
        if len(groups) > 1:
            suite = ET.Element("testsuite", name=group, tests=str(len(selected)),
                               failures=str(sum(case.find("failure") is not None for case in selected)),
                               errors=str(sum(case.find("error") is not None for case in selected)),
                               skipped=str(sum(case.find("skipped") is not None for case in selected)),
                               time=str(sum(float(case.get("time", "0")) for case in selected)))
            for case in selected:
                suite.append(deepcopy(case))
            ET.ElementTree(suite).write(logs / (group + ".xml"), encoding="utf-8",
                                       xml_declaration=True)
    if failed_command and all(results.values()):
        raise RuntimeError(f"{label}: CTest failed despite passing test reports")
    return results


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
