import subprocess,sys
result=subprocess.run([sys.argv[1]],capture_output=True,text=True,encoding="utf-8",errors="replace",timeout=10)
sys.stdout.write(result.stdout);sys.stderr.write(result.stderr)
assert result.returncode!=0 and "found==1" in result.stderr
print("Original sort body rejected: a work prefix duplicates or loses a receiving owner")
