"""Check bounded native instruction dispatch using the public C-only producer."""

import argparse
import re
import subprocess
import tempfile
from pathlib import Path


FIXTURES = Path(__file__).resolve().parent.parent / "fixtures" / "xir_language"
FUNCTION = re.compile(
    r"^(?P<header>(?:static XR_NOINLINE|XR_FUNC) XrXirAction "
    r"(?P<name>[A-Za-z_][A-Za-z_0-9]*)\([^\n]*\) \{)\n"
    r"(?P<body>.*?)^}\n", re.MULTILINE | re.DOTALL)
HELPER = re.compile(r"(?P<prefix>[A-Za-z_][A-Za-z_0-9]*)_step_(?P<function>[0-9]+)_(?P<chunk>[0-9]+)$")


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def check_source(source):
    functions = {match["name"]: match for match in FUNCTION.finditer(source)}
    groups = {}
    for name, function in functions.items():
        identity = HELPER.fullmatch(name)
        if not identity:
            continue
        prefix, function_id, chunk_id = identity["prefix"], int(identity["function"]), int(identity["chunk"])
        require(function["header"] ==
                f"static XR_NOINLINE XrXirAction {name}(XrXirCallView *view, {prefix}_state_{function_id} *state) {{",
                f"{name}: helper must borrow the shared activation state and prevent reinlining")
        body = function["body"]
        require("switch (state->pc)" in body, f"{name}: helper lost instruction dispatch")
        cases = [int(value) for value in re.findall(r"^    case ([0-9]+)u:", body, re.MULTILINE)]
        require(0 < len(cases) <= 32, f"{name}: instruction dispatch is not bounded to 32 cases")
        require(cases == list(range(chunk_id * 32, chunk_id * 32 + len(cases))),
                f"{name}: duplicate, reordered, or missing instruction identity")
        for case in cases:
            require(f"case {case}u:\n        state->pc = {case + 1}u;" in body,
                    f"{name}: instruction entry lost its original next PC")
        for forbidden in ("state->initialized", "state->waiting = false", "view->phase == XR_XIR_CALL_EXIT"):
            require(forbidden not in body, f"{name}: helper repeated function-entry processing")
        for label in ("invalid", "limit"):
            declared = f"\n{label}:\n" in body
            references = body.count(f"goto {label};")
            require(declared == (references > 0), f"{name}: missing or unreferenced {label} label")
            if label == "invalid" and declared:
                require(references >= 2, f"{name}: invalid label needs an instruction fault edge")
        groups.setdefault((prefix, function_id), {})[chunk_id] = cases
    require(groups, "large programs must use bounded native instruction helpers")
    for (prefix, function_id), chunks in groups.items():
        expected_ids = list(range(len(chunks)))
        require(sorted(chunks) == expected_ids, "chunk identities must be contiguous")
        for chunk_id in expected_ids[:-1]:
            require(len(chunks[chunk_id]) == 32, "only the final chunk may be partial")
        entry_name = f"{prefix}_f{function_id}"
        require(entry_name in functions, f"{entry_name}: helper has no official call entry")
        body = functions[entry_name]["body"]
        require("switch (state->pc / 32u)" in body, f"{entry_name}: missing bounded dispatch")
        actual = re.findall(r"^    case ([0-9]+)u: return ([A-Za-z_][A-Za-z_0-9]*)\(view, state\);$",
                            body, re.MULTILINE)
        require(actual == [(str(chunk_id), f"{prefix}_step_{function_id}_{chunk_id}") for chunk_id in expected_ids],
                f"{entry_name}: official entry must dispatch every helper exactly once")
        require(body.count("if (!state->initialized)") == 1,
                f"{entry_name}: activation initialization must run at the official entry")
        require(body.count("if (state->waiting)") == 1,
                f"{entry_name}: inbox admission must run at the official entry")
    return len(groups), sum(len(chunks) for chunks in groups.values())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--xray", type=Path, required=True)
    arguments = parser.parse_args()
    executable = arguments.xray.resolve()
    with tempfile.TemporaryDirectory(prefix="xray-resume-shape-") as directory:
        fixtures = [(name, FIXTURES / f"{name}.xr")
                    for name in ("array_callbacks", "array_queries", "array_callback_timer")]
        fixtures.append(("cleanup", FIXTURES.parent / "xir_cleanup_source" / "root.xr"))
        for name, fixture in fixtures:
            output = Path(directory) / f"{name}.c"
            result = subprocess.run([str(executable), "build", str(fixture),
                                     "--c-only", "-o", str(output)], capture_output=True, timeout=240)
            require(result.returncode == 0, result.stderr.decode(errors="replace"))
            groups, helpers = check_source(output.read_text(encoding="utf-8"))
            print(f"{name}: {groups} bounded functions, {helpers} helpers; instruction identities preserved")


if __name__ == "__main__":
    main()
