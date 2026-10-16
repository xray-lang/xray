import subprocess,sys
progress=len(sys.argv)>2
r=subprocess.run([sys.argv[1],*(["--before-progress"] if progress else [])],capture_output=True,timeout=25)
expected=b"PROGRESS_BEFORE embedded_NUL_selected_prefix=1 callbacks=1 JSON0/0 id0/0\n" if progress else b"ENVELOPE_BEFORE duplicate_id_selected=1 JSON0/0\n"
assert r.returncode==1 and r.stderr.replace(b"\r\n",b"\n")==expected,(r.returncode,r.stdout,r.stderr)
assert not r.stdout
print("Exact predecessor counterexample observed:",expected.decode().strip())
