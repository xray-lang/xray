"""Check source-backed usability state and deletion-only legacy directories."""

import argparse
from datetime import date
import io
import json
from pathlib import Path
import re
import subprocess


LEGACY = ("src/ir/", "src/plan/", "src/aot/", "src/analysis/")
TASK = "tasks/327-end-to-end-usability-first-reset-cn.md"


def git(root: Path, *arguments: str) -> bytes:
    return subprocess.check_output(["git", "-C", str(root), *arguments], stderr=subprocess.PIPE)


def legacy_deletions(root: Path, base: str, candidate: str = "HEAD") -> dict:
    """Reject additions, rewrites, binaries and moves; count tracked C/H lines."""
    raw = git(root, "diff", "--raw", "--no-renames", base, candidate,
              "--", *LEGACY).decode("utf-8").splitlines()
    deleted = set()
    for line in raw:
        metadata, path = line.split("\t", 1)
        old_mode, new_mode, _, _, status = metadata.split()
        if status == "D":
            deleted.add(path)
        elif status != "M" or old_mode.lstrip(":") != new_mode:
            raise ValueError(f"legacy directories permit deletions only: {path}")
    changes = git(root, "diff", "--no-renames", "--numstat", base, candidate,
                  "--", *LEGACY).decode("utf-8").splitlines()
    for change in changes:
        added, removed, path = change.split("\t", 2)
        if path not in deleted and (added != "0" or removed == "-"):
            raise ValueError(f"legacy directories permit deletions only: {path}")
    # A move out of the legacy tree is not a retirement.
    moves = git(root, "diff", "--find-renames", "--name-status", base, candidate).decode("utf-8")
    for line in moves.splitlines():
        fields = line.split("\t")
        if fields[0].startswith("R") and fields[1].startswith(LEGACY):
            raise ValueError(f"moving legacy code is not deletion: {fields[1]}")
    files = git(root, "ls-tree", "-r", "--name-only", candidate, "--", *LEGACY).decode("utf-8").splitlines()
    selected = [path for path in files if Path(path).suffix.lower() in {".c", ".h"}]
    batch = subprocess.run(["git", "-C", str(root), "cat-file", "--batch"], check=True,
                           input="".join(f"{candidate}:{path}\n" for path in selected).encode("utf-8"),
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    stream, lines = io.BytesIO(batch.stdout), 0
    for _ in selected:
        header = stream.readline().split()
        if len(header) != 3 or header[1] != b"blob":
            raise ValueError("legacy tree contains a non-blob source file")
        lines += len(stream.read(int(header[2])).splitlines())
        if stream.read(1) != b"\n":
            raise ValueError("invalid legacy blob boundary")
    return {"files": len(files), "c_h_files": len(selected), "c_h_lines": lines,
            "changed_files": len(changes)}


def smoke_ratchet(root: Path, base: str, candidate: str = "HEAD") -> None:
    path = "tests/smoke/cases.json"
    before = {row["id"]: row for row in json.loads(git(root, "show", f"{base}:{path}"))["cases"]}
    after = {row["id"]: row for row in json.loads(git(root, "show", f"{candidate}:{path}"))["cases"]}
    for name, row in before.items():
        if name not in after or (row["expect"] == "pass" and after[name]["expect"] != "pass"):
            raise ValueError(f"smoke case removed or passing case demoted: {name}")
        if row["expect"] == "pass" and row.get("suite") is not None and row["suite"] != after[name].get("suite"):
            raise ValueError(f"passing smoke suite removed or replaced: {name}")
    baseline = "tests/smoke/original-baseline.json"
    if git(root, "ls-tree", "--name-only", base, "--", baseline):
        if git(root, "show", f"{base}:{baseline}") != git(root, "show", f"{candidate}:{baseline}"):
            raise ValueError("the original smoke denominator cannot change")


def document_state(root: Path, docs: Path) -> dict:
    """Pair the implementation checklist with the source and CLI behavior gate."""
    text = (docs / TASK).read_text(encoding="utf-8")
    paths = (root / "src/api/xsource_paths.c").read_text(encoding="utf-8")
    lexer = (root / "src/frontend/lexer/xlex.c").read_text(encoding="utf-8")
    cli = (root / "src/app/cli/xcli_spec.c").read_text(encoding="utf-8")
    build = (root / "src/app/cli/xcmd_build.c").read_text(encoding="utf-8")
    options = re.search(r"build_options\[\]\s*=\s*\{(.*?)XR_CLI_OPT_END", cli, re.S)
    if not options:
        raise ValueError("cannot identify the registered build option surface")
    advertised = set(re.findall(r'\{\s*"([^"]+)"', options[1]))
    admitted = set(re.findall(r'strcmp\(name,\s*"([^"]+)"', build))
    provider_help = re.search(r'AOT provider: ([^"\n]+)', options[1])
    providers = set(re.findall(r'\b(auto|host|clang|gcc|msvc|zig)\b', provider_help[1])) if provider_help else set()
    accepted_providers = set(re.findall(r'strcmp\(provider,\s*"([^"]+)"', build))
    implemented = {
        "R1.2": "XR_STDLIB_SOURCE_TREE" in paths,
        "R1.3": "scanner_has_bom" in lexer,
        "R1.6": not (advertised - admitted or providers - accepted_providers),
    }
    for item, present in implemented.items():
        found = re.findall(r"^- \[([ xX])\] " + re.escape(item) + r"\b", text, re.M)
        if len(found) != 1 or (found[0].lower() == "x") != present:
            raise ValueError(f"{item}: checklist differs from current source")
    return implemented


def fixed_metrics(baseline: dict, cases: list[dict], results: list[dict]) -> dict:
    """Keep the original denominator and require real VM/native paired repairs."""
    original = baseline["cases"]
    if len(original) != 60 or len({row["id"] for row in original}) != 60:
        raise ValueError("original smoke baseline must contain exactly 60 unique cases")
    failed = {row["id"] for row in original if row["group"] == "demos" and row["expect"] == "gap"}
    if len(failed) != 20:
        raise ValueError("original smoke baseline must contain 20 failed demos")
    current = {row["id"]: row for row in cases}
    actual = {row["id"]: row for row in results}
    if len(current) != len(cases) or len(actual) != len(results):
        raise ValueError("duplicate smoke case or result")
    if not {row["id"] for row in original} <= current.keys() or set(current) != set(actual):
        raise ValueError("original case removed or smoke results incomplete")
    for row in original:
        now = current[row["id"]]
        if row["path"] != now["path"] or row["mode"] != now["mode"]:
            raise ValueError(f"original smoke identity changed: {row['id']}")
        if row["expect"] == "pass" and now["expect"] != "pass":
            raise ValueError(f"original passing case demoted: {row['id']}")

    def suite_passed(name: str) -> bool:
        case, row = current.get(name, {}), actual.get(name, {})
        if case.get("path") != "demos/07-advanced/testing.xr" and case.get("suite") is None:
            return True
        suite = row.get("suite", {})
        engine = "native" if case.get("mode") == "build-run" else "vm"
        executions = [item for item in row.get("commands", []) if item.get("stage") == "suite-" + engine]
        if (row.get("executed") is not True or len(executions) != 1 or
                executions[0].get("exit_code") != 0 or executions[0].get("timed_out") is not False):
            return False
        catalog = suite.get("catalog", {})
        entries = catalog.get("entries", [])
        if (not isinstance(entries, list) or len(entries) != 7 or any(not isinstance(item, dict) or
                set(item) != {"name", "function", "role", "timeout"} for item in entries)):
            return False
        if (any(not isinstance(item["name"], str) or not item["name"] or
                any(type(item[key]) is not int for key in ("function", "role", "timeout")) or
                not 0 <= item["function"] <= 0xffffffff or item["timeout"] < 0 for item in entries) or
                len({item["name"] for item in entries}) != 7 or len({item["function"] for item in entries}) != 7 or
                sum(item["role"] == 1 for item in entries) != 6 or sum(item["role"] == 2 for item in entries) != 1):
            return False
        return (case.get("suite") is not None and suite.get("status") == "pass"
                and suite.get("executed") is True and suite.get("engine") == engine
                and (engine != "native" or suite.get("matches_vm") is True)
                and type(catalog.get("schema")) is int and catalog["schema"] == 1 and catalog.get("source") == case.get("path")
                and isinstance(catalog.get("sourceSha256"), str) and re.fullmatch(r"[0-9a-f]{64}", catalog["sourceSha256"]) is not None
                and all(type(catalog.get(key)) is int for key in ("passed", "skipped", "failed"))
                and (catalog.get("passed"), catalog.get("skipped"), catalog.get("failed")) == (6, 1, 0)
                and isinstance(catalog.get("entries"), list) and len(catalog["entries"]) == 7
                and isinstance(suite.get("generated_c_sha256"), str) and re.fullmatch(r"[0-9a-f]{64}", suite["generated_c_sha256"]) is not None)

    def paired_suite(name: str) -> bool:
        if current[name].get("suite") is None:
            return True
        vm = actual.get(name, {}).get("suite", {})
        native = actual.get("native:" + name, {}).get("suite", {})
        return (vm.get("catalog") == native.get("catalog") and
                vm.get("generated_c_sha256") == native.get("generated_c_sha256"))

    def passed(name: str, stages: list[str]) -> bool:
        row = actual.get(name, {})
        commands = row.get("commands", [])
        if current.get(name, {}).get("suite") is not None:
            stages = stages + ["suite-native" if current[name]["mode"] == "build-run" else "suite-vm"]
        return (current.get(name, {}).get("expect") == "pass" and row.get("status") == "pass"
                and suite_passed(name)
                and row.get("executed") is True and [item.get("stage") for item in commands] == stages
                and all(item.get("exit_code") == 0 and item.get("timed_out") is False for item in commands))

    repaired = sorted(name for name in failed if passed(name, ["run"])
                      and passed("native:" + name, ["build", "run"])
                      and paired_suite(name)
                      and current["native:" + name].get("path") == current[name]["path"]
                      and current["native:" + name].get("mode") == "build-run")
    return {"original_pass": sum(actual[row["id"]]["status"] == "pass" and suite_passed(row["id"]) for row in original),
            "original_total": 60, "original_failed_demo_repaired": len(repaired),
            "original_failed_demo_total": 20, "repaired_demo_ids": repaired}


def weekly_progress(previous: dict, current: dict) -> None:
    """Check completed weekly snapshots without treating new cases as repairs."""
    old_stage, stage = previous.get("stage"), current.get("stage")
    if old_stage not in {"R3", "R4"} or stage not in {"R3", "R4"}:
        raise ValueError("weekly snapshots require explicit R3 or R4 stage")
    if old_stage == "R4" and stage == "R3":
        raise ValueError("weekly stage cannot return from R4 to R3")
    old, new = previous["metrics"], current["metrics"]
    if (date.fromisoformat(current["week_ending"]) - date.fromisoformat(previous["week_ending"])).days < 7:
        raise ValueError("weekly comparison requires snapshots at least seven days apart")
    if new["original_failed_demo_repaired"] <= old["original_failed_demo_repaired"] < 20:
        raise ValueError("weekly original failed demo repairs did not increase")
    old_lines, new_lines = previous["legacy"]["c_h_lines"], current["legacy"]["c_h_lines"]
    if new_lines > old_lines:
        raise ValueError("weekly legacy C/H lines grew during " + stage)
    if stage == "R4" and new_lines == old_lines and old_lines != 0:
        raise ValueError("R4 weekly legacy C/H lines did not decrease")
    if not set(old["repaired_demo_ids"]) <= set(new["repaired_demo_ids"]):
        raise ValueError("a previously repaired original demo regressed")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--base", required=True)
    parser.add_argument("--docs", type=Path, required=True)
    parser.add_argument("--smoke-report", type=Path)
    parser.add_argument("--snapshot", type=Path)
    parser.add_argument("--previous-week", type=Path)
    parser.add_argument("--week-ending", default=date.today().isoformat())
    parser.add_argument("--stage", choices=("R3", "R4"), required=True,
                        help="Explicit execution stage; R3 freezes legacy growth, R4 requires weekly retirement")
    args = parser.parse_args()
    value = {"stage": args.stage, "week_ending": date.fromisoformat(args.week_ending).isoformat(),
             "source_commit": git(args.root, "rev-parse", "HEAD").decode("ascii").strip(),
             "legacy": legacy_deletions(args.root, args.base),
             "checklist": document_state(args.root, args.docs)}
    if args.smoke_report:
        report = json.loads(args.smoke_report.read_text(encoding="utf-8"))
        if report["gate"] != "pass":
            raise ValueError("a passing complete smoke run is required for weekly progress")
        if report.get("identity", {}).get("commit") != value["source_commit"] or report["identity"].get("dirty") is not False:
            raise ValueError("weekly progress requires the current clean executable identity")
        baseline = json.loads((args.root / "tests/smoke/original-baseline.json").read_text(encoding="utf-8"))
        cases = json.loads((args.root / "tests/smoke/cases.json").read_text(encoding="utf-8"))["cases"]
        value["metrics"] = fixed_metrics(baseline, cases, report["cases"])
    if args.previous_week:
        if "metrics" not in value:
            raise ValueError("weekly comparison requires --smoke-report")
        weekly_progress(json.loads(args.previous_week.read_text(encoding="utf-8")), value)
    content = json.dumps(value, indent=2) + "\n"
    if args.snapshot:
        if "metrics" not in value:
            raise ValueError("weekly snapshot requires --smoke-report")
        if args.snapshot.exists():
            raise ValueError("weekly snapshot already exists")
        args.snapshot.write_text(content, encoding="utf-8")
    print(content, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
