"""Execute the registered validation tiers without dropping or skipping tests."""

import argparse
from collections import Counter
from copy import deepcopy
from fnmatch import fnmatchcase
import json
import os
from pathlib import Path
import shutil
import subprocess
import xml.etree.ElementTree as ET

from xray_process import run_child

TIER_BASELINE = ("6d8e663ac084ba57ccaa39d89673a80216295843", "scripts/l1-v0.json")


def command(arguments, directory, environment, log):
    actual = [str(a) for a in arguments]
    if os.name == "nt" and Path(actual[0]).stem.lower() in {"cmake", "ctest"}:
        batch = log.parent / (log.stem + "-" + str(len(list(log.parent.glob(log.stem + "-*.cmd")))) + ".cmd")
        batch.write_text("@echo off\nchcp 65001 >nul\n" + subprocess.list2cmdline(actual) +
                         "\nexit /b %errorlevel%\n", encoding="utf-8")
        actual = ["cmd.exe", "/d", "/c", str(batch)]
    with log.open("a", encoding="utf-8") as output:
        output.write("\n$ " + json.dumps([str(a) for a in arguments]) + "\n")
        output.flush()
        result = run_child(actual, cwd=directory, env=environment,
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
        required = {"smoke", "test_cli_usability", "test_cli_build_options", "test_cli_resource_profiles_vm",
                    "test_compile_request_memory", "test_compile_request_deadline",
                    "meta_ownership_inventory", "skeleton_diff"}
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


def build_targets(build: Path, targets: list[str], logs: Path, environment: dict, jobs: int = 8) -> None:
    command(["cmake", "--build", build, "--target", *targets, "-j" + str(jobs)], build, environment,
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


def tier_catalog(source: Path) -> dict:
    """Keep every original test and build target assigned to an executable tier."""
    config = json.loads((source / "scripts/validation-tiers.json").read_text(encoding="utf-8"))
    expected = {"L0": "scripts/l0-related.json", "L1": "scripts/l1-v0.json",
                "L2": "scripts/l2-capability.json", "L3": "scripts/l3-nightly.json"}
    if config.get("schema") != 1 or config.get("levels") != expected:
        raise ValueError("Invalid validation tier registry")
    if (config.get("baselineCommit"), config.get("baselineManifest")) != TIER_BASELINE:
        raise ValueError("The original tier baseline cannot be replaced")
    catalog, homes, targets = {}, {}, set()
    for level, file in expected.items():
        data = json.loads((source / file).read_text(encoding="utf-8"))
        if level == "L1":
            manifest((source / file).read_bytes())
            rows = {"integration": data}
        else:
            if data.get("schema") != 1 or data.get("tier") != level or not data.get("groups"):
                raise ValueError("Invalid tier groups: " + level)
            rows = data["groups"]
        for name, row in rows.items():
            if name in catalog:
                raise ValueError("Duplicate validation group: " + name)
            for key in ("tests", "targets"):
                values = row.get(key)
                if (not isinstance(values, list) or len(values) != len(set(values)) or
                        any(not isinstance(x, str) or not x or "\n" in x or "\r" in x for x in values)):
                    raise ValueError("Invalid " + name + " " + key)
            if not row["tests"]:
                raise ValueError("Empty validation group: " + name)
            for test in row["tests"]:
                if test in homes:
                    raise ValueError("Test has multiple home tiers: " + test)
                homes[test] = level
            targets.update(row["targets"])
            catalog[name] = dict(row, tier=level)
    baseline = json.loads(subprocess.check_output([
        "git", "-C", str(source), "show",
        config["baselineCommit"] + ":" + config["baselineManifest"]]))
    if not set(baseline["tests"]) <= homes.keys() or not set(baseline["targets"]) <= targets:
        raise ValueError("An original validation test or build target lost its tier")
    l2 = json.loads((source / expected["L2"]).read_text(encoding="utf-8"))
    for row in l2["requiredOn"].values():
        if (not set(row["tests"]) <= homes.keys() or not set(row["targets"]) <= targets or
                not set(row.get("groups", [])) <= catalog.keys()):
            raise ValueError("Unknown API/owner-sensitive consumer")
    return {"groups": catalog, "homes": homes, "targets": targets, "l2": l2}


def tier_plan(source: Path, level: str, capabilities=(), changed=(), sensitive=()) -> dict:
    """Select explicit families, or affected tooling/capabilities, from the same lists."""
    if level not in {"L0", "L1", "L2", "L3"}:
        raise ValueError("Unknown validation tier")
    catalog = tier_catalog(source)
    groups = catalog["groups"]
    names = list(dict.fromkeys(capabilities))
    if any(name not in groups or name == "integration" for name in names):
        raise ValueError("Unknown capability group")
    if level == "L2" and any(groups[name]["tier"] not in {"L2", "L3"} for name in names):
        raise ValueError("Tooling self-tests cannot replace an L2 capability batch")
    paths = [str(p).replace("\\", "/") for p in changed]
    if any(p.startswith("/") or ":" in p or ".." in p.split("/") for p in paths):
        raise ValueError("Affected paths must be repository relative")
    if level == "L1":
        if names or sensitive:
            raise ValueError("L1 selection is fixed; use the merge queue for execution")
        names = ["integration"]
    elif level == "L3" and not names:
        names = list(groups)
    elif not names:
        names = [name for name, row in groups.items() if row["tier"] == level and
                 any(fnmatchcase(path, pattern) for path in paths for pattern in row["match"])]
    if not names:
        raise ValueError("No affected validation group; select an explicit capability")
    selected_tests = list(dict.fromkeys(test for name in names for test in groups[name]["tests"]))
    selected_targets = list(dict.fromkeys(target for name in names for target in groups[name]["targets"]))
    sensitivity = set(sensitive)
    if sensitivity - {"api", "owner"}:
        raise ValueError("Unknown interface sensitivity")
    if level == "L2":
        for kind, patterns in catalog["l2"]["interfaceMatches"].items():
            if any(fnmatchcase(path, pattern) for path in paths for pattern in patterns):
                sensitivity.add(kind)
        for kind in sorted(sensitivity):
            row = catalog["l2"]["requiredOn"][kind]
            for name in row.get("groups", []):
                if name not in names:
                    names.append(name)
                    selected_tests = list(dict.fromkeys(selected_tests + groups[name]["tests"]))
                    selected_targets = list(dict.fromkeys(selected_targets + groups[name]["targets"]))
            selected_tests = list(dict.fromkeys(selected_tests + row["tests"]))
            selected_targets = list(dict.fromkeys(selected_targets + row["targets"]))
    if level != "L0" or selected_targets:
        selected_targets = list(dict.fromkeys(["xray"] + selected_targets))
    return dict(schema=1, tier=level, groups=names, sensitivity=sorted(sensitivity),
                tests=selected_tests, targets=selected_targets,
                versionCommand=["{build}/xray.exe" if os.name == "nt" else "{build}/xray", "--version", "--json"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tier", choices=("L0", "L1", "L2", "L3"), required=True)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--build", type=Path)
    parser.add_argument("--logs", type=Path)
    parser.add_argument("--base", help="Actual source comparison base for affected capability selection")
    parser.add_argument("--capability", action="append", default=[])
    parser.add_argument("--changed", action="append", default=[])
    parser.add_argument("--sensitive", choices=("api", "owner"), action="append", default=[])
    parser.add_argument("--plan-only", action="store_true")
    parser.add_argument("--slots", type=Path, default=Path("D:/xray-build/slots"))
    args = parser.parse_args()
    source = args.root.resolve()
    changed = list(args.changed)
    if args.base:
        changed += subprocess.check_output(["git", "-C", str(source), "diff", "--name-only", args.base, "HEAD"], text=True).splitlines()
    plan = tier_plan(source, args.tier, args.capability, changed, args.sensitive)
    if args.plan_only:
        print(json.dumps(plan, indent=2))
        return 0
    if args.tier == "L1":
        parser.error("Execute L1 once through merge_slice.py, not a second local runner")
    if args.build is None or args.logs is None or (args.tier != "L3" and not args.base):
        parser.error("execution requires --build, a new --logs directory, and --base for L0/L2")
    logs, build = args.logs.resolve(), args.build.resolve()
    logs.mkdir(parents=True, exist_ok=False)
    result = dict(state="RUNNING", plan=plan, base=args.base, changed=sorted(set(changed)))
    def save():
        (logs / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    save()
    from xray_slot import Lease
    from xray_process import RecoveryRequired
    try:
        if subprocess.check_output(["git", "-C", str(source), "status", "--porcelain"]).strip():
            raise RuntimeError("Validation execution requires a clean source tree")
        head = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
        result["sourceSHA"] = head
        environment = msvc_environment(logs / "env")
        environment.update(XR_BUILD_JOBS="2", PYTHONDONTWRITEBYTECODE="1")
        with Lease(args.slots, "heavy", priority=1, payload={"tier": args.tier, "head": head}) as heavy:
            result["heavyLease"] = str(heavy.path)
            with Lease(args.slots, "build", priority=1, payload={"tier": args.tier, "jobs": 2}) as lease:
                result["buildLease"] = str(lease.path)
                save()
                configure(source, build, logs, environment)
                if plan["targets"]:
                    build_targets(build, plan["targets"], logs, environment, jobs=2)
                    result["identity"] = identity(plan, source, build, head, logs, environment)
                else:
                    result["identity"] = "NOT_APPLICABLE: tooling-only selection has no compiled consumer"
                tests(plan["tests"], build, logs, args.tier.lower(), environment)
        if subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip() != head or subprocess.check_output(["git", "-C", str(source), "status", "--porcelain"]).strip():
            raise RuntimeError("Source changed during validation")
        result["state"] = "PASS"
    except BaseException as error:
        result.update(state="NEEDS_RECOVERY" if isinstance(error, RecoveryRequired) else "FAIL", error=repr(error))
        raise
    finally:
        save()
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
