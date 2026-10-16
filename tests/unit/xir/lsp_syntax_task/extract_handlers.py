from pathlib import Path
import hashlib,json,re,sys
source=Path(sys.argv[1]);out=Path(sys.argv[2]);text=source.read_text(encoding="utf-8")
def extract(signature):
    start=text.index(signature);i=text.index("{",start);depth=0;state="code";quote="";n=i
    while n<len(text):
        c=text[n];d=text[n:n+2]
        if state=="line":
            if c=="\n":state="code"
        elif state=="comment":
            if d=="*/":state="code";n+=1
        elif state=="string":
            if c=="\\":n+=1
            elif c==quote:state="code"
        elif d=="//":state="line";n+=1
        elif d=="/*":state="comment";n+=1
        elif c in "\"'":state="string";quote=c
        elif c=="{":depth+=1
        elif c=="}":
            depth-=1
            if not depth:return text[start:n+1]
        n+=1
    raise AssertionError(signature)
before=len(sys.argv)>5
inputs=[(source,"static int source_request_mode(")] if not before else []
inputs += [(Path(sys.argv[3]),("" if before else "XR_FUNC ")+"XrJsonValue *xlsp_handle_selection_range("),
           (Path(sys.argv[4]),("" if before else "XR_FUNC ")+"XrJsonValue *xlsp_handle_td_document_symbol(")]
parts=[];records=[]
for path,signature in inputs:
    text=path.read_text(encoding="utf-8");part=extract(signature);parts.append(part)
    records.append(dict(source=str(path.resolve()),sha256=hashlib.sha256(path.read_bytes()).hexdigest(),signature=signature,function_sha256=hashlib.sha256(part.encode()).hexdigest()))
body="\n\n".join(parts)+"\n"
if before:body="#define xr_compile_resources_new syntax_before_resources\n#define xlsp_source_selection_json before_xlsp_source_selection_json\n#define xlsp_handle_selection_range before_xlsp_handle_selection_range\n#define xlsp_handle_td_document_symbol before_xlsp_handle_td_document_symbol\n"+body+"#undef xlsp_handle_td_document_symbol\n#undef xlsp_handle_selection_range\n#undef xlsp_source_selection_json\n#undef xr_compile_resources_new\n"
out.parent.mkdir(parents=True,exist_ok=True);out.write_text(body,encoding="utf-8",newline="\n")
(out.parent/("before-source.json" if before else "syntax-source.json")).write_text(json.dumps(records,indent=2)+"\n",newline="\n")
