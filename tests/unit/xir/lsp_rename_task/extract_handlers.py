from pathlib import Path
import hashlib,json,sys
server,textdoc,out=map(Path,sys.argv[1:4])
def extract(text,signature):
    start=text.index(signature);n=text.index("{",start);depth=0;state="code";quote=""
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
parts=[];records=[]
for path,signature in [(server,"static int source_request_mode("),
    (textdoc,"XrJsonValue *xlsp_handle_td_rename("),(textdoc,"XrJsonValue *xlsp_handle_td_prepare_rename(")]:
    text=path.read_text(encoding="utf-8");part=extract(text,signature);parts.append(part)
    records.append(dict(path=str(path.resolve()),source_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
        signature=signature,function_sha256=hashlib.sha256(part.encode()).hexdigest()))
out.parent.mkdir(parents=True,exist_ok=True);out.write_text("\n\n".join(parts)+"\n",encoding="utf-8",newline="\n")
out.with_name("rename-source.json").write_text(json.dumps(records,indent=2)+"\n",encoding="utf-8")
