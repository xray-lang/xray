from pathlib import Path
import hashlib,subprocess,sys,tempfile
exe,dll=map(lambda s:Path(s).resolve(),sys.argv[1:3])
def run(args):
    p=subprocess.run([str(exe),*map(str,args)],capture_output=True,timeout=90)
    sys.stdout.buffer.write(p.stdout);sys.stderr.buffer.write(p.stderr)
    assert p.returncode==0,p.returncode
    for line in p.stdout.decode('utf8').splitlines():
        if line.startswith('IMAGE '):
            _,kind,size,digest,path=line.split(' ',4)
            data=Path(path).read_bytes()
            assert len(data)==int(size) and hashlib.sha256(data).hexdigest()==digest,path
with tempfile.TemporaryDirectory(prefix='xray-image-owner-') as temporary:
    root=Path(temporary);data=root/'file.bin';data.write_bytes(b'abc')
    run([root,data,root/'alias.bin',root/'saved.bin','unit'])
    run([root,data,root/'alias.bin',dll,'process'])
print('real collector and independently hashed image files PASS')
