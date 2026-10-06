"""Execute the immutable catalog after its original Source producer has died."""
import pathlib
import subprocess
import sys
import tempfile

executable, source = map(pathlib.Path, sys.argv[1:])
with tempfile.TemporaryDirectory(prefix='xir-cache-consumer-') as temporary:
    root = pathlib.Path(temporary).resolve()
    # This deliberately shifts library ordinals and adds a homonymous user declaration.
    text = (source/'tests/fixtures/xir_stdlib_output/root.xr').read_text(encoding='utf-8')
    (root/'root.xr').write_text('export fn padding() -> i64 { return 7 }\n' + text +
                             '\nexport fn writeStdout() -> bool { return false }\nexport fn writer() -> fn(string)->bool { return output.writeStdout }\n', encoding='utf-8')
    subprocess.run([str(executable.resolve()), str(root), str(root/'root.xr')], check=True)
