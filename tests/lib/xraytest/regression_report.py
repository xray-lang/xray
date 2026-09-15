"""Stable regression case identities and exact structured-result validation."""

from __future__ import annotations

from pathlib import PurePosixPath

SCHEMA_VERSION = 2
OUTCOMES = ("PASS", "FAIL", "CRASH", "TIMEOUT", "SKIP", "NOT_RUN")
FAILED_OUTCOMES = frozenset(("FAIL", "CRASH", "TIMEOUT"))


def validate(payload: dict, expected: set[str]) -> dict[str, str]:
    """Reject missing, duplicate, foreign, or malformed results before ratcheting.

    A case absent from the report is not evidence of success. The caller
    supplies the independently discovered case set for this run.
    """
    if payload.get("schema_version") != SCHEMA_VERSION:
        raise ValueError("unsupported regression report schema")
    manifest = payload.get("manifest")
    results = payload.get("results")
    if not isinstance(manifest, list) or not isinstance(results, list):
        raise ValueError("regression report has no manifest or results")
    identities = []
    for row in manifest:
        identity = row.get("case_id") if isinstance(row, dict) else None
        if (not isinstance(identity, str) or
                not identity.startswith("tests/regression/") or
                not identity.endswith(".xr") or
                ".." in PurePosixPath(identity).parts or
                str(PurePosixPath(identity)) != identity):
            raise ValueError(f"invalid regression case identity: {identity!r}")
        identities.append(identity)
    if len(set(identities)) != len(identities) or set(identities) != expected:
        raise ValueError("regression manifest differs from discovered case set")
    outcomes = {}
    for row in results:
        if not isinstance(row, dict):
            raise ValueError("malformed regression result")
        identity, outcome = row.get("case_id"), row.get("outcome")
        if identity not in expected or identity in outcomes or outcome not in OUTCOMES:
            raise ValueError(f"invalid or duplicate regression result: {identity!r}")
        outcomes[identity] = outcome
    if set(outcomes) != expected:
        raise ValueError("regression results omit discovered cases")
    if payload.get("counts") != {
        outcome: sum(value == outcome for value in outcomes.values())
        for outcome in OUTCOMES
    }:
        raise ValueError("regression result counts disagree with outcomes")
    return outcomes
