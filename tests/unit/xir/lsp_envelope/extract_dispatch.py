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
names=["static bool progress_cancel_token_matches(","static void handle_cancel_request(","static void handle_message("]
before=len(sys.argv)>3
if before:names=names[1:]
parts=[extract(n) for n in names];out.parent.mkdir(parents=True,exist_ok=True)
body="\n\n".join(parts)+"\n"
if before:body="#define handle_message before_handle_message\n#define handle_cancel_request before_handle_cancel_request\n"+body+"#undef handle_cancel_request\n#undef handle_message\n"
out.write_text(body,encoding="utf-8",newline="\n")
(out.parent/("before-dispatch-source.json" if before else "dispatch-source.json")).write_text(json.dumps(dict(source=str(source.resolve()),sha256=hashlib.sha256(source.read_bytes()).hexdigest(),functions=[dict(signature=n,sha256=hashlib.sha256(p.encode()).hexdigest()) for n,p in zip(names,parts)]),indent=2)+"\n",newline="\n")
