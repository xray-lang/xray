"""Backend selection used by the differential runner and the AOT sweep."""

from dataclasses import dataclass
import json
from pathlib import Path


BACKEND_ORDER = ("vm", "aot")


@dataclass(frozen=True)
class BackendPolicy:
    backends: tuple[str, ...] = ()
    aot_reject: str = ""


_DEFAULT = BackendPolicy()


def read_policy(case: Path) -> BackendPolicy:
    """Read test metadata without changing or interpreting the program bytes."""
    path = case.with_suffix(".xr.backends.json")
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return _DEFAULT
    if not isinstance(data, dict) or data.keys() - {"backends", "aot_reject"}:
        raise ValueError(f"{path}: invalid backend policy fields")
    backends = data.get("backends")
    if (not isinstance(backends, list) or not backends or
            any(name not in BACKEND_ORDER for name in backends) or len(backends) != len(set(backends))):
        raise ValueError(f"{path}: expected a nonempty list of distinct backends")
    aot_reject = data.get("aot_reject", "")
    if not isinstance(aot_reject, str) or (aot_reject and backends != ["vm"]):
        raise ValueError(f"{path}: AOT rejection requires a VM-only policy")
    return BackendPolicy(tuple(backends), aot_reject)
