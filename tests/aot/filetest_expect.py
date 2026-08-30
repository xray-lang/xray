"""The .expect directive language for AOT filetests.

A .expect file is a line-oriented DSL asserting facts about a case's XAOT dump
and its generated C. Splitting the language out from the runner keeps the
directive semantics unit-testable without a compiler in the loop -- the gap that
let the missing `-I include` in the C syntax check hide behind a warm cache.

Directives (KEY=VALUE, `#` comments, blank lines ignored):

  args=...            build arguments (consumed by the runner, not a check)
  status=pass|fail    whether the dump/build is expected to succeed (default pass)
  product_status=fail require the product command to fail after any contract passes
  product_contains=S require literal S in product-command output
  artifact=absent    require the requested output path to remain absent
  skip=reason         report the case as skipped
  contains=S          the dump must contain literal S
  not_contains=S      the dump must not contain literal S
  regex=RE            the dump must match extended regex RE
  not_regex=RE        the dump must not match RE
  c_contains=S        the generated C must contain literal S
  c_not_contains=S    the generated C must not contain literal S
  c_count=N:S         the generated C must contain literal S exactly N times
  c_regex=RE          the generated C must match RE
  c_not_regex=RE      the generated C must not match RE
  c_syntax=pass       the generated C must compile (handled by the runner)
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

# Directives that are inputs to the runner, not assertions checked here.
_RUNNER_KEYS = {
    "args",
    "status",
    "c_syntax",
    "product_status",
    "product_contains",
    "artifact",
}

# Regex directives match line-wise, the way `grep -E` did: expect files anchor
# with ^...$ meaning "some line looks like this", so ^ and $ must bind to line
# boundaries, not to the whole dump. Without MULTILINE every anchored assertion
# silently fails against a multi-line dump.
_REGEX_FLAGS = re.MULTILINE


def _search(pattern: str, text: str) -> re.Match | None:
    return re.search(pattern, text, _REGEX_FLAGS)

# Assertion directives and whether each targets the dump or the generated C.
_DUMP_KEYS = {"contains", "not_contains", "regex", "not_regex"}
_C_KEYS = {"c_contains", "c_not_contains", "c_count", "c_regex", "c_not_regex"}
_KNOWN_KEYS = _RUNNER_KEYS | _DUMP_KEYS | _C_KEYS | {"skip"}
_SINGLETON_KEYS = {"args", "status", "c_syntax", "skip", "product_status", "artifact"}


@dataclass
class Directive:
    lineno: int
    key: str
    value: str


@dataclass
class Expect:
    path: Path
    directives: list[Directive] = field(default_factory=list)
    status: str = "pass"
    args: str = ""
    parse_error: str | None = None

    @property
    def wants_c_syntax(self) -> bool:
        return any(d.key == "c_syntax" and d.value == "pass" for d in self.directives)

    @property
    def wants_product_failure(self) -> bool:
        return any(d.key == "product_status" and d.value == "fail" for d in self.directives)

    @property
    def product_contains(self) -> list[str]:
        return [d.value for d in self.directives if d.key == "product_contains"]

    @property
    def wants_absent_artifact(self) -> bool:
        return any(d.key == "artifact" and d.value == "absent" for d in self.directives)


def _parse_failure(path: Path, lineno: int, detail: str) -> str:
    return f"bad expect directive: {path}:{lineno}: {detail}"


def _validate_combinations(exp: Expect) -> str | None:
    product_status = [d for d in exp.directives if d.key == "product_status"]
    product_contains = [d for d in exp.directives if d.key == "product_contains"]
    artifact = [d for d in exp.directives if d.key == "artifact"]
    if not product_status and (product_contains or artifact):
        offender = (product_contains or artifact)[0]
        return _parse_failure(
            exp.path, offender.lineno, f"{offender.key} requires product_status=fail"
        )
    if product_status:
        status_directive = product_status[0]
        if exp.status == "fail":
            return _parse_failure(
                exp.path,
                status_directive.lineno,
                "product directives cannot be combined with status=fail",
            )
        if not product_contains:
            return _parse_failure(
                exp.path,
                status_directive.lineno,
                "product_status=fail requires at least one product_contains directive",
            )
        if not artifact:
            return _parse_failure(
                exp.path,
                status_directive.lineno,
                "product_status=fail requires artifact=absent",
            )
        c_directive = next(
            (d for d in exp.directives if d.key in _C_KEYS or d.key == "c_syntax"), None
        )
        if c_directive is not None:
            return _parse_failure(
                exp.path,
                c_directive.lineno,
                "artifact=absent cannot be combined with generated-C directives",
            )
    return None


def parse(path: Path) -> Expect:
    """Parse a .expect file. A malformed directive is recorded, not raised, so
    the runner can report it as a case failure with the offending line."""
    exp = Expect(path=path)
    if not path.is_file():
        return exp
    seen_singletons: set[str] = set()
    seen_product_contains: set[str] = set()
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw.rstrip("\r")
        if not line or line.lstrip().startswith("#"):
            continue
        if "=" not in line:
            exp.parse_error = _parse_failure(path, lineno, "expected KEY=VALUE")
            return exp
        key, value = line.split("=", 1)
        if key not in _KNOWN_KEYS:
            exp.parse_error = _parse_failure(path, lineno, f"unknown key {key!r}")
            return exp
        if not value:
            exp.parse_error = _parse_failure(path, lineno, f"{key} requires a non-empty value")
            return exp
        if key in _SINGLETON_KEYS:
            if key in seen_singletons:
                exp.parse_error = _parse_failure(path, lineno, f"duplicate {key} directive")
                return exp
            seen_singletons.add(key)
        if key == "product_contains":
            if value in seen_product_contains:
                exp.parse_error = _parse_failure(
                    path, lineno, f"duplicate product_contains value {value!r}"
                )
                return exp
            seen_product_contains.add(value)
        if key == "status" and value not in {"pass", "fail"}:
            exp.parse_error = _parse_failure(path, lineno, "status must be pass or fail")
            return exp
        if key == "product_status" and value != "fail":
            exp.parse_error = _parse_failure(path, lineno, "product_status must be fail")
            return exp
        if key == "artifact" and value != "absent":
            exp.parse_error = _parse_failure(path, lineno, "artifact must be absent")
            return exp
        if key == "c_syntax" and value != "pass":
            exp.parse_error = _parse_failure(path, lineno, "c_syntax must be pass")
            return exp
        exp.directives.append(Directive(lineno, key, value))
        if key == "status":
            exp.status = value
        elif key == "args":
            exp.args = value
    exp.parse_error = _validate_combinations(exp)
    return exp


def check_product(exp: Expect, product_text: str) -> CheckOutcome:
    """Evaluate only product-output assertions after an expected product failure."""
    if exp.parse_error:
        return CheckOutcome(exp.parse_error)
    for needle in exp.product_contains:
        if needle not in product_text:
            return CheckOutcome(f"missing product output: {needle}")
    return CheckOutcome()


# A check result: None means pass; a string is the failure/skip reason. The
# `skip` flag distinguishes a requested skip from a failure.
@dataclass
class CheckOutcome:
    reason: str | None = None
    skip: bool = False

    @property
    def ok(self) -> bool:
        return self.reason is None


def _count_literal(haystack: str, needle: str) -> int:
    if not needle:
        return 0
    count = start = 0
    while True:
        idx = haystack.find(needle, start)
        if idx < 0:
            return count
        count += 1
        start = idx + len(needle)


def check(exp: Expect, dump_text: str, c_text: str) -> CheckOutcome:
    """Evaluate every assertion directive against the dump and generated C.

    Returns the first failure (matching the shell runner's fail-on-first-miss
    order), a skip, or a clean pass. c_syntax is not evaluated here; the runner
    compiles the C and reports that separately.
    """
    if exp.parse_error:
        return CheckOutcome(exp.parse_error)

    for d in exp.directives:
        key, value = d.key, d.value
        if key in _RUNNER_KEYS:
            continue
        if key == "skip":
            return CheckOutcome(value, skip=True)

        if key == "contains":
            if value not in dump_text:
                return CheckOutcome(f"missing: {value}")
        elif key == "not_contains":
            if value in dump_text:
                return CheckOutcome(f"unexpected: {value}")
        elif key == "regex":
            if not _search(value, dump_text):
                return CheckOutcome(f"missing regex: {value}")
        elif key == "not_regex":
            if _search(value, dump_text):
                return CheckOutcome(f"unexpected regex: {value}")
        elif key == "c_contains":
            if value not in c_text:
                return CheckOutcome(f"missing generated C: {value}")
        elif key == "c_not_contains":
            if value in c_text:
                return CheckOutcome(f"unexpected generated C: {value}")
        elif key == "c_count":
            if ":" not in value:
                return CheckOutcome(f"bad c_count directive: {value}")
            count_raw, needle = value.split(":", 1)
            if not count_raw.isdigit():
                return CheckOutcome(f"bad c_count directive: {value}")
            actual = _count_literal(c_text, needle)
            if actual != int(count_raw):
                return CheckOutcome(
                    f"generated C count: expected {int(count_raw)}, got {actual}: {needle}"
                )
        elif key == "c_regex":
            if not _search(value, c_text):
                return CheckOutcome(f"missing generated C regex: {value}")
        elif key == "c_not_regex":
            if _search(value, c_text):
                return CheckOutcome(f"unexpected generated C regex: {value}")
        else:
            return CheckOutcome(f"bad expect directive: {exp.path}:{d.lineno}")

    return CheckOutcome()


def target_triple(args: str) -> str:
    """The `--target <triple>` a case builds for, or "" if none.

    Generated C for a foreign target pulls in that target's intrinsics and libc
    headers, so a host compiler cannot parse it without the same target.
    """
    tokens = args.split()
    prev = ""
    for tok in tokens:
        if prev == "--target":
            return tok
        if tok.startswith("--target="):
            return tok[len("--target="):]
        prev = tok
    return ""


# The 16-hex monomorphization suffix the link mode strips before matching, so a
# case can assert on a symbol shape without pinning its content hash.
_LINK_SUFFIX_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)_[0-9a-f]{16}_")


def normalize_link_c(text: str) -> str:
    return _LINK_SUFFIX_RE.sub(lambda m: m.group(1) + "_", text)
