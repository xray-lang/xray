from pathlib import Path
import argparse,hashlib,json,re,subprocess
p=argparse.ArgumentParser();p.add_argument('--source-root',type=Path,required=True);p.add_argument('--build-root',type=Path,required=True);p.add_argument('--exe',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args();sha=lambda x:hashlib.sha256(x.read_bytes()).hexdigest()
source=a.source_root/'tests/unit/xir/test_xir_vm19_fault_oracle.c';entry=next(x for x in json.loads((a.build_root/'compile_commands.json').read_text()) if Path(x['file']).resolve()==source.resolve())
match=re.search(r'(?:/Fo|-o\s+)([^\s]+)',entry['command']);assert match,'Actual object path missing'
obj=Path(match.group(1).strip('"'));obj=obj if obj.is_absolute() else Path(entry['directory'])/obj;assert obj.is_file() and a.exe.is_file()
relative=str(obj.relative_to(a.build_root)).replace('\\','/')
deps=subprocess.check_output(['ninja','-C',str(a.build_root),'-t','deps',relative],text=True)
assert 'deps' in deps and 'VALID' in deps,'Fresh UTF8 Ninja dependencies required'
inputs={}
for line in deps.splitlines()[1:]:
 if not line.startswith('    '):continue
 path=Path(line.strip());path=path if path.is_absolute() else a.build_root/path
 if path.is_file():inputs[str(path.resolve())]=sha(path)
# The main translation unit is an explicit compile input, not an MSVC /showIncludes dependency.
assert source.is_file()
inputs[str(source.resolve())]=sha(source)
assert str(source.resolve()) in inputs and len(inputs)>50
commands=subprocess.check_output(['ninja','-C',str(a.build_root),'-t','commands',str(a.exe.relative_to(a.build_root)).replace('\\','/')],text=True)
link=commands.splitlines()[-1]
for token in re.findall(r'[^\s"]+\.lib',link):
 token=token.removeprefix('/implib:');path=Path(token);path=path if path.is_absolute() else a.build_root/path
 if path.is_file():inputs[str(path.resolve())]=sha(path)
inputs[str(obj.resolve())]=sha(obj)
assert not a.out.exists()
a.out.parent.mkdir(parents=True,exist_ok=True)
a.out.write_text(json.dumps({'status':'ACTUAL_INPUT_BINDING_NOT_TEST_PASS','executable_sha256':sha(a.exe),'object_sha256':sha(obj),'compile_recipe':entry,'link_recipe':link,'ninja_dependency_text':deps,'inputs':inputs,'source_root':str(a.source_root),'build_root':str(a.build_root)},indent=2)+'\n')
