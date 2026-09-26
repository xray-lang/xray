"""Run exact enum typed-catch outputs independently on VM and native AOT."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def checked(command, folder):
    result = subprocess.run(command, cwd=folder, capture_output=True, timeout=120)
    if result.returncode:
        raise RuntimeError(f"{command!r}: exit {result.returncode}\n" +
                           (result.stdout + result.stderr).decode("utf-8", errors="replace"))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    args = parser.parse_args()
    compiler = str(args.compiler.resolve(strict=True))
    cases = {
        "builtin": ('fn fail() { throw CryptoError.InvalidLength }\n'
                    'try { fail() } catch (e: CryptoError) { print("builtin") }\n'),
        "source": ('enum ProbeError { Bad }\nfn fail() { throw ProbeError.Bad }\n'
                   'try { fail() } catch (e: ProbeError) { print("source") }\n'),
    }
    with tempfile.TemporaryDirectory(prefix="xray-enum-execution-") as temporary:
        folder = Path(temporary)
        (folder / "xray.toml").write_text(
            '[project]\nname="enum-execution"\nmain="main.xr"\n', encoding="utf-8")
        for name, source in cases.items():
            (folder / "main.xr").write_text(source, encoding="utf-8")
            expected = (name + "\n").encode()
            vm = checked([compiler, "run", "main.xr"], folder)
            output = folder / (name + (".exe" if os.name == "nt" else ""))
            checked([compiler, "build", "--native", "main.xr", "-o", str(output)], folder)
            native = checked([str(output)], folder)
            for backend, result in (("VM", vm), ("native", native)):
                if result.stdout.replace(b"\r\n", b"\n") != expected or result.stderr:
                    raise RuntimeError(f"{name} {backend}: unexpected output "
                                       f"{result.stdout!r} {result.stderr!r}")
                print(f"{name} {backend}: exact output PASS", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
