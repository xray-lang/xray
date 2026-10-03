"""Extract exact production functions/dispatch for a limited protocol gate.
Whole legacy servers are intentionally not represented by this target.
"""
import argparse, hashlib, json, pathlib, re
p=argparse.ArgumentParser();p.add_argument('--root',required=True);p.add_argument('--output',required=True);a=p.parse_args()
root=pathlib.Path(a.root);out=pathlib.Path(a.output);records=[];parts=[]
def add(path,label,text):
    records.append(dict(path=path,label=label,sha256=hashlib.sha256(text.encode()).hexdigest()))
    parts.append('/* Exact production selection: '+path+' : '+label+' */\n'+text)
def block(text,start):
    brace=text.index('{',start);depth=0;i=brace;state='code'
    while i<len(text):
        c=text[i];n=text[i+1:i+2]
        if state=='line':
            if c=='\n':state='code'
        elif state=='comment':
            if c=='*' and n=='/':state='code';i+=1
        elif state in ('"',"'"):
            if c=='\\':i+=1
            elif c==state:state='code'
        elif c=='/' and n=='/':state='line';i+=1
        elif c=='/' and n=='*':state='comment';i+=1
        elif c in ('"',"'"):state=c
        elif c=='{':depth+=1
        elif c=='}':
            depth-=1
            if depth==0:return text[start:i+1]
        i+=1
    raise AssertionError('unterminated block')
def fn(path,name):
    text=(root/path).read_text(encoding='utf-8');matches=list(re.finditer(r'(?m)^(?:static |XR_FUNC |void |XrJsonValue |XrLspDocument )[^;{}]*?\b'+re.escape(name)+r'\s*\([^;{}]*\)\s*\{',text));assert len(matches)==1,(name,len(matches))
    add(path,name,block(text,matches[0].start()))
lang='src/app/mcp/xmcp_tools_lang.c';text=(root/lang).read_text(encoding='utf-8')
for name in ('ErrorCapture','FormatCapture'):
    match=re.search(r'typedef struct(?: '+name+r')?\s*\{[^}]+\}\s*'+name+r';',text);assert match
    add(lang,name,match.group())
for name in ('make_diagnostic','make_parser_diagnostics','make_format_result_content','format_error_callback'):fn(lang,name)
for name in ('xmcp_make_error_result','xmcp_make_text_result','xmcp_make_text_structured_result'):fn('src/app/mcp/xmcp_tools.c',name)
fn(lang,'xmcp_tool_xray_format')
server='src/app/lsp/xlsp_server.c'
text=(root/server).read_text(encoding='utf-8')
add(server,'cancel constant',re.search(r'(?m)^#define LSP_ERROR_REQUEST_CANCELLED .+$',text).group())
for name in ('hash_uri','doc_table_get','xlsp_document_get','xlsp_request_id_free','xlsp_request_id_equals','xlsp_request_id_to_json','xlsp_request_id_debug','send_response','send_error','pending_request_remove','pending_request_is_cancelled'):fn(server,name)
fn('src/app/lsp/xlsp_handlers_textdoc.c','xlsp_handle_td_formatting')
for name in ('write_all','xlsp_transport_write'):fn('src/app/lsp/xlsp_transport.c',name)
text=(root/server).read_text(encoding='utf-8');needle='if (entry->request_handler) {';assert text.count(needle)==1
selected=block(text,text.index(needle));records.append(dict(path=server,label='request_handler branch',sha256=hashlib.sha256(selected.encode()).hexdigest()))
parts.append("""static void selected_dispatch(XrLspServer *server,XrJsonValue *params, bool formatting) {
    struct FixtureEntry { XrJsonValue *(*request_handler)(XrLspServer *,XrJsonValue *); } value={formatting?xlsp_handle_td_formatting:fixture_other_handler};
    const struct FixtureEntry *entry=&value;
    XlspRequestId id={.kind=XLSP_ID_NUMBER,.as.number=7};
"""+selected+'\n}')
out.parent.mkdir(parents=True,exist_ok=True);out.write_text('\n\n'.join(parts)+'\n',encoding='utf-8');out.with_suffix('.json').write_text(json.dumps(records,indent=2),encoding='utf-8')
