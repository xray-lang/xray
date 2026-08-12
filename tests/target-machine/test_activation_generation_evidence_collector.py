#!/usr/bin/env python3
"""Mutation tests for activation/generation raw evidence evaluation."""

from __future__ import annotations

import importlib.util
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "activation_producer",
    ROOT / "scripts/collect_target_machine_activation_generation_evidence.py",
)
assert SPEC is not None and SPEC.loader is not None
producer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(producer)


def main() -> int:
    routes = [
        "source-to-xsm", "xsm-to-xtp", "xtp-to-vm", "target-plan-to-native",
        "runtime-only-embed", "hosted-fragment", "generation-lifecycle",
    ]
    identities = {name: chr(97 + index) * 64 for index, name in enumerate(
        ("artifact", "generation", "semantic", "target")
    )}
    clean_codes = {name: 0 for name in ("generation", "archive", "xtp", "cli")}
    payload, passed = producer.evaluate(clean_codes, identities, 0, routes)
    if passed or set(payload["missing_artifact_routes"]) != {
        "hosted-fragment", "target-plan-to-native"
    }:
        raise AssertionError("unproven routes must keep real collection failed")
    complete_payload, complete_passed = producer.evaluate(
        clean_codes, identities, 0,
        [route for route in routes if route not in
         ("hosted-fragment", "target-plan-to-native")],
    )
    if not complete_passed or complete_payload["activation_before_verify"] != 0:
        raise AssertionError("complete exact fixture did not pass")
    mutations = 0
    for name in clean_codes:
        changed = dict(clean_codes)
        changed[name] = 1
        if producer.evaluate(changed, identities, 0, complete_payload["artifact_routes"])[1]:
            raise AssertionError(f"failed lane {name} was accepted")
        mutations += 1
    if producer.evaluate(clean_codes, None, 0, complete_payload["artifact_routes"])[1]:
        raise AssertionError("missing identities were accepted")
    mutations += 1
    if producer.evaluate(clean_codes, identities, 1,
                         complete_payload["artifact_routes"])[1]:
        raise AssertionError("runtime compiler symbols were accepted")
    mutations += 1
    print(f"target-machine activation evidence self-test: PASS ({mutations} mutations)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
