"""Explicit, target-neutral provider declarations and host projections.

Logical facts come from the declaration. A host symbol or an ABI shape never
supplies missing effects, ownership, resource, or concurrency semantics.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass
import hashlib
import json
import re
from typing import Mapping, Sequence


LOGICAL_PROPERTIES = frozenset({
    "provider_parameter_modes", "provider_parameter_owners", "provider_result_owner",
    "provider_error", "provider_panic", "provider_suspend", "provider_refusal",
    "provider_resources", "provider_effects", "provider_threads", "provider_reentry", "provider_callbacks",
    "provider_platforms", "provider_profiles",
})
HOST_PROPERTIES = frozenset({
    "provider_adapter", "provider_host_header", "provider_host_symbol",
})
IDENTITY_PROPERTIES = frozenset({"provider_contract", "provider_operation"})
PROPERTIES = LOGICAL_PROPERTIES | HOST_PROPERTIES | IDENTITY_PROPERTIES
PIPE_RESOURCE = "xray.runtime.resource.v1/pipe-endpoint"


@dataclass(frozen=True)
class LogicalParameter:
    type: str
    mode: str
    owner: str


@dataclass(frozen=True)
class ResourceTransition:
    resource: str
    value: str
    action: str
    when: str


@dataclass(frozen=True)
class ProviderLogicalDeclaration:
    parameters: tuple[LogicalParameter, ...]
    result_type: str
    result_owner: str
    error: str
    panic: str
    suspend: str
    refusal: str
    resources: tuple[ResourceTransition, ...]
    effects: tuple[str, ...]
    threads: str
    reentry: str
    callbacks: str
    platforms: tuple[str, ...]
    profiles: tuple[str, ...]

    def facts(self) -> dict:
        return {"schema": 1, **asdict(self)}

    def declaration_sha256(self) -> str:
        """Fingerprint the source declaration, independently of a host ABI.

        This inventory digest does not certify a compiled binding or replace
        the runtime's validated logical contract identity.
        """
        payload = json.dumps(self.facts(), sort_keys=True, separators=(",", ":"))
        return hashlib.sha256(payload.encode("utf-8")).hexdigest()


@dataclass(frozen=True)
class ProviderHostProjection:
    adapter: str
    header: str
    symbol: str


@dataclass(frozen=True)
class ProviderDeclaration:
    contract: str
    operation: str
    logical: ProviderLogicalDeclaration
    host: ProviderHostProjection


def fail(context: str, message: str):
    raise SystemExit(f"{context}: {message}")


def text_property(props: Mapping[str, object], name: str, context: str) -> str:
    value = props[name]
    if not isinstance(value, str):
        fail(context, f"{name} must be a string")
    return value


def choice(props: Mapping[str, object], suffix: str, choices: set[str], context: str) -> str:
    name = "provider_" + suffix
    value = text_property(props, name, context)
    if value not in choices:
        fail(context, f"{name} has unsupported value {value!r}")
    return value


def words(props: Mapping[str, object], suffix: str, context: str) -> tuple[str, ...]:
    value = text_property(props, "provider_" + suffix, context)
    if not value:
        return ()
    parts = tuple(part.strip() for part in value.split(","))
    if any(not part for part in parts):
        fail(context, f"provider_{suffix} contains an empty item")
    return parts


def choice_set(props: Mapping[str, object], suffix: str, choices: set[str],
               context: str) -> tuple[str, ...]:
    values = words(props, suffix, context)
    if not values or len(set(values)) != len(values) or set(values) - choices:
        fail(context, f"provider_{suffix} must contain distinct supported values")
    return tuple(sorted(values))


def parse_resources(raw: str, parameters: tuple[LogicalParameter, ...], result: str,
                    context: str) -> tuple[ResourceTransition, ...]:
    def unique_object(pairs):
        values = {}
        for key, value in pairs:
            if key in values:
                raise ValueError(f"duplicate resource field {key}")
            values[key] = value
        return values

    try:
        rows = json.loads(raw, object_pairs_hook=unique_object)
    except (TypeError, ValueError) as error:
        fail(context, f"provider_resources must be a JSON array: {error}")
    if not isinstance(rows, list):
        fail(context, "provider_resources must be a JSON array")
    result_rows = []
    seen_values = set()
    for row in rows:
        if not isinstance(row, dict) or set(row) != {"resource", "value", "action", "when"}:
            fail(context, "provider_resources requires resource, value, action and when")
        if not all(isinstance(value, str) for value in row.values()):
            fail(context, "provider_resources fields must be strings")
        if not re.fullmatch(r"xray\.runtime\.resource\.v1/[a-z0-9][a-z0-9-]*", row["resource"]):
            fail(context, "provider_resources has a malformed resource identity")
        if row["value"] in seen_values:
            fail(context, "provider_resources repeats a value")
        seen_values.add(row["value"])
        parameter = re.fullmatch(r"parameter\.([0-9]+)", row["value"])
        if parameter:
            index = int(parameter.group(1))
            if (index >= len(parameters) or parameters[index] != LogicalParameter("i64", "in", "trivial")
                    or row["action"] != "consume" or row["when"] != "call-enter"):
                fail(context, "provider_resources has an invalid parameter token transition")
        elif row["value"] in {"result.some.0", "result.some.1"}:
            if (result != "(i64, i64)?" or row["action"] != "acquire"
                    or row["when"] != "result-present"):
                fail(context, "provider_resources has an invalid result token transition")
        else:
            fail(context, "provider_resources references an unsupported logical value path")
        result_rows.append(ResourceTransition(**row))
    return tuple(sorted(result_rows, key=lambda row: row.value))


def parse_provider_declaration(props: Mapping[str, object], parameter_types: Sequence[str],
                               result_type: str, context: str) -> ProviderDeclaration | None:
    present = {key for key in props if key.startswith("provider_")}
    if not present:
        return None
    if present - PROPERTIES:
        fail(context, "unknown provider properties: " + ", ".join(sorted(present - PROPERTIES)))
    if PROPERTIES - present:
        fail(context, "missing explicit provider facts: " + ", ".join(sorted(PROPERTIES - present)))
    contract = text_property(props, "provider_contract", context)
    operation = text_property(props, "provider_operation", context)
    contract_match = re.fullmatch(r"xray\.runtime\.provider\.v1/([a-z0-9][a-z0-9-]*)", contract)
    operation_match = re.fullmatch(
        r"xray\.runtime\.provider-operation\.v1/([a-z0-9][a-z0-9-]*)/[a-z0-9][a-z0-9-]*",
        operation)
    if not contract_match or not operation_match or contract_match[1] != operation_match[1]:
        fail(context, "malformed or cross-family provider identity")
    modes = words(props, "parameter_modes", context)
    owners = words(props, "parameter_owners", context)
    if len(modes) != len(parameter_types) or len(owners) != len(parameter_types):
        fail(context, "provider parameter facts must describe every logical parameter")
    if set(modes) - {"in", "ref", "out"} or set(owners) - {"trivial", "borrow", "consume"}:
        fail(context, "provider parameter modes or owners are invalid")
    parameters = tuple(LogicalParameter(*row) for row in zip(parameter_types, modes, owners))
    resources = parse_resources(text_property(props, "provider_resources", context),
                                parameters, result_type, context)
    logical = ProviderLogicalDeclaration(
        parameters=parameters, result_type=result_type,
        result_owner=choice(props, "result_owner", {"trivial", "owned", "borrowed"}, context),
        error=choice(props, "error", {"none", "typed"}, context),
        panic=choice(props, "panic", {"none", "may-panic"}, context),
        suspend=choice(props, "suspend", {"never", "may-suspend"}, context),
        refusal=choice(props, "refusal", {"trap"}, context),
        resources=resources,
        effects=choice_set(props, "effects", {
            "none", "reads-clock", "reads-process", "reads-environment", "io",
            "managed-allocation", "managed-deallocation"}, context),
        threads=choice(props, "threads", {"any", "instance-affine"}, context),
        reentry=choice(props, "reentry", {"allowed", "forbidden"}, context),
        callbacks=choice(props, "callbacks", {"none", "synchronous"}, context),
        platforms=choice_set(props, "platforms", {"linux", "macos", "windows"}, context),
        profiles=choice_set(props, "profiles", {"hosted", "freestanding"}, context),
    )
    if "none" in logical.effects and len(logical.effects) != 1:
        fail(context, "provider_effects cannot combine none with an observable effect")
    host = ProviderHostProjection(
        adapter=text_property(props, "provider_adapter", context),
        header=text_property(props, "provider_host_header", context),
        symbol=text_property(props, "provider_host_symbol", context),
    )
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", host.symbol):
        fail(context, "provider_host_symbol must name a C function")
    if (not re.fullmatch(r"[A-Za-z0-9_/-]+\.h", host.header)
            or host.header.startswith("/") or ".." in host.header.split("/")):
        fail(context, "provider_host_header must be a source-relative C header")
    if not re.fullmatch(r"[a-z][a-z0-9-]*", host.adapter):
        fail(context, "provider_adapter must name an explicit projection")
    return ProviderDeclaration(contract, operation, logical, host)


def admission_reasons(declaration: ProviderDeclaration) -> tuple[str, ...]:
    """Report unsupported declarations without dropping them from inventory."""
    logical, host = declaration.logical, declaration.host
    reasons = []
    if (logical.error != "none" or logical.panic != "none" or logical.suspend != "never"
            or logical.callbacks != "none"):
        reasons.append("typed-error-panic-suspend-or-callback-binding-not-implemented")
    if logical.result_owner != "trivial" or any(
            parameter.mode != "in" or parameter.owner != "trivial"
            for parameter in logical.parameters):
        reasons.append("managed-or-reference-parameter-binding-not-implemented")
    if logical.threads != "any" or logical.reentry != "allowed":
        reasons.append("instance-affine-or-nonreentrant-binding-not-implemented")
    if logical.profiles != ("hosted",):
        reasons.append("non-hosted-leaf-binding-not-implemented")
    shapes = {
        "i64-nullary-u64": ((), "i64"),
        "i64-nullary-i64": ((), "i64"),
        "i64-unary-status-out": (("i64",), "i64"),
        "optional-i64-pair-pipe-create": ((), "(i64, i64)?"),
        "bool-i64-pipe-close": (("i64",), "bool"),
    }
    actual = (tuple(parameter.type for parameter in logical.parameters), logical.result_type)
    if host.adapter not in shapes:
        reasons.append("host-adapter-not-implemented")
    elif actual != shapes[host.adapter]:
        reasons.append("declared-signature-does-not-match-explicit-host-adapter")
    expected_resources = ()
    if host.adapter == "optional-i64-pair-pipe-create":
        expected_resources = tuple(ResourceTransition(
            PIPE_RESOURCE, f"result.some.{index}", "acquire", "result-present") for index in (0, 1))
    elif host.adapter == "bool-i64-pipe-close":
        expected_resources = (ResourceTransition(
            PIPE_RESOURCE, "parameter.0", "consume", "call-enter"),)
    if logical.resources != expected_resources:
        reasons.append("declared-resources-do-not-match-explicit-host-adapter")
    return tuple(reasons)


def declaration_inventory(entries: Sequence[object]) -> dict:
    rows = []
    identities = set()
    for entry in entries:
        if not entry.is_internal or not entry.name.startswith("__"):
            continue
        declaration = entry.provider_declaration
        if declaration:
            identity = (declaration.contract, declaration.operation)
            if identity in identities:
                fail("provider inventory", "duplicate provider operation identity")
            identities.add(identity)
        reasons = admission_reasons(declaration) if declaration else ("missing-provider-declaration",)
        rows.append({
            "symbol": entry.symbol,
            "signature": entry.signature,
            "declared": declaration is not None,
            "admitted": declaration is not None and not reasons,
            "blocking_reasons": reasons,
            "declaration": asdict(declaration) if declaration else None,
            "logical_declaration_sha256": declaration.logical.declaration_sha256()
                if declaration else None,
            "binding": {"state": "NOT_VERIFIED", "reason": "requires-compiled-runtime-evidence"},
            "testing": {"state": "NOT_VERIFIED", "reason": "requires-build-bound-program-evidence"},
        })
    rows.sort(key=lambda row: row["symbol"])
    symbols = [row["symbol"] for row in rows]
    if len(set(symbols)) != len(symbols):
        fail("provider inventory", "duplicate private leaf identity")
    return {"schema": 1,
            "evidence_scope": "generated-provider-path; binding and tests require build-bound evidence",
            "leaf_count": len(rows), "leaves": rows}
