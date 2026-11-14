#!/usr/bin/env python3
"""Check the commit messages of a revision range against the repository rules.

Commit history is the permanent record of why the code is the way it is, so a
message must stand on its own:

  * A commit that touches the compiler core, the runtime or the public headers,
    or more than ten files, explains itself in a body: what semantic or
    invariant changed, why, and what was verified.
  * A merge names what it brings in instead of Git's default subject.
  * No message names a tool as a co-author, refers to a Markdown document by
    path, or speaks in plan phases (Phase A, Step 2, P0, Round 3); documents
    move and phases expire.

Usage:
  check_commit_messages.py <base>..<head>
  check_commit_messages.py --self-test

Exit 0 when every commit in the range passes, 1 otherwise.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# A change to any of these is a change to language or runtime behavior.
CORE_PREFIXES = ("src/xir/", "src/lir/", "src/vm/", "src/runtime/", "src/hir/", "include/")
MAX_FILES_WITHOUT_BODY = 10

DEFAULT_MERGE = re.compile(
    r"^Merge (?:commit '[0-9a-f]+'|branch '[^']+'|remote-tracking branch '[^']+')"
    r"(?: of \S+)?(?: into \S+)?$")
CO_AUTHOR = re.compile(r"^co-authored-by:.*\b(claude|anthropic|copilot|codex|chatgpt|gpt)\b",
                       re.IGNORECASE | re.MULTILINE)
MARKDOWN_PATH = re.compile(r"[\w./-]+\.md\b")
PHASE_TALK = re.compile(r"\b(?:Phase [A-Z0-9]\b|Step [0-9]+\b|Round [0-9]+\b|P[0-9]\b)")


@dataclass(frozen=True)
class Commit:
    sha: str
    parents: int
    subject: str
    body: str
    files: tuple[str, ...]


def problems(commit: Commit) -> list[str]:
    found = []
    message = commit.subject + "\n" + commit.body
    if commit.parents > 1:
        if DEFAULT_MERGE.match(commit.subject):
            found.append("merge keeps Git's default subject; name what it brings in")
    elif not commit.body.strip():
        core = [path for path in commit.files if path.startswith(CORE_PREFIXES)]
        if core:
            found.append(f"changes {core[0]} without a body explaining what and why")
        elif len(commit.files) > MAX_FILES_WITHOUT_BODY:
            found.append(f"changes {len(commit.files)} files without a body")
    if CO_AUTHOR.search(message):
        found.append("names a tool as co-author")
    markdown = MARKDOWN_PATH.search(message)
    if markdown:
        found.append(f"refers to the document {markdown.group(0)} by path")
    phase = PHASE_TALK.search(message)
    if phase:
        found.append(f"uses plan phase wording '{phase.group(0)}'")
    return found


def git(*args: str) -> str:
    return subprocess.run(["git", "-C", str(REPO_ROOT), *args], check=True,
                          capture_output=True, text=True, encoding="utf-8").stdout


def read_commits(revision_range: str) -> list[Commit]:
    commits = []
    for sha in git("rev-list", "--reverse", revision_range).split():
        parents = len(git("rev-list", "--parents", "-n", "1", sha).split()) - 1
        subject = git("log", "-1", "--format=%s", sha).rstrip("\n")
        body = git("log", "-1", "--format=%b", sha)
        files = tuple(git("diff-tree", "--no-commit-id", "--name-only", "-r", "-m",
                          "--first-parent", sha).split())
        commits.append(Commit(sha, parents, subject, body, files))
    return commits


def self_test() -> int:
    good = Commit("1", 1, "Fix a thing", "Why and what was checked.\n", ("src/xir/a.c",))
    cases = [
        (good, False),
        (Commit("2", 1, "Fix a thing", "", ("src/xir/a.c",)), True),
        (Commit("3", 1, "Tidy docs", "", ("docs/a.txt",)), False),
        (Commit("4", 1, "Move tests", "", tuple(f"tests/t{i}.py" for i in range(11))), True),
        (Commit("5", 2, "Merge commit 'abc123' into HEAD", "", ()), True),
        (Commit("6", 2, "Merge remote-tracking branch 'origin/main' into agent/x", "", ()), True),
        (Commit("7", 2, "Integrate the String cursor", "", ("src/xir/a.c",)), False),
        (Commit("8", 1, "Fix", "Body.\n\nCo-Authored-By: Claude <noreply@anthropic.com>\n", ()), True),
        (Commit("9", 1, "Fix", "See tasks/339-plan.md for details.\n", ()), True),
        (Commit("10", 1, "Finish Phase B of the cutover", "Body.\n", ()), True),
        (Commit("11", 1, "Keep P10 wide registers", "Body.\n", ()), False),
    ]
    failed = [commit.sha for commit, bad in cases if bool(problems(commit)) != bad]
    if failed:
        print("self-test FAILED for cases: " + ", ".join(failed))
        return 1
    print(f"self-test passed: {len(cases)} cases")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("range", nargs="?", help="revision range such as origin/main..HEAD")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv[1:])
    if args.self_test:
        return self_test()
    if not args.range:
        parser.error("a revision range is required")
    commits = read_commits(args.range)
    failures = 0
    for commit in commits:
        for problem in problems(commit):
            print(f"{commit.sha[:12]} {commit.subject}: {problem}")
            failures += 1
    print(f"{len(commits)} commit(s) checked, {failures} problem(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
