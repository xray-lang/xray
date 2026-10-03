"""Run the actual native command, local discovery, admission and publisher."""
from pathlib import Path
import argparse
import ctypes
import hashlib
import json
import os
import shutil
import subprocess
import time
import traceback


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('root', 'driver', 'output', 'cc', 'fault-driver'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    root, driver, cc = (p.resolve(strict=True) for p in (args.root, args.driver, args.cc))
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    temporary = output/'private-temp'
    temporary.mkdir(exist_ok=True)
    assert not list(temporary.iterdir()), 'Preserve and inspect earlier cleanup failure before retry'
    environment = {k: v for k, v in os.environ.items() if k.upper() not in (
        'INCLUDE', 'LIB', 'LIBPATH', 'VCTOOLSINSTALLDIR', 'WINDOWSSDKDIR', 'WINDOWSSDKVERSION')
        and not k.upper().startswith(('VSCMD_', '__VSCMD_'))}
    environment.update(XRAY_STDLIB_PATH=str(root/'stdlib'), NO_COLOR='1', TEMP=str(temporary), TMP=str(temporary))
    record = dict(status='RUNNING', commands=[], coverage='Actual cmd_build parser/consumer; automatic trusted-local '
                  'MSVC discovery, SDK, six-run Operation, actual admission and atomic publication. '
                  'Does not exercise the complete xray top-level dispatcher or qualify unsanitized SDK payload.')

    def save():
        (output/'record.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')

    def run(name, command, code=0, expected=None, stderr=b'', stdout_handle=None,
            cleanup=True, failure=None):
        command = list(map(str, command))
        item = dict(name=name, command=command, expected_exit=code,
                    expected_stdout=None if expected is None else expected.hex())
        record['commands'].append(item)
        save()
        started = time.perf_counter()
        try:
            child_environment = dict(environment)
            if failure:
                child_environment['XR_NATIVE_CLEANUP_TEST_FAILURE'] = failure
            result = subprocess.run(command, cwd=output, env=child_environment, timeout=180,
                                    stdout=stdout_handle or subprocess.PIPE, stderr=subprocess.PIPE)
        except subprocess.TimeoutExpired as error:
            (output/(name+'.stdout')).write_bytes(error.stdout or b'')
            (output/(name+'.stderr')).write_bytes(error.stderr or b'')
            item.update(status='TIMEOUT', seconds=time.perf_counter()-started)
            save()
            raise
        (output/(name+'.stdout')).write_bytes(result.stdout or b'')
        (output/(name+'.stderr')).write_bytes(result.stderr)
        item.update(returncode=result.returncode, seconds=time.perf_counter()-started)
        okay = result.returncode == code and (expected is None or result.stdout == expected)
        okay = okay and (stderr is None or result.stderr == stderr)
        item['status'] = 'PASS' if okay else 'FAIL'
        save()
        assert okay, (name, result.returncode, result.stdout, result.stderr)
        if cleanup:
            assert not list(temporary.iterdir()), (name, 'native workspace was not reclaimed')
        assert not list(output.glob('.xray-*.tmp')), (name, 'publication was not reclaimed')
        return result

    try:
        multi = root/'tests/fixtures/xir_compile_owner/root.xr'
        expected = b'owner\xe4\xb8\xad-ok 42 true\n'
        executable = output/'app.exe'
        run('automatic-default', [driver, multi], expected=b'Built: app.exe\n')
        run('multi-native', [executable], expected=expected)
        original = executable.read_bytes()
        run('explicit-missing-compiler', [driver, '--cc', output/'absent-cl.exe', multi], code=1, stderr=None)
        assert executable.read_bytes() == original
        run('unsupported-provider', [driver, '--toolchain', 'zig', multi], code=1, stderr=None)
        assert executable.read_bytes() == original
        bad = output/'bad.xr'
        bad.write_text('print(missing_symbol)\n', encoding='utf-8')
        run('rejected-source', [driver, bad], code=1, stderr=None)
        assert executable.read_bytes() == original
        named = output/'输出 空格.exe'
        run('explicit-compiler', [driver, '--cc', cc, '--toolchain', 'msvc', '--native', multi, '-o', named],
            expected=('Built: '+str(named)+'\n').encode('utf-8'))
        run('explicit-native', [named], expected=expected)
        source = output/'字符串.xr'
        source.write_text('print("before中\\u{0}'+'x'*6147+'after")\n', encoding='utf-8')
        string_expected = b'before\xe4\xb8\xad\x00'+b'x'*6147+b'after\n'
        run('overwrite-native', [driver, source, '-o', named],
            expected=('Built: '+str(named)+'\n').encode('utf-8'))
        run('owned-string-native', [named], expected=string_expected)
        preserved = named.read_bytes()
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                                      ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
        kernel.CreateFileW.restype = ctypes.c_void_p
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        handle = kernel.CreateFileW(str(named), 0x80000000, 1, None, 3, 0, None)
        assert handle != ctypes.c_void_p(-1).value
        try:
            run('publish-share-denied', [driver, multi, '-o', named], code=1, stderr=None)
        finally:
            assert kernel.CloseHandle(handle)
        assert named.read_bytes() == preserved
        readonly = output/'readonly.stdout'
        readonly.write_bytes(b'untouched')
        with readonly.open('rb') as stream:
            result = run('report-after-publish', [driver, multi, '-o', named], code=1,
                         stderr=None, stdout_handle=stream)
        assert b'published=1' in result.stderr and readonly.read_bytes() == b'untouched'
        run('reported-failure-native', [named], expected=expected)
        fault_driver = args.fault_driver.resolve(strict=True)
        run('cleanup-pending-progress', [fault_driver, multi, '-o', named], failure='pending',
            expected=('Built: '+str(named)+'\n').encode('utf-8'))
        preserved = named.read_bytes()
        result = run('cleanup-recovered-no-publish', [fault_driver, source, '-o', named],
                     code=1, stderr=None, failure='recovered')
        assert b'cleanup recovered' in result.stderr and b'published=0' in result.stderr
        assert named.read_bytes() == preserved
        result = run('cleanup-terminal-no-publish', [fault_driver, source, '-o', named],
                     code=4, stderr=None, failure='terminal', cleanup=False)
        assert b'cleanup_pending=1 published=0' in result.stderr
        assert named.read_bytes() == preserved
        retained = list(temporary.iterdir())
        assert len(retained) == 1 and retained[0].is_dir(), retained
        record['terminal_cleanup'] = dict(owner_completed=False, process_exit=4,
            retained_files=[dict(path=str(p.relative_to(temporary)), bytes=p.stat().st_size,
                                sha256=hashlib.sha256(p.read_bytes()).hexdigest())
                            for p in retained[0].rglob('*') if p.is_file()])
        save()
        # Terminal process exit intentionally left the private owner unfinished.
        # This fixture cleanup is not evidence that the command reclaimed it.
        directory = retained[0].resolve(strict=True)
        assert temporary.resolve(strict=True) == output/'private-temp'
        assert directory.is_relative_to(temporary) and directory.parent == temporary
        assert all(p.resolve(strict=True).is_relative_to(directory) for p in directory.rglob('*'))
        shutil.rmtree(directory)
        assert not list(temporary.iterdir())
        record['final_output'] = dict(path=str(named), bytes=named.stat().st_size,
                                     sha256=hashlib.sha256(named.read_bytes()).hexdigest())
        record['status'] = 'PASS'
        save()
        print('Actual native build: automatic/explicit toolchain, independent multi-module and UTF8/NUL/long '
              'byte oracles, rejection preserves old output, publication failure and complete workspace cleanup PASS')
    except Exception:
        record.update(status='FAIL', failure=traceback.format_exc())
        save()
        raise


if __name__ == '__main__':
    main()
