"""Real native transactions; injected gates use the same owner implementation."""
from pathlib import Path
import hashlib, os, re, shutil, subprocess, sys, tempfile, time

exe, root, compiler, linker, sdk = map(lambda value: Path(value).resolve(), sys.argv[1:6])
faults = sys.argv[6:] == ['faults']
profile = sys.argv[6:] == ['profile']
preflight = sys.argv[6:] == ['preflight']
vc = Path(os.environ['VCToolsInstallDir']) / 'lib/x64'
kit = Path(os.environ['WindowsSdkDir']) / 'Lib' / os.environ['WindowsSDKVersion'].strip('/\\')
libraries = [vc / name for name in ('msvcrt.lib', 'oldnames.lib', 'vcruntime.lib')]
libraries += [kit / 'ucrt/x64/ucrt.lib', kit / 'um/x64/kernel32.Lib']
assert all(path.is_file() for path in libraries)
def run(mode):
    with tempfile.TemporaryDirectory(prefix='xray-native-invocation-') as temporary:
        directory = Path(temporary)
        source = directory / 'entry.xr'; source.write_text('print("native-invocation-ok")\n', encoding='utf-8')
        authority = directory
        if mode == 'full-program':
            authority = root / 'tests/fixtures/xir_compile_owner'
            source = authority / 'root.xr'
            for name in ('root.xr', 'counter.xr', 'label.xr'):
                print(f'actual fixture {name} sha256={hashlib.sha256((authority / name).read_bytes()).hexdigest()}', flush=True)
        inputs = directory / 'input'; inputs.mkdir()
        output = directory / 'input-output'; output.mkdir()
        launcher = root / 'tests/unit/xir/xir_sdk_consumer.c'
        if mode == 'profile-header':
            header = directory / 'outside.h'; header.write_text('/* actual out-of-root include */\n', encoding='utf-8')
            private_launcher = directory / 'outside-launcher.c'
            private_launcher.write_bytes(launcher.read_bytes() + ('\n#include "' + header.as_posix() + '"\n').encode('utf-8'))
            launcher = private_launcher
        selected_compiler, selected_linker = compiler, linker
        if mode in ('profile-config', 'profile-link-config', 'profile-local', 'profile-manifest', 'profile-local-directory'):
            provider = directory / 'provider'; provider.mkdir()
            for original in (compiler, linker):
                shutil.copy2(original, provider / original.name)
                shutil.copy2(str(original) + '.config', provider / (original.name + '.config'))
            selected_compiler, selected_linker = provider / compiler.name, provider / linker.name
            if mode in ('profile-config', 'profile-link-config'):
                config = Path(str(selected_linker if mode == 'profile-link-config' else selected_compiler) + '.config')
                original = config.read_bytes(); assert len(original) == 409
                config.write_bytes(original.replace(b'apply="no"', b'apply="NO"'))
            elif mode == 'profile-local-directory':
                Path(str(selected_compiler) + '.local').mkdir()
            else:
                Path(str(selected_compiler) + ('.manifest' if mode == 'profile-manifest' else '.local')).write_bytes(b'redirect')
        command = [exe, authority, source, root / 'stdlib', sdk, selected_compiler, selected_linker, inputs,
                   launcher, *libraries, output, mode,
                   Path(os.environ['VCToolsInstallDir']) / 'include',
                   Path(os.environ['WindowsSdkDir']) / 'Include' / os.environ['WindowsSDKVersion'].strip('/\\') / 'ucrt',
                   Path(os.environ['WindowsSdkDir']) / 'Include' / os.environ['WindowsSDKVersion'].strip('/\\') / 'shared',
                   Path(os.environ['WindowsSdkDir']) / 'Include' / os.environ['WindowsSDKVersion'].strip('/\\') / 'um',
                   os.environ['SystemRoot']]
        started = time.perf_counter()
        result = subprocess.run(list(map(str, command)), stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=600)
        sys.stdout.buffer.write(result.stdout); sys.stderr.buffer.write(result.stderr)
        print(f'case={mode} wall={time.perf_counter()-started:.3f}s', flush=True)
        assert result.returncode == 0, (mode, result.returncode)
        if mode in ('positive', 'full-program') or mode.startswith('headroom-'):
            native = subprocess.run([str(output / 'program.exe')], capture_output=True, timeout=30)
            expected = b'owner\xe4\xb8\xad-ok 42 true\r\n' if mode == 'full-program' else b'native-invocation-ok\r\n'
            assert native.returncode == 0 and native.stdout == expected and native.stderr == b'', (native.returncode, native.stdout, native.stderr)
        return result.stdout.decode('utf-8', 'replace')

positive = '' if preflight else run('positive')
if not faults and not profile and not preflight:
    run('full-program')
if profile or preflight:
    run('constructor-admission')
    for mode in ['foreign-sdk', 'foreign-0', 'foreign-1', 'foreign-2', 'occupied-out']:
        run(mode)
    measured = run('constructor-measure')
    total_bytes, peak, work = map(int, re.search(r'constructor costs allocated=(\d+) peak=(\d+) work=(\d+)', measured).groups())
    for axis, exact in [('bytes', total_bytes), ('live', peak), ('work', work)]:
        run(f'constructor-{axis}-{exact}-pass')
        run(f'constructor-{axis}-{exact - 1}-fail')
    for mode in ['profile-completion-legacy', 'profile-legacy', 'profile-env-extra', 'profile-temp', 'profile-system-root', 'profile-include',
                 'profile-config', 'profile-link-config',
                 'profile-local', 'profile-manifest', 'profile-local-directory', 'profile-header', 'profile-image']:
        if not preflight or mode not in ('profile-header', 'profile-image'):
            run(mode)
if faults:
    allocations, comparisons, runs = map(int, re.search(
        r'owner direct allocations=(\d+) comparisons=(\d+) attempted-runs=(\d+)', positive).groups())
    assert runs == 6 and allocations and comparisons
    for mode in ['foreign-sdk', 'foreign-0', 'foreign-1', 'foreign-2', 'occupied-out', 'bad-recipe', 'nonempty',
                 'same-directory', 'case-directory', 'ancestor-directory', 'reverse-ancestor', 'source-shape', 'launcher-shape',
                 'launcher-race', 'source-race', 'read-oom', 'cancel', 'timeout', 'child', 'replay',
                 'includes-replay', 'link-replay', 'image-replay']:
        run(mode)
    for index in range(allocations):
        run(f'oom-{index}')
    for index in range(6):
        run(f'process-oom-{index}')
    for axis, cuts in [('work', [50000000, 250000000, 500000000, 650000000]),
                       ('bytes', [10000000, 30000000, 50000000, 60000000]), ('live', [5000000, 15000000, 23000000])]:
        for cut in cuts:
            run(f'budget-{axis}-{cut}')
    for mode in ['headroom-work-800000000', 'headroom-bytes-70000000', 'headroom-live-28000000']:
        run(mode)
    run(f'io-all-{comparisons}')
print('pre-execution profile gates; no provider execution' if preflight else
      'actual MSVC Source/SDK six calls; positive leases and exact native output; no full Target authority', flush=True)
