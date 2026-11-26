"""Select the registered product gates for the host that produced a receipt."""

import json


def gate_tests(content, system, machine, execution64):
    groups = json.loads(content)
    if (not isinstance(groups, dict) or set(groups) != {"common", "execution64", "native", "windows"}
            or any(not isinstance(group, list) for group in groups.values())):
        raise ValueError("Invalid current gate inventory")
    names = [name for group in groups.values() for name in group]
    if any(not isinstance(name, str) or not name for name in names) or len(names) != len(set(names)):
        raise ValueError("Invalid or duplicate current gate name")
    selected = list(groups["common"])
    if execution64:
        selected.extend(groups["execution64"])
        x64 = machine in {"amd64", "x86_64"}
        if ((system == "windows" and x64) or
                (system in {"linux", "macos"} and (x64 or machine in {"arm64", "aarch64"}))):
            selected.extend(groups["native"])
    if system == "windows":
        selected.extend(groups["windows"])
    return selected
