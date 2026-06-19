#!/usr/bin/env python3
"""
DAP native-backend driver.

Drives `xray dap --native` over stdio and verifies that a `.xr` program can be
debugged at the source level: launch (which transparently compiles the `.xr`
to a `-g` native binary and hands it to lldb-dap), set a source breakpoint on
a `.xr` line, hit it, read a stack trace that points back at the `.xr` source,
then continue to exit.

Usage:
    native_driver.py <xray-bin> <program.xr> <breakpoint-line>

Exit code 0 means every assertion passed.
"""
import json
import os
import queue
import subprocess
import sys
import threading
import time


def main():
    if len(sys.argv) != 4:
        print("usage: native_driver.py <xray-bin> <program.xr> <bpline>", file=sys.stderr)
        return 2
    xray, prog, bpline = sys.argv[1], sys.argv[2], int(sys.argv[3])

    proc = subprocess.Popen([xray, "dap", "--native"],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, bufsize=0)
    q = queue.Queue()

    def reader():
        buf = b""
        while True:
            chunk = proc.stdout.read(1)
            if not chunk:
                q.put(None)
                return
            buf += chunk
            while True:
                idx = buf.find(b"\r\n\r\n")
                if idx < 0:
                    break
                clen = 0
                for line in buf[:idx].decode("ascii", "replace").split("\r\n"):
                    if line.lower().startswith("content-length:"):
                        clen = int(line.split(":")[1].strip())
                need = idx + 4 + clen
                while len(buf) < need:
                    more = proc.stdout.read(need - len(buf))
                    if not more:
                        q.put(None)
                        return
                    buf += more
                body = buf[idx + 4:need]
                buf = buf[need:]
                try:
                    q.put(json.loads(body.decode("utf-8")))
                except Exception as e:
                    q.put({"_parse_error": str(e)})

    threading.Thread(target=reader, daemon=True).start()

    seq = [0]

    def send(cmd, args=None):
        seq[0] += 1
        msg = {"seq": seq[0], "type": "request", "command": cmd}
        if args is not None:
            msg["arguments"] = args
        data = json.dumps(msg).encode("utf-8")
        proc.stdin.write(b"Content-Length: %d\r\n\r\n%s" % (len(data), data))
        proc.stdin.flush()

    def wait_for(pred, timeout=40, label=""):
        end = time.time() + timeout
        seen = []
        while time.time() < end:
            try:
                m = q.get(timeout=max(0.01, end - time.time()))
            except queue.Empty:
                break
            if m is None:
                break
            seen.append(m)
            if pred(m):
                return m
        sys.stderr.write("TIMEOUT waiting for %s\n" % label)
        for m in seen:
            sys.stderr.write("  " + json.dumps(m)[:160] + "\n")
        sys.stderr.write(proc.stderr.read().decode("utf-8", "replace"))
        return None

    failures = [0]

    def check(cond, msg):
        print(("PASS" if cond else "FAIL"), msg)
        if not cond:
            failures[0] += 1

    # initialize
    send("initialize", {"adapterID": "xray", "linesStartAt1": True, "columnsStartAt1": True,
                        "pathFormat": "path"})
    m = wait_for(lambda m: m.get("type") == "response" and m.get("command") == "initialize",
                 label="initialize response")
    check(m is not None and m.get("success"), "initialize succeeds")

    # launch the .xr (bridge compiles to -g native + drives lldb-dap)
    send("launch", {"program": prog, "stopOnEntry": False})
    m = wait_for(lambda m: m.get("type") == "event" and m.get("event") == "initialized",
                 label="initialized event")
    check(m is not None, "received 'initialized' event")

    # set a breakpoint on the .xr source line
    send("setBreakpoints", {"source": {"path": prog}, "breakpoints": [{"line": bpline}]})
    m = wait_for(lambda m: m.get("type") == "response" and m.get("command") == "setBreakpoints",
                 label="setBreakpoints response")
    verified = bool(m) and any(b.get("verified") for b in m.get("body", {}).get("breakpoints", []))
    check(verified, "breakpoint at %s:%d verified" % (os.path.basename(prog), bpline))

    send("configurationDone")

    m = wait_for(lambda m: m.get("type") == "event" and m.get("event") == "stopped",
                 label="stopped event")
    reason = (m or {}).get("body", {}).get("reason")
    check(m is not None, "received 'stopped' event")
    check(reason == "breakpoint", "stop reason is 'breakpoint' (got %r)" % reason)
    tid = (m or {}).get("body", {}).get("threadId", 1)

    send("stackTrace", {"threadId": tid})
    m = wait_for(lambda m: m.get("type") == "response" and m.get("command") == "stackTrace",
                 label="stackTrace response")
    top_line = None
    xr_frame = False
    if m:
        for fr in m.get("body", {}).get("stackFrames", []):
            src = (fr.get("source") or {}).get("path") or (fr.get("source") or {}).get("name") or ""
            if src.endswith(".xr"):
                xr_frame = True
                if top_line is None:
                    top_line = fr.get("line")
    check(xr_frame, "stackTrace has a frame in .xr source")
    check(top_line == bpline, "top frame line == %d (got %r)" % (bpline, top_line))

    send("continue", {"threadId": tid})
    m = wait_for(lambda m: m.get("type") == "event" and m.get("event") in ("terminated", "exited"),
                 label="terminated/exited event")
    check(m is not None, "program terminated/exited after continue")

    send("disconnect", {})
    time.sleep(0.2)
    try:
        proc.terminate()
    except Exception:
        pass

    print("RESULT:", "ALL PASS" if failures[0] == 0 else "%d FAILED" % failures[0])
    return 0 if failures[0] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
