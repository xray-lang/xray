#!/usr/bin/env python3
"""Every `_`-prefixed test module must still have someone that imports it.

All five test discoverers -- run_backend_diff.py, run_regression_tests.py,
run_aot_tests.py, run_aot_filetests.py and run_aot_manifest_sweep_tests.py --
skip any .xr file whose name begins with an underscore. That convention is
deliberate: those files are library halves of multi-module cases, and running
one on its own would prove nothing. The consequence is that an underscore file
is never compiled because a suite selected it; it is compiled only as a side
effect of building some case that imports it.

Which makes their coverage silent, and silently loseable. Delete the last case
that imports a library, or rename it and forget one call site, and the library
stops being compiled by anything. No suite reports a missing case, because no
suite ever had that case in its list. An emitter crash inside the orphaned
library body now keeps every gate green -- the code is still in the tree, still
looks tested, and is not compiled at all. This gate is what turns that silence
into a failure.

Reachability is computed over the whole tests/ tree at once, never per
directory: a library under tests/fixtures/ is legitimately imported from
tests/compile_errors/, and sharding the walk by top-level directory would
condemn it. A file is reachable when some chain of import / export-from edges
leads to it from any non-underscore .xr file, so a library imported only by
another library is reachable through it. Import specifiers are resolved the way
src/module/xmodule_resolver.c resolves them: `./x` and `../x` try `x.xr` first
and then `x/index.xr`, and anything that is not relative is a stdlib or package
name this gate has no business resolving.

Three verdicts, and the allowlist is a ratchet like every other baseline here:

  - An unreachable file that is not in tests/unreachable_case_allowlist.txt
    fails. Either give it a caller or delete it.
  - An allowlisted file that has become reachable fails too. The line must go,
    so that a future orphaning of the same file cannot hide behind it.
  - An allowlisted path that no longer exists fails, for the same reason.

Plus one stronger assertion that costs nothing once the graph is built: a
relative import whose target does not exist is a dangling edge, and fails.
That catches the rename-the-library-forget-the-caller accident at the moment
it happens, rather than when the orphaned half later starts crashing.

The tests/fuzz/corpus/ seeds are excluded from the graph. They are adversarial
byte sequences fed to libFuzzer, not programs -- some do not parse at all, and
their imports name modules that were never meant to exist. The same carve-out
is made in gen_conversion_inventory.py and check_surface_drift.py.

Exit 0 when every check passes, 1 otherwise.

Usage: check_underscore_case_reachability.py [--root PATH] [--list]
"""

from __future__ import annotations

import argparse
import subprocess
import os
import re
import sys
from collections import deque
from pathlib import Path


def _bootstrap() -> None:
    """Put tests/lib on sys.path so `import xraytest` works without an install."""
    lib = Path(__file__).resolve().parent.parent / "tests" / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import ratchet  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent
TESTS_DIR = Path("tests")
ALLOWLIST_FILE = Path("tests/unreachable_case_allowlist.txt")
UNCHECKABLE_FILE = Path("tests/uncheckable_case_allowlist.txt")

# Both `import ... from "spec"` and `import "spec" as ns` end in a quoted
# specifier, and `export { a, b } from "spec"` forwards a module the same way an
# import consumes one. An import list may wrap across lines, so the match must
# not be anchored to the start of a line -- `} from "./_lib"` is a real edge.
IMPORT_SPEC = re.compile(r'(?:from|import)\s+"([^"]+)"')

# Directories that hold build output or tool state, never test sources. Walking
# into them is how two governance gates recently blew past their 30s timeout.
SKIP_DIR_NAMES = {".git", "__pycache__", "node_modules"}
SKIP_DIR_PREFIXES = ("build", "cmake-build-")

# Adversarial parser input, not compilable programs. See the module docstring.
EXCLUDED_SUBTREES = ("tests/fuzz/corpus",)

USE_COLOR = sys.stdout.isatty() and not os.environ.get("NO_COLOR")
RED = "\033[31m" if USE_COLOR else ""
GREEN = "\033[32m" if USE_COLOR else ""
YELLOW = "\033[33m" if USE_COLOR else ""
NC = "\033[0m" if USE_COLOR else ""


def red(message: str) -> None:
    print(f"{RED}{message}{NC}")


def green(message: str) -> None:
    print(f"{GREEN}{message}{NC}")


def yellow(message: str) -> None:
    print(f"{YELLOW}{message}{NC}")


def section(title: str) -> None:
    print("")
    print(f"=== {title} ===")


def is_skipped_dir(name: str) -> bool:
    return name in SKIP_DIR_NAMES or name.startswith(SKIP_DIR_PREFIXES)


def display(path: Path, root: Path) -> str:
    """Root-relative POSIX path -- the form the allowlist stores."""
    try:
        return path.resolve().relative_to(root).as_posix()
    except ValueError:
        return path.as_posix()


def case_files(root: Path, tests_root: Path) -> list[Path]:
    """Every .xr file under tests/, minus build trees and the fuzz corpus."""
    if not tests_root.is_dir():
        return []
    found: list[Path] = []
    for dirpath, dirnames, filenames in os.walk(tests_root):
        dirnames[:] = sorted(d for d in dirnames if not is_skipped_dir(d))
        directory = Path(dirpath)
        relative = display(directory, root)
        if any(relative == prefix or relative.startswith(prefix + "/")
               for prefix in EXCLUDED_SUBTREES):
            dirnames[:] = []
            continue
        found.extend(directory / name
                     for name in filenames if name.endswith(".xr"))
    return sorted(found)


def strip_line_comment(line: str) -> str:
    """Drop a `//` comment tail.

    Import-shaped text does appear inside comments -- 1032_reexport.xr documents
    the re-export forms it tests in a comment block, and those would otherwise
    register as edges to files that never existed.
    """
    return line.split("//", 1)[0]


def import_specs(path: Path) -> list[str]:
    """Quoted module specifiers this file imports or re-exports."""
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    specs: list[str] = []
    for raw in text.splitlines():
        specs.extend(IMPORT_SPEC.findall(strip_line_comment(raw)))
    return specs


def resolve_spec(source: Path, spec: str) -> Path | None:
    """Resolve one relative specifier, or None when it is not this gate's to resolve.

    Mirrors the relative arm of src/module/xmodule_resolver.c: `<rel>.xr` first,
    then `<rel>/index.xr` for a directory module. A bare name is a stdlib module
    or a package, which has no file in this tree to point at.
    """
    if not (spec.startswith("./") or spec.startswith("../")):
        return None
    base = source.parent / spec
    direct = base if base.suffix == ".xr" else base.with_suffix(".xr")
    if direct.is_file():
        return direct.resolve()
    directory_entry = base / "index.xr"
    if directory_entry.is_file():
        return directory_entry.resolve()
    return None


def build_graph(
    files: list[Path],
) -> tuple[dict[Path, list[Path]], list[tuple[Path, str]]]:
    """Import edges between case files, plus relative edges resolving nowhere."""
    edges: dict[Path, list[Path]] = {}
    dangling: list[tuple[Path, str]] = []
    for path in files:
        targets: list[Path] = []
        for spec in import_specs(path):
            if not (spec.startswith("./") or spec.startswith("../")):
                continue
            target = resolve_spec(path, spec)
            if target is None:
                dangling.append((path, spec))
                continue
            targets.append(target)
        edges[path.resolve()] = targets
    return edges, dangling


def reachable_from_cases(
    files: list[Path], edges: dict[Path, list[Path]]
) -> set[Path]:
    """Closure over import edges starting from every discoverable (non-`_`) case."""
    seen = {path.resolve() for path in files if not path.name.startswith("_")}
    queue = deque(seen)
    while queue:
        for target in edges.get(queue.popleft(), ()):
            if target not in seen:
                seen.add(target)
                queue.append(target)
    return seen


def compile_check(root: Path, modules: list[Path], xray: Path) -> bool:
    """Front-end-check each `_`-prefixed module alone. True when any failed.

    Reachability proves a library is compiled *as part of* some case. It does
    not prove the library is individually sound: an importer that is itself
    refused for an unrelated reason takes its library's coverage down with it,
    silently. Checking each module on its own closes that, cheaply -- `xray
    check` is a front-end pass, roughly 20ms per file.

    Deliberately `check` and not `build --native`: emitter refusals on this
    branch are a tree-level state with their own baselines, and folding them in
    here would make this gate report the AOT line's condition rather than its
    own subject.
    """
    section("individual front-end check")
    if not (xray.is_file() and os.access(xray, os.X_OK)):
        red(f"FAIL: not an executable xray binary: {xray}")
        return True

    exempt = ratchet.read_baseline(root / UNCHECKABLE_FILE)
    broken: list[tuple[Path, str]] = []
    passing_exempt: list[str] = []
    for module in modules:
        name = display(module, root)
        result = subprocess.run(
            [str(xray), "check", str(module)],
            capture_output=True, text=True, cwd=str(root))
        if result.returncode != 0:
            if name in exempt:
                continue
            tail = (result.stdout + result.stderr).strip().splitlines()
            broken.append((module, tail[-1] if tail else
                           f"exit {result.returncode}, no output"))
        elif name in exempt:
            passing_exempt.append(name)

    failed = False
    if broken:
        red(f"FAIL: {len(broken)} `_`-prefixed module(s) do not pass "
            "`xray check` on their own:")
        for module, detail in broken:
            print(f"  {display(module, root)}")
            print(f"    {detail}")
        print("")
        print("  A library only ever compiles as part of some importing case,")
        print("  so a library that has stopped being sound stays invisible for")
        print("  as long as its importers are refused for other reasons.")
        print(f"  Fix it, or -- with the reason written next to it -- record it")
        print(f"  in {UNCHECKABLE_FILE}.")
        failed = True
    if passing_exempt:
        red(f"FAIL: {len(passing_exempt)} {UNCHECKABLE_FILE} entr(ies) now "
            "pass `xray check`:")
        for name in passing_exempt:
            print(f"  {name}")
        print("")
        print(f"  Delete those line(s). {UNCHECKABLE_FILE} only shrinks.")
        failed = True
    missing = sorted(name for name in exempt
                     if not (root / name).is_file())
    if missing:
        red(f"FAIL: {len(missing)} {UNCHECKABLE_FILE} entr(ies) name a file "
            "that no longer exists:")
        for name in missing:
            print(f"  {name}")
        failed = True
    if not failed:
        green(f"OK: all {len(modules) - len(exempt)} unexempt `_`-prefixed "
              f"module(s) pass `xray check` individually; {len(exempt)} "
              "recorded exception(s).")
    return failed


def main(argv: list[str]) -> int:
    os.chdir(REPO_ROOT)

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument(
        "--list",
        action="store_true",
        help="print current reachability counts and exit 0 without asserting",
    )
    parser.add_argument(
        "--check-with",
        metavar="XRAY",
        default=None,
        help="also front-end-check every `_`-prefixed module on its own with "
             "`xray check`, so a library that no longer parses or type-checks "
             "is caught even while its importers still build",
    )
    args = parser.parse_args(argv[1:])

    root = Path(args.root).resolve()
    tests_root = root / TESTS_DIR
    allowlist_path = root / ALLOWLIST_FILE

    files = case_files(root, tests_root)
    if not files:
        red(f"FAIL: no .xr files found under {display(tests_root, root)}.")
        print("")
        print("  Point --root at the repository root; this gate has nothing")
        print("  to walk otherwise.")
        return 1

    edges, dangling = build_graph(files)
    reached = reachable_from_cases(files, edges)
    underscore = [path for path in files if path.name.startswith("_")]
    unreachable = sorted(
        display(path, root)
        for path in underscore
        if path.resolve() not in reached
    )
    library_count = sum(1 for path in underscore
                        if path.name.endswith("_lib.xr"))

    if args.list:
        section("reachability inventory")
        print(f"  case files walked:        {len(files)}")
        print(f"  `_`-prefixed modules:     {len(underscore)}")
        print(f"    of which *_lib.xr:      {library_count}")
        print(f"  reachable from a case:    {len(underscore) - len(unreachable)}")
        print(f"  unreachable:              {len(unreachable)}")
        print(f"  dangling relative imports: {len(dangling)}")
        for entry in unreachable:
            print(f"    unreachable: {entry}")
        for source, spec in dangling:
            print(f'    dangling:    {display(source, root)} -> "{spec}"')
        section("summary")
        green("Listing only; no assertions were made.")
        return 0

    failed = False
    allowlist = ratchet.read_baseline(allowlist_path)

    section("graph")
    green(f"Walked {len(files)} case file(s); {len(underscore)} carry the "
          f"`_` prefix ({library_count} named *_lib.xr).")
    if not allowlist:
        yellow(f"Note: {ALLOWLIST_FILE} is empty or absent; nothing is "
               "exempt from this gate.")

    section("dangling imports")
    if dangling:
        red("FAIL: relative import(s) resolving to no file:")
        for source, spec in dangling:
            print(f'  {display(source, root)} -> "{spec}"')
        print("")
        print("  A relative specifier must resolve to <spec>.xr or to")
        print("  <spec>/index.xr. Fix the specifier, or restore the module it")
        print("  names -- a renamed library with a stale caller lands here.")
        failed = True
    else:
        green("OK: every relative import resolves to a file.")

    section("unreachable modules")
    orphans = [entry for entry in unreachable if entry not in allowlist]
    if orphans:
        red("FAIL: `_`-prefixed module(s) no case imports, directly or "
            "transitively:")
        for entry in orphans:
            print(f"  {entry}")
        print("")
        print("  Every test discoverer skips these files, so nothing compiles")
        print("  them any more and their coverage is gone. Either import each")
        print("  one from a case that a suite collects, or delete it. To")
        print("  exempt one instead, append its path to")
        print(f"  {ALLOWLIST_FILE} with a comment saying why:")
        print("")
        for entry in orphans:
            print(f"    echo '{entry}  # why it may stay orphaned' >> "
                  f"{ALLOWLIST_FILE}")
        failed = True
    else:
        green(f"OK: all {len(underscore)} `_`-prefixed module(s) are reachable "
              "or allowlisted.")

    section("allowlist ratchet")
    tracked = {display(path, root) for path in files}
    now_reachable = sorted(entry for entry in allowlist
                           if entry in tracked and entry not in unreachable)
    missing = sorted(entry for entry in allowlist if not (root / entry).is_file())
    if now_reachable:
        red(f"FAIL: {ALLOWLIST_FILE} entries that are reachable again:")
        for entry in now_reachable:
            print(f"  {entry}")
        print("")
        print("  The allowlist may only shrink. Delete the lines above, so a")
        print("  future orphaning of these files cannot hide behind them.")
        failed = True
    if missing:
        red(f"FAIL: {ALLOWLIST_FILE} entries naming files that no longer exist:")
        for entry in missing:
            print(f"  {entry}")
        print("")
        print("  Delete the stale lines, or correct the paths if the modules")
        print("  were moved rather than removed.")
        failed = True
    if not now_reachable and not missing:
        green(f"OK: {len(allowlist)} allowlist entr(ies) are all still "
              "unreachable and present.")

    if args.check_with:
        failed = compile_check(root, underscore, Path(args.check_with)) or failed

    section("summary")
    if failed:
        red("underscore case reachability: one or more checks failed.")
        return 1
    green("underscore case reachability: all checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
