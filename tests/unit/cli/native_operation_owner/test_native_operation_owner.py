"""One real operation, or explicitly non-executing owner/resource gates."""
from pathlib import Path
import hashlib, json, os, subprocess, sys, tempfile, time

exe, root, compiler, linker, sdk = map(lambda x: Path(x).resolve(), sys.argv[1:6])
mode = sys.argv[6]
with tempfile.TemporaryDirectory(prefix='xray-native-operation-') as temporary:
    parent = Path(temporary)
    marker = parent / 'unrelated.txt'
    marker.write_bytes(b'preserve unrelated workspace-parent entry')
    if mode == 'unit':
        command = [exe, mode, parent]
    else:
        authority = root / 'tests/fixtures/xir_compile_owner'
        source = authority / 'root.xr'
        vc = Path(os.environ['VCToolsInstallDir'])
        kit = Path(os.environ['WindowsSdkDir'])
        version = os.environ['WindowsSDKVersion'].strip('/\\')
        libraries = [vc / 'lib/x64' / name for name in ('msvcrt.lib', 'oldnames.lib', 'vcruntime.lib')]
        libraries += [kit / 'Lib' / version / 'ucrt/x64/ucrt.lib', kit / 'Lib' / version / 'um/x64/kernel32.Lib']
        command = [exe, mode, authority, source, root / 'stdlib', sdk, compiler, linker, parent, *libraries,
                   vc / 'include', kit / 'Include' / version / 'ucrt', kit / 'Include' / version / 'shared',
                   kit / 'Include' / version / 'um', os.environ['SystemRoot'], parent / 'export.exe']
        for path in [source, authority / 'counter.xr', authority / 'label.xr', sdk / 'src/execution/xr_xir_native_main.inc.c']:
            print(f'input {path} sha256={hashlib.sha256(path.read_bytes()).hexdigest()}', flush=True)
    started = time.perf_counter()
    print('actual argv=' + json.dumps(list(map(str, command)), ensure_ascii=False), flush=True)
    result = subprocess.run(list(map(str, command)), capture_output=True, timeout=240)
    sys.stdout.buffer.write(result.stdout); sys.stderr.buffer.write(result.stderr)
    print(f'case={mode} return={result.returncode} seconds={time.perf_counter()-started:.3f}', flush=True)
    assert marker.read_bytes() == b'preserve unrelated workspace-parent entry'
    assert not any(p.name.startswith('xray-') for p in parent.iterdir()), list(parent.iterdir())
    if result.returncode:
        # Natural BROKEN stays a failed transaction; no retries or acceptance.
        raise SystemExit(result.returncode)
    if mode == 'native':
        native = subprocess.run([str(parent / 'export.exe')], capture_output=True, timeout=30)
        assert (native.returncode, native.stdout, native.stderr) == (0, b'owner\xe4\xb8\xad-ok 42 true\n', b''), native
        print('actual SDK launcher + complete 3-module output PASS', flush=True)
