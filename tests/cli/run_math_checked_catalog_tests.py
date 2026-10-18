"""Exercise the complete Checked math Catalog without library sources or children."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

from xir_no_child_process import NoChildProcess

root = Path(__file__).resolve().parents[2]
binary = Path(sys.argv[1]).resolve()
fixture = root / 'tests/unit/xir/math_library'
governed = (root / 'stdlib/math/math.xr').read_text(encoding='utf-8')
consumer = (fixture / 'root.xr').read_text(encoding='utf-8')
for pattern, count in ((r'export fn (\w+)', 37), (r'export const (\w+)', 14)):
    names = set(re.findall(pattern, governed))
    assert len(names) == count and names <= set(re.findall(r'math\.(\w+)', consumer))
cases = [
    ('root.xr', consumer + '\nprint(exercise())\n', b'51\n', None),
    ('ctfe.xr', (fixture / 'ctfe.xr').read_text(encoding='utf-8') + '\nprint(folded()==9.0)\n', b'true\n', None),
    ('random-ctfe.xr', 'import math\nexport fn rejected()->f64{return comptime math.random()}\n', b'',
     'comptime requires an admitted deterministic scalar direct-call closure'),
    ('equal-random-ctfe.xr', 'import math\nexport fn rejected()->i64{return comptime math.randomInt(7,7)}\n', b'',
     'comptime requires an admitted deterministic scalar direct-call closure'),
    ('private.xr', 'print(__sqrt(9.0))\n', b'', 'name is not an initialized value'),
    ('private-import.xr', 'import math\nprint(math.__sqrt(9.0))\n', b'', 'import requires an exported declaration'),
    ('math/math.xr', 'print(__sqrt(9.0))\n', b'', 'name is not an initialized value'),
    ('shadow.xr', 'fn __sqrt(x:f64)->f64{return x+1.0}\nprint(__sqrt(9.0)==10.0)\n', b'true\n', None),
]
with tempfile.TemporaryDirectory(prefix='xir-math-catalog-cli-') as temporary:
    work = Path(temporary)
    (work / 'empty-stdlib').mkdir()
    environment = dict(os.environ, XRAY_STDLIB_PATH=str(work / 'empty-stdlib'),
                       PATH=str(work / 'no-host-C-provider'))
    policy = NoChildProcess(root, work)
    for name, text, expected, diagnostic in cases:
        entry = work / name
        entry.parent.mkdir(parents=True, exist_ok=True)
        entry.write_text(text, encoding='utf-8')
        result = policy.run(name, [str(binary), 'run', str(entry)], environment)
        if diagnostic is None:
            assert result.returncode == 0 and result.stdout == expected and not result.stderr, result
        else:
            assert result.returncode == 1 and not result.stdout and diagnostic in result.stderr.decode('utf-8'), result
    for name, _, expected, _ in cases[:2]:
        entry = work / name
        generated = work / (name + '.c')
        native = work / (name + '.exe')
        provider_environment = dict(os.environ, XRAY_STDLIB_PATH=str(work / 'empty-stdlib'))
        for label, arguments in (
                ('emit', ['build', '--c-only', str(entry), '-o', str(generated)]),
                ('build', ['build', str(entry), '-o', str(native)])):
            result = subprocess.run([str(binary), *arguments], cwd=root,
                                    env=provider_environment, capture_output=True, timeout=60)
            expected_report = f'{"Generated" if label == "emit" else "Built"}: {generated if label == "emit" else native}\n'.encode()
            assert result.returncode == 0 and result.stdout == expected_report and not result.stderr, (label, result)
        text = generated.read_text(encoding='utf-8')
        assert 'const XrXirProgramSpec xray_app_program' in text
        assert 'XrXirCallEntry' in text and not re.search(r'\(\s*\{', text)
        result = policy.run(name + '-native', [str(native)], environment)
        assert result.returncode == 0 and result.stdout == expected and not result.stderr, result
    if policy.supported:
        assert policy.control['absolute_child_denied'] and len(policy.observations) == len(cases) + 3
        print('Inherited Job limit2, absolute child denial control and ten VM/native executions PASS')
print('Complete37 functions/14 constants, actual CTFE, SDK native linking, both RNG refusals, private authority and lexical shadow PASS')
