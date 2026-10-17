from pathlib import Path
import re,sys
source,target,*names=sys.argv[1:];text=Path(source).read_text(encoding='utf-8')
token=re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]|[^{}"\'/]+|.')
parts=[]
for name in names:
 match=re.search(r'(?m)^(?:(?:XR_FUNC|static)\s+)?(?:XrJsonValue\s*\*|void\s+)\s*'+re.escape(name)+r'\s*\(',text);assert match,name
 start=text.index('{',match.end());depth=0;end=None
 for item in token.finditer(text,start):
  if item.group()=='{':depth+=1
  elif item.group()=='}':
   depth-=1
   if not depth:end=item.end();break
 assert end;parts.append(text[match.start():end])
path=Path(target);path.parent.mkdir(parents=True,exist_ok=True);path.write_text('\n\n'.join(parts)+'\n',encoding='utf-8',newline='\n')
