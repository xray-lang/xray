"""Test-support: put tests/lib on sys.path so `import xraytest` resolves.

The unittest files live in tests/lib/tests; the package is in tests/lib. This
adds the parent once, mirroring what a real runner's bootstrap() does, without
importing the package (which would fix the path prematurely).
"""

import importlib.util
import sys
from pathlib import Path


def bootstrap_xraytest() -> None:
    lib_dir = str(Path(__file__).resolve().parent.parent)
    if lib_dir not in sys.path:
        sys.path.insert(0, lib_dir)


def load_module(name: str, path: Path):
    """Import a runner script by path so its internals can be unit tested.

    Registering in sys.modules before exec is required, not optional: dataclass
    resolves field types through sys.modules[cls.__module__], and a module that
    is missing there fails with an opaque AttributeError on None.
    """
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module
