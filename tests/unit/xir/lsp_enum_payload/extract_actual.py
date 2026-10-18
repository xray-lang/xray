from pathlib import Path
import sys,re,json,hashlib
root=Path(sys.argv[1]);output=Path(sys.argv[2]);output.mkdir(parents=True,exist_ok=True)
token=re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]|[^{}"\'/]+|.')
def extract(text,name):
 match=re.search(r'(?m)^[A-Za-z_][^\n;{}]*\b'+re.escape(name)+r'\s*\(',text);assert match,name
 start=text.index('{',match.end());depth=0
 for item in token.finditer(text,start):
  if item.group()=='{':depth+=1
  elif item.group()=='}':
   depth-=1
   if not depth:return text[match.start():item.end()]
 raise AssertionError(name)
groups={
 'enum_strategy_actual.inc.c':{'src/app/lsp/xlsp_document_position.c':['display_document_valid','display_line_span','xlsp_position_to_offset'],
  'src/app/lsp/xlsp_completion.c':['find_enum_in_ast','enum_completion_is_pattern_context','completion_buffer_append','enum_completion_item','enum_completion_failed','set_enum_payload_completion','complete_enum_decl']},
 'enum_response_actual.inc.c':{'src/app/lsp/xlsp_handlers_textdoc.c':['xlsp_handle_td_completion']}}
records=[]
for dest,sources in groups.items():
 parts=[]
 for name,functions in sources.items():
  path=root/name;text=path.read_text(encoding='utf-8')
  for function in functions:
   body=extract(text,function);parts.append(body);records.append(dict(source=name,function=function,sha256=hashlib.sha256(body.encode()).hexdigest()))
 (output/dest).write_text('\n\n'.join(parts)+'\n',encoding='utf-8',newline='\n')
(output/'enum-fragment-inputs.json').write_text(json.dumps(dict(all_function_bodies_raw_extracted=True,functions=records,full_completion_dispatch_and_document_lookup_are_unit_adapters=True),indent=2)+'\n',encoding='utf-8',newline='\n')
