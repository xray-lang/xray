"""Real CLI deps, with independent exact report bytes and fail-before-file errors."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
from run_source_dependencies import fixture, expected_reports

binary=Path(sys.argv[1]).resolve();stdlib=Path(sys.argv[2]).resolve()
with tempfile.TemporaryDirectory(prefix='xray-cli-deps-') as directory:
    root=Path(directory);checksum,rows=fixture(root)
    (root/'xray.toml').write_text('[project]\nname="fixture"\nversion="1.0.0"\nmain="root.xr"\n',encoding='utf-8')
    (root/'xray.lock').write_text('# Format version: 1\n[package.fixture/math]\nversion="1.0.0"\nresolved=""\nchecksum="'+checksum+'"\n',encoding='utf-8')
    env=dict(os.environ,HOME=str(root),USERPROFILE=str(root),XRAY_STDLIB_PATH=str(stdlib))
    def invoke(*args):
        result=subprocess.run([str(binary),'deps',*map(str,args)],env=env,capture_output=True,timeout=30)
        sys.stderr.buffer.write(result.stderr);return result
    # Report generation does not execute these functions or their output calls.
    for name,expected_rows in [('root.xr',rows),('io.xr',[(1,0,'io',[]),(0,1,str((root/'io.xr').resolve()),[1])])]:
        entry=root/name;formats=expected_reports(str(entry.resolve()),expected_rows)
        for option,suffix in [('--shell','.sh'),('--json','.json'),('--list','.list')]:
            result=invoke(entry,option)
            assert result.returncode==0 and result.stdout.replace(b'\r\n',b'\n')==formats[suffix],(name,option,result.stdout)
            report=root/('file-'+name+suffix)
            result=invoke(entry,option,'-o',report)
            assert result.returncode==0 and report.read_bytes().replace(b'\r\n',b'\n')==formats[suffix]
            assert b'Dependency script generated:' in result.stdout
    for name in ['bad.xr','missing.xr','cycle.xr','absent.xr']:
        report=root/(name+'.report')
        result=invoke(root/name,'--json','-o',report)
        assert result.returncode!=0 and not report.exists() and not result.stdout,(name,result.stdout)
        # Existing caller output is also untouched because admission precedes fopen.
        report.write_bytes(b'occupied-report\x00canary')
        result=invoke(root/name,'--json','-o',report)
        assert result.returncode!=0 and report.read_bytes()==b'occupied-report\x00canary' and not result.stdout
    result=invoke(root/'root.xr','--json','-o',root/'absent-dir'/'report')
    assert result.returncode!=0 and not result.stdout and not (root/'absent-dir').exists()
print('deps real CLI formats/classification/admission/occupied output PASS')
