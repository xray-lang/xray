"""Compile actual stale callers, then reject their retired symbols at link time."""
from pathlib import Path
import subprocess
import sys
import tempfile

compiler, archive, include = sys.argv[1:4]
with tempfile.TemporaryDirectory(prefix='xray-target-retired-') as temporary:
    root = Path(temporary)
    for symbol, declaration, call in [
        ('xtc_xir_target_capture', 'extern int xtc_xir_target_capture(const void *, void **);',
         'void *out = 0; return xtc_xir_target_capture(0, &out);'),
        ('xtc_xir_target_command', 'extern const void *xtc_xir_target_command(const void *, unsigned);',
         'return xtc_xir_target_command(0, 0) != 0;'),
    ]:
        source, obj = root / (symbol + '.c'), root / (symbol + '.obj')
        source.write_text(declaration + '\nint main(void) { ' + call + ' }\n', encoding='ascii')
        p = subprocess.run([compiler, '/nologo', '/W4', '/WX', '/utf-8', '/MT', '/c', str(source),
                            '/Fo' + str(obj)], capture_output=True)
        assert p.returncode == 0 and obj.exists(), p.stdout + p.stderr
        linked = subprocess.run([compiler, '/nologo', '/MT', str(obj), archive,
                                 '/Fe' + str(root / (symbol + '.exe'))], capture_output=True)
        text = (linked.stdout + linked.stderr).decode('utf-8', errors='replace')
        print(text, end='')
        assert linked.returncode != 0 and symbol in text
        assert 'LNK2019' in text or 'undefined symbol' in text
    for name in ['XrXirTargetRequest', 'XrXirTargetCommand']:
        source = root / (name + '.c')
        source.write_text('#include "app/toolchain/xtc_xir_target.h"\n' + name + ' stale;\n', encoding='ascii')
        p = subprocess.run([compiler, '/nologo', '/W4', '/WX', '/utf-8', '/c', '/I' + include,
                            str(source), '/Fo' + str(root / (name + '.obj'))], capture_output=True)
        text = (p.stdout + p.stderr).decode('utf-8', errors='replace')
        assert p.returncode != 0 and name in text, text
print('both actual stale objects fail at their retired symbols; both old input types reject PASS')
