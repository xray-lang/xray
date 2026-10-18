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
names=["static void send_protocol_message(","static void send_error(","static void handle_json_frame("]
parts=[extract(n) for n in names];out.parent.mkdir(parents=True,exist_ok=True)
out.write_text("\n\n".join(parts)+"\n",encoding="utf-8",newline="\n")
(out.parent/"byte-source.json").write_text(json.dumps(dict(source=str(source.resolve()),sha256=hashlib.sha256(source.read_bytes()).hexdigest(),functions=[dict(signature=n,sha256=hashlib.sha256(p.encode()).hexdigest()) for n,p in zip(names,parts)]),indent=2)+"\n",newline="\n")
