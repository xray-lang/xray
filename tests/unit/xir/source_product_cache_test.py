"""Exercise the default SourceProduct projection without library source files."""
import pathlib
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='xir-product-cache-') as temporary:
    root = pathlib.Path(temporary).resolve()
    state = 'var serial:i64=41\nexport fn main()->i64 { serial+=1; return serial }\n'
    (root/'plain.xr').write_text(state, encoding='utf-8')
    (root/'cached.xr').write_text(
        'import "std/io/output" as output\n'
        'export fn stdoutText(data:string)->bool { return output.writeStdout(data) }\n'
        'export fn stderrText(data:string)->bool { return output.writeStderr(data) }\n'
        'output.writeStdout("A\\0B")\n' + state, encoding='utf-8')
    subprocess.run([sys.argv[1],str(root),str(root/'plain.xr'),str(root/'cached.xr'),sys.argv[2]], check=True)
