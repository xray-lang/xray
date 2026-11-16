#!/usr/bin/env python3
"""Execute explicit G13 spec modes; report actual failures without rewriting baselines."""
import argparse
import collections
import hashlib
import json
import os
import platform
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ELLIPSIS_RE = re.compile(r"(^|[^.])\.\.\.($|[^.])")
ERROR_MARK_RE = re.compile(r"编译错误|error\[E\d{4}\]|// error:")
MAIN_CALL_RE = re.compile(r"^(?:try[ \t]+)?main\(\)\s*$", re.M)
MODE_RE = re.compile(r"@(?:check|run|fragment|error=E\d{4})\Z")
PRIMARY_ERROR_RE = re.compile(r"^(.+):(\d+):(\d+):\s*error(?:\[(E\d{4})\])?:\s*(?:(E\d{4}):)?", re.M)
WORD_RE = re.compile(r"(?:[^\W\d]|_)\w*", re.UNICODE)
NUMBER_RE = re.compile(r"(?:0[xX][0-9a-fA-F_]+|0[bB][01_]+|(?:\d[\d_]*\.[\d_]+|\d[\d_]*)(?:[eE][+-]?[\d_]+)?n?)")
OPERATORS = sorted(("...", "..<", "..=", "=>", "->", "??", "?.", "==", "!=", "<=", ">=", "&&", "||", "<<=", ">>=", "<<", ">>", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "::"), key=len, reverse=True)


def classify(code):
    """Legacy inventory helper only; never grants a mode or execution verdict."""
    if ERROR_MARK_RE.search(code):
        return "error-demo"
    if ELLIPSIS_RE.search(code) or not MAIN_CALL_RE.search(code):
        return "fragment"
    return "runnable"


def parse_document(text, source="<memory>"):
    lines = text.splitlines()
    blocks, errors = [], []
    language, index = None, 0
    tagged = "<!-- xr-spec:cn -->" in text or "<!-- xr-spec:en -->" in text
    while index < len(lines):
        stripped = lines[index].strip()
        if stripped in ("<!-- xr-spec:cn -->", "<!-- xr-spec:en -->"):
            language = "cn" if ":cn" in stripped else "en"
        elif stripped in ("<!-- /xr-spec:cn -->", "<!-- /xr-spec:en -->"):
            language = None
        if not re.match(r"^```xray(?:\s|$)", stripped):
            index += 1
            continue
        opening, markers = index, stripped.split()[1:]
        index += 1
        body = []
        while index < len(lines) and lines[index].strip() != "```":
            body.append(lines[index])
            index += 1
        if index == len(lines):
            errors.append(dict(status="METADATA_FAIL", source=source, line=opening + 1, detail="unclosed xray fence"))
            break
        block = dict(source=source, line=opening + 2, fence_line=opening + 1,
                     code="\n".join(body), markers=markers, language=language,
                     tagged=tagged, expected_stdout=None)
        shared = re.fullmatch(r"<!-- xr-source:example ([A-Za-z0-9_.-]+) -->",
                              lines[opening - 1].strip()) if opening else None
        block["source_key"] = shared[1] if shared else None
        ids = [marker[4:] for marker in markers if marker.startswith("@id=")]
        block["id"] = ids[0] if len(ids) == 1 else None
        block["id_errors"] = len(ids) > 1 or bool(ids and not ids[0])
        following = index + 1
        while following < len(lines) and not lines[following].strip():
            following += 1
        if following < len(lines) and lines[following].strip() == "```output":
            output, following = [], following + 1
            while following < len(lines) and lines[following].strip() != "```":
                output.append(lines[following])
                following += 1
            if following == len(lines):
                block["output_error"] = "unclosed output fence"
            else:
                block["expected_stdout"] = "".join(line + "\n" for line in output)
        blocks.append(block)
        index += 1
    return blocks, errors


def extract_blocks(spec_text):
    """Stable three-tuple extractor used by spec/MCP consumers."""
    return [(b["line"], b["code"], b["markers"]) for b in parse_document(spec_text)[0]]


def snippet_key(start_line, code):
    del start_line
    return "sha:" + hashlib.sha256(code.encode("utf-8")).hexdigest()[:12]


def lexical_facts(code, *, with_spans=False):
    """Remove real comments while preserving complete literal spellings and token boundaries.

    Interpolation remains verbatim inside its literal token, conservatively
    refusing translated inner comments rather than erasing string contents.
    """
    tokens, comments, spans = [], [], []

    def quoted(start, raw=False):
        quote = code[start]
        count = 1
        while start + count < len(code) and code[start + count] == quote:
            count += 1
        if count == 2:
            return start + 2
        delimiter = quote * (count if count >= 3 else 1)
        at = start + len(delimiter)
        while at < len(code):
            if code.startswith(delimiter, at):
                return at + len(delimiter)
            if not raw and code[at] == "\\":
                at += 2
                continue
            if code.startswith("${", at):
                depth, at = 1, at + 2
                while at < len(code) and depth:
                    if code[at] in "\"'`":
                        at = quoted(at)
                    elif code.startswith("//", at):
                        end = code.find("\n", at)
                        at = len(code) if end < 0 else end
                    elif code.startswith("/*", at):
                        at = block_comment(at)
                    else:
                        depth += (code[at] == "{") - (code[at] == "}")
                        at += 1
                continue
            at += 1
        raise ValueError("unterminated quoted literal")

    def block_comment(start):
        depth, at = 1, start + 2
        while at < len(code) and depth:
            if code.startswith("/*", at):
                depth, at = depth + 1, at + 2
            elif code.startswith("*/", at):
                depth, at = depth - 1, at + 2
            else:
                at += 1
        if depth:
            raise ValueError("unterminated block comment")
        return at

    at = 0
    while at < len(code):
        if code[at].isspace():
            at += 1
            continue
        if code.startswith("//", at):
            end = code.find("\n", at)
            end = len(code) if end < 0 else end
            comments.append((code.count("\n", 0, at) + 1, code[at:end]))
            at = end
            continue
        if code.startswith("/*", at):
            at = block_comment(at)
            continue
        if code[at] in "\"'`":
            end = quoted(at)
        else:
            match = WORD_RE.match(code, at) or NUMBER_RE.match(code, at)
            if match:
                end = match.end()
                if match.group() in ("r", "b", "c", "re", "rb", "br") and end < len(code) and code[end] in "\"'":
                    end = quoted(end, "r" in match.group())
            else:
                operator = next((op for op in OPERATORS if code.startswith(op, at)), None)
                end = at + len(operator) if operator else at + 1
        tokens.append(code[at:end])
        spans.append((at, end))
        at = end
    return (tokens, comments, spans) if with_spans else (tokens, comments)


def metadata(block):
    markers = block["markers"]
    modes = [marker for marker in markers if MODE_RE.fullmatch(marker)]
    unknown = [marker for marker in markers if not MODE_RE.fullmatch(marker) and not marker.startswith("@id=")]
    if block.get("id_errors") or unknown or len(modes) != 1:
        return None, "exactly one explicit G13 mode and valid metadata are required"
    mode = modes[0][1:]
    if block.get("output_error"):
        return None, block["output_error"]
    if mode == "run" and block.get("expected_stdout") is None:
        return None, "@run requires an immediately following output fence"
    if mode.startswith("error="):
        try:
            sites = [line for line, comment in lexical_facts(block["code"])[1] if re.match(r"//\s*error(?:\b|:)", comment)]
        except ValueError as error:
            return None, str(error)
        if len(sites) != 1:
            return None, "@error requires exactly one // error line"
        block["expected_error_line"] = sites[0]
    return mode, None


def command_result(command, cwd):
    executable = Path(command[0])
    before = hashlib.sha256(executable.read_bytes()).hexdigest() if executable.is_file() else None
    result = dict(command=[str(arg) for arg in command], cwd=str(cwd), exit=None, stdout="", stderr="", utf8_valid=True,
                  binary_sha256_before=before)
    try:
        proc = subprocess.run(result["command"], cwd=cwd, capture_output=True, timeout=30)
        result["exit"] = proc.returncode
        stdout, stderr = proc.stdout, proc.stderr
    except subprocess.TimeoutExpired as error:
        result["timeout_seconds"] = 30
        stdout, stderr = error.stdout or b"", error.stderr or b""
    except OSError as error:
        result["tool_error"] = str(error)
        stdout, stderr = b"", b""
    for key, raw in (("stdout", stdout), ("stderr", stderr)):
        try:
            result[key] = raw.decode("utf-8").replace("\r\n", "\n")
        except UnicodeDecodeError:
            result["utf8_valid"] = False
            result[key] = raw.decode("utf-8", errors="replace")
            result[key + "_hex"] = raw.hex()
    result["binary_sha256_after"] = hashlib.sha256(executable.read_bytes()).hexdigest() if executable.is_file() else None
    return result


def command_ok(result):
    return result.get("exit") == 0 and result.get("utf8_valid", True) and not result.get("timeout_seconds") and not result.get("tool_error") and result.get("binary_sha256_before") == result.get("binary_sha256_after")


def evaluate_block(block, binary, folder, cwd, run=command_result):
    row = {key: value for key, value in block.items() if key not in ("id_errors", "tagged")}
    row["key"] = snippet_key(block["line"], block["code"])
    row["commands"] = []
    mode, error = metadata(block)
    row["mode"] = mode
    if "expected_error_line" in block:
        row["expected_error_line"] = block["expected_error_line"]
    if error:
        row.update(status="METADATA_FAIL", detail=error)
        return row
    fixture = folder / ("snippet_" + hashlib.sha256((block["source"] + str(block["line"])).encode()).hexdigest()[:16] + ".xr")
    fixture.write_text(block["code"] + "\n", encoding="utf-8")
    row["fixture"] = str(fixture)
    row["fixture_sha256"] = hashlib.sha256(fixture.read_bytes()).hexdigest()

    def invoke(arguments):
        result = run(arguments, cwd)
        row["commands"].append(result)
        return result

    if mode in ("check", "fragment") or mode.startswith("error="):
        checked = invoke([str(binary), "check", *(["-S"] if mode == "fragment" else []), str(fixture)])
        if mode.startswith("error="):
            primary = PRIMARY_ERROR_RE.findall(checked["stderr"] + checked["stdout"])
            row["actual_primary_errors"] = primary
            code = mode.split("=", 1)[1]
            matched = (checked.get("exit") == 1 and checked.get("utf8_valid", True)
                       and not checked.get("timeout_seconds") and not checked.get("tool_error") and len(primary) == 1
                       and (primary[0][3] or primary[0][4]) == code and int(primary[0][1]) == block["expected_error_line"]
                       and os.path.normcase(os.path.abspath(primary[0][0])) == os.path.normcase(os.path.abspath(fixture))
                       and checked.get("binary_sha256_before") == checked.get("binary_sha256_after"))
            row["status"] = "ERROR_PASS" if matched else "ERROR_FAIL"
        else:
            row["status"] = ("PARSE_" if mode == "fragment" else "CHECK_") + ("PASS" if command_ok(checked) else "FAIL")
    else:
        expected = block["expected_stdout"]
        vm = invoke([str(binary), "run", str(fixture)])
        row["vm_pass"] = command_ok(vm) and vm["stdout"] == expected and not vm["stderr"]
        native = folder / (fixture.stem + (".exe" if os.name == "nt" else ".native"))
        built = invoke([str(binary), "build", str(fixture), "-o", str(native)])
        row["native_build_pass"] = command_ok(built) and native.is_file()
        if row["native_build_pass"]:
            executed = invoke([str(native)])
            row["native_pass"] = command_ok(executed) and executed["stdout"] == expected and not executed["stderr"]
        else:
            row["native_pass"] = False
            row["native_execution"] = "NOT_RUN"
        row["status"] = "RUN_PASS" if row["vm_pass"] and row["native_pass"] else "RUN_FAIL"
    failed = next((command for command in row["commands"] if not command_ok(command)), row["commands"][-1])
    detail = (failed["stderr"] or failed["stdout"]).strip().splitlines()
    row["detail"] = detail[0] if detail else "mode result or exact output did not match" if row["status"].endswith("FAIL") else ""
    return row


def load_blocks(path, language):
    paths = sorted(path.glob("*.md")) if path.is_dir() else [path]
    blocks, errors = [], []
    for source in paths:
        text = source.read_text(encoding="utf-8")
        found, invalid = parse_document(text, str(source))
        digest = hashlib.sha256(source.read_bytes()).hexdigest()
        for block in found:
            block["source_sha256"] = digest
            if not block["tagged"] or block["language"] == language:
                blocks.append(block)
        errors.extend(invalid)
    return blocks, errors


def parity(primary, peer):
    rows = []
    if len(primary) != len(peer):
        rows.append(dict(status="PARITY_FAIL", detail=f"fence counts differ: {len(primary)} != {len(peer)}"))
    for left, right in zip(primary, peer):
        lmode, lerror = metadata(left)
        rmode, rerror = metadata(right)
        try:
            same = lexical_facts(left["code"])[0] == lexical_facts(right["code"])[0]
        except ValueError:
            same = False
        valid = (same and not lerror and not rerror and lmode == rmode
                 and left["expected_stdout"] == right["expected_stdout"])
        rows.append(dict(status="PARITY_PASS" if valid else "PARITY_FAIL", source=left["source"], line=left["line"],
                         id=left["id"], peer_id=right["id"], peer_source=right["source"], peer_line=right["line"], detail="" if valid else "code, metadata or exact output differs (or metadata is invalid)"))
    return rows


def support_table(report):
    """Project measured example results without promoting unrun backends."""
    lines = ["# Specification example support", "",
             "This is measured example coverage, not a declaration of complete feature support.", "",
             f"Platform: `{report.get('platform', 'unknown')}`",
             f"Source revision: `{report.get('source_revision') or 'unknown'}`; dirty: `{report.get('source_dirty')}`",
             f"Compiler SHA-256: `{report.get('compiler_sha256') or 'unknown'}`", "",
             f"All examples conform: **{report['conforms']}**; ratchet passes: **{report['gate_pass']}**.", "",
             "| Example | Mode | Result | VM | Native build | Native execution | Baselined failure |",
             "|---|---|---|---|---|---|---|"]
    def stage(row, key):
        return "NOT_RUN" if key not in row else "PASS" if row[key] else "FAIL"
    for row in report["results"]:
        label = row.get("source_key") or row.get("id") or row["key"]
        values = [label, row.get("mode") or "UNSPECIFIED", row["status"],
                  stage(row, "vm_pass"), stage(row, "native_build_pass"),
                  "NOT_RUN" if row.get("native_execution") == "NOT_RUN" else stage(row, "native_pass"),
                  "yes" if row.get("baselined") and row["status"].endswith("FAIL") else "no"]
        lines.append("| " + " | ".join(str(value).replace("|", "\\|") for value in values) + " |")
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xray", required=True, type=Path)
    parser.add_argument("--spec", required=True, type=Path)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--peer-spec", type=Path)
    parser.add_argument("--language", choices=("cn", "en"), default="cn")
    parser.add_argument("--parity-only", action="store_true")
    parser.add_argument("--ids", action="append", default=[], help="comma-separated IDs or basename:body-line selectors; scope never changes modes")
    parser.add_argument("--report", type=Path)
    parser.add_argument("--support-table", type=Path)
    args = parser.parse_args(argv)
    baseline = set()
    if args.baseline.is_file():
        for raw in args.baseline.read_text(encoding="utf-8").splitlines():
            value = raw.split("#", 1)[0].strip()
            if value:
                baseline.add(value.split()[0])
    blocks, errors = load_blocks(args.spec, args.language)
    peer, peer_errors = [], []
    peer_requested = bool(args.peer_spec or args.spec.is_dir() or any(block["tagged"] for block in blocks))
    if peer_requested:
        peer, peer_errors = load_blocks(args.peer_spec or args.spec, "en" if args.language == "cn" else "cn")
    requested = {value for group in args.ids for value in group.split(",") if value}
    if requested:
        def selectors(block):
            shared = "example:" + block["source_key"] if block.get("source_key") else None
            return {value for value in (block["id"], shared, f"{Path(block['source']).name}:{block['line']}") if value}
        found = set().union(*(selectors(block) for block in blocks))
        errors.extend(dict(status="SCOPE_FAIL", id=value, detail="requested fence selector is absent") for value in sorted(requested - found))
        chosen = [(index, block) for index, block in enumerate(blocks) if selectors(block) & requested]
        peer_ids = {block["id"]: block for block in peer if block["id"]}
        # Anonymous fences pair by their existing document order; no fake ID is published.
        peer = [peer_ids[block["id"]] if block["id"] in peer_ids else peer[index]
                for index, block in chosen if block["id"] in peer_ids or index < len(peer)]
        blocks = [block for _, block in chosen]
    if not blocks:
        errors.append(dict(status="SCOPE_FAIL", detail="no xray fences selected"))
    comparisons = parity(blocks, peer) if peer_requested else []
    if args.parity_only and not peer:
        errors.append(dict(status="SCOPE_FAIL", detail="--parity-only requires a peer document"))
    rows = []
    cwd = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="spec_snippets.") as temporary:
        if not args.parity_only:
            for block in blocks:
                rows.append(evaluate_block(block, args.xray.resolve(), Path(temporary), cwd))
    for row in rows:
        row["baselined"] = row["key"] in baseline and row["status"] not in ("METADATA_FAIL", "SCOPE_FAIL")
    failures = [row for row in rows if row["status"].endswith("FAIL")]
    new = [row for row in failures if not row["baselined"]]
    stale = [row for row in rows if row["key"] in baseline and row["status"].endswith("PASS")]
    defects = errors + peer_errors + [row for row in comparisons if row["status"] == "PARITY_FAIL"]
    counts = dict(collections.Counter(row["status"] for row in rows))
    report = dict(spec=str(args.spec), peer_spec=str(args.peer_spec) if args.peer_spec else None,
                  ids=sorted(requested), language=args.language, results=rows, parity=comparisons, metadata_errors=errors + peer_errors,
                  counts=counts, deviations=len(failures), new_deviations=len(new), stale_baseline=[row["key"] for row in stale],
                  conforms=not failures and not defects, gate_pass=not new and not defects)
    report["platform"] = platform.platform()
    report["compiler_sha256"] = hashlib.sha256(args.xray.read_bytes()).hexdigest() if args.xray.is_file() else None
    try:
        revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=cwd, capture_output=True, text=True, timeout=10)
        dirty = subprocess.run(["git", "status", "--porcelain"], cwd=cwd, capture_output=True, text=True, timeout=10)
        report["source_revision"] = revision.stdout.strip() if revision.returncode == 0 else None
        report["source_dirty"] = bool(dirty.stdout) if dirty.returncode == 0 else None
    except (OSError, subprocess.TimeoutExpired):
        report["source_revision"], report["source_dirty"] = None, None
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if args.support_table:
        args.support_table.parent.mkdir(parents=True, exist_ok=True)
        args.support_table.write_text(support_table(report), encoding="utf-8")
    print("spec snippets: " + json.dumps(counts, sort_keys=True) + f"; {len(failures)} actual failures ({len(new)} new); {len(defects)} metadata/parity defects")
    for row in new + defects:
        print(f"  {row['status']} {row.get('source', args.spec)}:{row.get('line', 0)} {row.get('id') or ''}: {row.get('detail', '')}")
    for row in stale:
        print(f"  STALE baseline {row['key']} at {row['source']}:{row['line']} (not rewritten)")
    return 0 if report["gate_pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
