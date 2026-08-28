#!/usr/bin/env python3
"""The `.expected` sibling format for compile-error cases, and the parser that
reads the compiler's own diagnostics back into the same shape.

A compile-error case is only worth its runtime if it pins WHERE the compiler
points, not merely that it complained. Every assertion therefore opens with a
location line mirroring the compiler's own location gutter:

    --> 5:9 E0352
    Index type 'string' is not assignable to expected type 'i64'

    --> circular_dependency_peer.xr:3:1 E0504
    circular dependency

Grammar:

  * `#` at the start of a line is a comment; blank lines are ignored.
  * `--> ` opens an assertion. What follows is `[<file>:]<line>:<col>`,
    optionally then an `E####` code. The file defaults to the case file, so the
    common single-file assertion stays short; naming another file is how a case
    pins a diagnostic raised inside an imported module.
  * `--> ?` (in place of a position) asserts a diagnostic the compiler emits
    with NO location at all.
  * `misplaced <[file:]line:col>` says the assertion above pins where the
    compiler currently points while the defect actually sits elsewhere. The
    runner reports those cases as diagnostic-position gaps instead of passes,
    the same way `.expected-runtime` reports compile-time gaps. Deleting the
    line once the compiler is fixed is what turns the gap back into a demand.
  * Every remaining non-blank line of an assertion is a literal substring that
    must appear in THAT diagnostic's text -- not merely somewhere in the run's
    combined output. Substrings, not patterns: a message containing regex
    metacharacters must never silently match something else.

Matching is one-to-one: each expected assertion claims a distinct emitted
diagnostic. Two assertions can never be satisfied by the same diagnostic, so a
case listing three errors really does require three.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

# The compiler emits diagnostics in three shapes, all of which must parse back
# into the same structure or a case could pin words the runner never checks.
#
#   error[E0352]: msg          the frontend's rendered form; the location
#    --> file:line:col         follows the message, possibly after `hint:` lines
#
#   file:line:col: error: msg  the checker's one-line form, location first
#
#   Error: msg                 module resolution, and the Xi pipeline's
#   [xcompiler] ... failed at <stage>:   -- neither carries a location
RENDERED_HEAD_RE = re.compile(
    r"^(?P<level>error|warning|note|Error)(?:\[(?P<code>[EWN][0-9]+)\])?:(?P<msg>.*)$")
INLINE_HEAD_RE = re.compile(
    r"^(?P<file>\S.*?):(?P<line>[0-9]+):(?P<col>[0-9]+): "
    r"(?P<level>error|warning|note)(?:\[(?P<code>[EWN][0-9]+)\])?: (?P<msg>.*)$")
LOCATION_RE = re.compile(r"^\s*--> (?P<file>.+):(?P<line>[0-9]+):(?P<col>[0-9]+)\s*$")
LOCATIONLESS_RE = re.compile(r"^\[xcompiler\].*failed at \S+:")
INLINE_CODE_RE = re.compile(r"^(?P<code>[EWN][0-9]{4}): (?P<rest>.*)$")
# Source-context gutter: `  |`, `5 | var x = ...`, `  |     ^^^`.
GUTTER_RE = re.compile(r"^\s*(?:[0-9]+\s*)?\|")
# The run's own tally, not a diagnostic -- counting it would let a case claim a
# match against the summary line instead of against a real error.
SUMMARY_RE = re.compile(r"^error: aborting due to [0-9]+ previous error")
# Structured logging the module graph writes alongside its diagnostics.
LOG_NOISE_RE = re.compile(r"^\[(?:WARNING|INFO|DEBUG|TRACE)\] ")

NO_POSITION = "?"
MISPLACED_KEYWORD = "misplaced "


class ExpectedFormatError(Exception):
    """The expected file does not parse. Never silently downgraded to a match."""


@dataclass
class Diagnostic:
    """One diagnostic as the compiler emitted it."""

    level: str
    code: str | None
    file: str | None
    line: int | None
    col: int | None
    text: str

    def position_text(self, case_name: str = "") -> str:
        if self.line is None:
            return "no position"
        where = Path(self.file).name if self.file else "<unknown>"
        if where == case_name:
            return f"{self.line}:{self.col}"
        return f"{where}:{self.line}:{self.col}"


@dataclass
class Position:
    file: str | None  # None means "the case file itself"
    line: int | None  # None is the `?` form: the compiler emits no position
    col: int | None

    def text(self) -> str:
        if self.line is None:
            return NO_POSITION
        return f"{self.file}:{self.line}:{self.col}" if self.file else f"{self.line}:{self.col}"

    @property
    def degenerate(self) -> bool:
        """A position the compiler could not have meant.

        Two shapes qualify. A diagnostic with no location at all (`?`) points
        the reader at the file and stops. And since lines and columns are
        1-based, a zero is the diagnostic falling back to an unset span rather
        than pointing anywhere.

        Cases pin what the compiler actually reports -- inventing a plausible
        position would hide the defect -- and the runner counts these as
        position gaps rather than passes.
        """
        return self.line is None or self.line < 1 or self.col < 1

    def gap_reason(self) -> str:
        if self.line is None:
            return "carries no location at all"
        return f"reports {self.text()}, which is not a 1-based position"

    def matches(self, diagnostic: Diagnostic, case_name: str) -> bool:
        if self.line is None:
            return diagnostic.line is None
        if diagnostic.line != self.line or diagnostic.col != self.col:
            return False
        emitted = Path(diagnostic.file).name if diagnostic.file else None
        return emitted == (self.file or case_name)


@dataclass
class Assertion:
    """One `-->` block of an expected file."""

    where: Position
    code: str | None
    misplaced: Position | None = None
    texts: list[str] = field(default_factory=list)
    source_line: int = 0  # line number inside the expected file, for reports

    def position_text(self) -> str:
        return self.where.text()

    def misplaced_text(self) -> str:
        return self.misplaced.text() if self.misplaced else ""

    def render(self) -> str:
        head = f"--> {self.where.text()}" + (f" {self.code}" if self.code else "")
        lines = [head]
        if self.misplaced:
            lines.append(f"misplaced {self.misplaced.text()}")
        lines.extend(self.texts)
        return "\n".join(lines) + "\n"

    def describe(self) -> str:
        code = f" {self.code}" if self.code else ""
        return f"--> {self.where.text()}{code}"


def expected_path_for(case: Path) -> tuple[Path, str]:
    """The sibling that governs this case, and whether it is a coverage gap."""
    runtime_sibling = Path(str(case) + ".expected-runtime")
    if runtime_sibling.is_file():
        return runtime_sibling, "runtime"
    return Path(str(case) + ".expected"), "compile"


def parse_expected(path: Path) -> list[Assertion]:
    """Read an expected file into assertions, in declaration order."""
    if not path.is_file():
        raise ExpectedFormatError(f"{path.name}: no expected file")

    assertions: list[Assertion] = []
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.rstrip()
        if not line or line.lstrip().startswith("#"):
            continue
        if line.startswith("--> "):
            assertions.append(_parse_head(line[4:].strip(), path, number))
        elif not assertions:
            raise ExpectedFormatError(
                f"{path.name}:{number}: text before any `--> line:col` assertion")
        elif line.startswith(MISPLACED_KEYWORD):
            current = assertions[-1]
            if current.misplaced:
                raise ExpectedFormatError(
                    f"{path.name}:{number}: a second `misplaced` for one assertion")
            current.misplaced = _parse_position(
                line[len(MISPLACED_KEYWORD):].strip(), path, number)
            if current.misplaced.text() == current.where.text():
                raise ExpectedFormatError(
                    f"{path.name}:{number}: `misplaced` repeats the asserted "
                    "position, so it claims no defect")
        else:
            assertions[-1].texts.append(line)
    if not assertions:
        raise ExpectedFormatError(f"{path.name}: no `--> line:col` assertion")
    return assertions


def _parse_head(rest: str, path: Path, number: int) -> Assertion:
    parts = rest.split()
    if not parts:
        raise ExpectedFormatError(f"{path.name}:{number}: `-->` with no position")
    code = None
    if len(parts) > 1:
        code = parts[1]
        if not re.fullmatch(r"[EWN][0-9]{4}", code):
            raise ExpectedFormatError(
                f"{path.name}:{number}: `{code}` is not an error code")
        if len(parts) > 2:
            raise ExpectedFormatError(
                f"{path.name}:{number}: trailing text after the code")
    return Assertion(_parse_position(parts[0], path, number), code, source_line=number)


def _parse_position(where: str, path: Path, number: int) -> Position:
    if where == NO_POSITION:
        return Position(None, None, None)
    fields = where.split(":")
    if len(fields) == 2:
        file_part, line_part, col_part = None, fields[0], fields[1]
    elif len(fields) == 3:
        file_part, line_part, col_part = fields
    else:
        raise ExpectedFormatError(
            f"{path.name}:{number}: `{where}` is not `[file:]line:col` or `?`")
    if not (line_part.isdigit() and col_part.isdigit()):
        raise ExpectedFormatError(
            f"{path.name}:{number}: `{where}` has a non-numeric line or column")
    return Position(file_part, int(line_part), int(col_part))


def parse_diagnostics(output: str) -> list[Diagnostic]:
    """Split compiler output into the diagnostics it actually emitted.

    A diagnostic runs from its head line to the next head, and its text is that
    span minus the source-context gutter -- so an assertion's substrings are
    checked against one diagnostic's own wording rather than against the whole
    run. A trailing `--> file:line:col` binds to the newest diagnostic that has
    no location yet, which is what lets a `hint:` line sit between a message and
    its own location.
    """
    diagnostics: list[Diagnostic] = []
    body: list[str] = []

    def flush() -> None:
        if diagnostics and body:
            diagnostics[-1].text += "\n" + "\n".join(body)
        body.clear()

    for raw in output.splitlines():
        line = raw.rstrip()
        stripped = line.strip()
        if not stripped or SUMMARY_RE.match(stripped) or LOG_NOISE_RE.match(stripped):
            continue
        if GUTTER_RE.match(line):
            continue

        inline = INLINE_HEAD_RE.match(stripped)
        if inline:
            flush()
            diagnostics.append(Diagnostic(
                inline.group("level").lower(), inline.group("code"),
                inline.group("file"), int(inline.group("line")),
                int(inline.group("col")), inline.group("msg").strip()))
            continue
        rendered = RENDERED_HEAD_RE.match(stripped)
        if rendered:
            flush()
            code, message = rendered.group("code"), rendered.group("msg").strip()
            # Module resolution writes `Error: E0504: ...` rather than the
            # frontend's `error[E0504]: ...`. Same code, so the assertion head
            # carries it either way.
            inline_code = INLINE_CODE_RE.match(message)
            if code is None and inline_code:
                code, message = inline_code.group("code"), inline_code.group("rest")
            diagnostics.append(Diagnostic(
                rendered.group("level").lower(), code, None, None, None, message))
            continue
        if LOCATIONLESS_RE.match(stripped):
            flush()
            diagnostics.append(Diagnostic("error", None, None, None, None, stripped))
            continue
        where = LOCATION_RE.match(line)
        if where and diagnostics and diagnostics[-1].line is None:
            current = diagnostics[-1]
            current.file = where.group("file")
            current.line = int(where.group("line"))
            current.col = int(where.group("col"))
            continue
        body.append(stripped)
    flush()
    return diagnostics


def match_assertions(assertions: list[Assertion], emitted: list[Diagnostic],
                     case_name: str) -> list[str]:
    """Claim one distinct diagnostic per assertion; report what did not match.

    Assertions are matched in declaration order and a claimed diagnostic is
    never offered again, so N assertions demand N diagnostics. When nothing
    matches, the report says which half failed -- a diagnostic carrying the
    right words at the wrong position is the finding this suite exists to
    surface, so it is named explicitly rather than folded into "no match".
    """
    claimed: set[int] = set()
    failures: list[str] = []

    for assertion in assertions:
        matched = None
        text_only: list[Diagnostic] = []
        for index, diagnostic in enumerate(emitted):
            if index in claimed:
                continue
            texts_ok = all(t in diagnostic.text for t in assertion.texts)
            code_ok = assertion.code is None or assertion.code == diagnostic.code
            if not (texts_ok and code_ok):
                if texts_ok:
                    text_only.append(diagnostic)
                continue
            if assertion.where.matches(diagnostic, case_name):
                matched = index
                break
            text_only.append(diagnostic)
        if matched is not None:
            claimed.add(matched)
            continue
        if text_only:
            found = text_only[0]
            code = f" [{found.code}]" if found.code else ""
            failures.append(
                f"{assertion.describe()}: matched the wording{code} at "
                f"{found.position_text(case_name)}, not the asserted position")
            continue
        taken = [i for i in claimed
                 if all(t in emitted[i].text for t in assertion.texts)]
        if taken:
            failures.append(
                f"{assertion.describe()}: the only diagnostic carrying this "
                f"wording was already claimed by an earlier assertion; one "
                f"diagnostic satisfies one assertion")
            continue
        wanted = "; ".join(assertion.texts) or "(no text)"
        failures.append(f"{assertion.describe()}: no diagnostic carrying {wanted!r}")

    return failures
