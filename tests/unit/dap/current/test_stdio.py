"""Real subprocess DAP framing and current Source/VM request assertions."""
import argparse
import json
import os
from pathlib import Path
import queue
import re
import socket
import subprocess
import threading
import time

parser=argparse.ArgumentParser()
parser.add_argument('--xray',type=Path,required=True)
parser.add_argument('--fixture',type=Path,required=True)
parser.add_argument('--start-request',choices=('launch','attach'),default='launch')
parser.add_argument('--transport',choices=('stdio','tcp'),default='stdio')
args=parser.parse_args()

if args.transport=='tcp':
    for flags in [('--stdio','--port','0'),('--port','65536'),('--port','4294967296'),('--port','-1')]:
        rejected=subprocess.run([str(args.xray),'dap',*flags],input=b'',capture_output=True,timeout=20)
        assert rejected.returncode==2 and not rejected.stdout,(flags,rejected.returncode,rejected.stdout,rejected.stderr)

def wire(message):
    body=json.dumps(message,ensure_ascii=False,separators=(',',':')).encode('utf-8')
    return f'Content-Length: {len(body)}\r\n\r\n'.encode()+body

class SocketInput:
    def __init__(self,peer):self.peer=peer;self.closed=False
    def write(self,data):self.peer.sendall(data);return len(data)
    def flush(self):pass
    def close(self):
        if not self.closed:
            self.closed=True
            self.peer.shutdown(socket.SHUT_WR)

class Client:
    def __init__(self):
        self.peer=None
        mode=['--port','0'] if args.transport=='tcp' else ['--stdio']
        self.process=subprocess.Popen([str(args.xray),'dap',*mode],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        self.input=self.process.stdin;self.output=self.process.stdout
        if args.transport=='tcp':
            try:
                notice=queue.Queue()
                threading.Thread(target=lambda:notice.put(self.process.stderr.readline()),daemon=True).start()
                line=notice.get(timeout=20);match=re.fullmatch(rb'DAP: listening on 127\.0\.0\.1:(\d+)\r?\n',line)
                assert match,line
                self.peer=socket.create_connection(('127.0.0.1',int(match[1])),timeout=20)
                self.process.stdin.close()
                self.input=SocketInput(self.peer);self.output=self.peer.makefile('rb',buffering=0)
            except BaseException:
                self.process.kill();self.process.wait(timeout=20)
                if self.peer:self.peer.close()
                raise
        self.messages=queue.Queue()
        self.seq=0
        self.events=[]
        self.replies=[]
        self.read_seq=0
        self.reader=threading.Thread(target=self.read,daemon=True)
        self.reader.start()
    def read(self):
        try:
            while True:
                header=b''
                while not header.endswith(b'\r\n\r\n'):
                    byte=self.output.read(1)
                    if not byte:
                        if header: raise AssertionError('truncated output header')
                        self.messages.put(None);return
                    header+=byte
                    assert len(header)<257
                length=int(header.split(b':',1)[1].strip())
                body=b''
                while len(body)<length:
                    part=self.output.read(length-len(body))
                    assert part,'truncated output body'
                    body+=part
                self.messages.put(json.loads(body))
        except BaseException as error:self.messages.put(error)
    def receive(self):
        value=self.messages.get(timeout=20)
        if isinstance(value,BaseException):raise value
        assert value is not None,'unexpected EOF'
        self.read_seq+=1
        assert value['seq']==self.read_seq
        if value['type']=='event':self.events.append(value)
        else:self.replies.append(value)
        return value
    def send(self,command,arguments=None,fragment=False):
        self.seq+=1
        value={'seq':self.seq,'type':'request','command':command}
        if arguments is not None:value['arguments']=arguments
        data=wire(value)
        if fragment:
            for byte in data:self.input.write(bytes([byte]));self.input.flush()
        else:self.input.write(data);self.input.flush()
        while True:
            reply=self.receive()
            if reply['type']=='response' and reply['request_seq']==self.seq:
                assert reply['command']==command
                return reply
    def event(self,name):
        for item in self.events:
            if item['event']==name:self.events.remove(item);return item
        while True:
            item=self.receive()
            if item['type']=='event' and item['event']==name:self.events.remove(item);return item
    def close(self,expected=0):
        if not self.input.closed:self.input.close()
        self.process.wait(timeout=20)
        stderr=self.process.stderr.read()
        assert self.process.returncode==expected,(self.process.returncode,stderr)
        self.reader.join(timeout=2)
        if self.peer:
            self.output.close();self.peer.close()
            assert not self.process.stdout.read(),'TCP mode wrote protocol bytes to stdout'

client=Client()
assert client.send('initialize',{'supportsVariableType':True,'supportsVariablePaging':True},fragment=True)['success']
client.event('initialized')
assert client.send(args.start_request,{'program':str(args.fixture.resolve()).replace('\\','/')})['success']
assert client.send('setFunctionBreakpoints',{'breakpoints':[{'name':'leaf'}]})['body']['breakpoints'][0]['verified']
assert client.send('configurationDone')['success']
client.event('stopped')
frames=client.send('stackTrace',{'threadId':1})['body']['stackFrames']
assert len(frames)==2 and frames[0]['name']=='leaf' and frames[0]['line']==0
assert all(frame['column']==0 and frame['source']['name']==args.fixture.name and
           Path(frame['source']['path']).resolve()==args.fixture.resolve() for frame in frames)
scope=client.send('scopes',{'frameId':frames[0]['id']})['body']['scopes'][0]['variablesReference']
values=client.send('variables',{'variablesReference':scope})['body']['variables']
assert values[0]['value']=='20'
assert client.send('continue',{'threadId':1})['success']
output=client.event('output')['body']['output']
assert output=='owned\0value\n',repr(output)
client.event('exited');client.event('terminated')
assert client.send('restart')['success']
client.event('stopped')
assert client.send('scopes',{'frameId':frames[0]['id']})['success'] is False
assert client.send('continue',{'threadId':1})['success']
assert client.event('output')['body']['output']=='owned\0value\n'
client.event('exited');client.event('terminated')
assert client.send('scopes',{'frameId':frames[0]['id']})['success'] is False
assert client.send('disconnect')['success']
client.close()
print(f'Real CLI/stdio Source {args.start_request}, fragmented framing, stop/inspect/output/disconnect: PASS')

client=Client()
client.input.write(wire({'seq':1,'type':'request','command':'initialize'}));client.input.flush();client.input.close()
reply=client.receive();assert reply['type']=='response' and reply['success']
client.event('initialized');client.close()
print('Buffered final frame delivered before peer EOF: PASS')

client=Client()
client.input.write(b'Content-Length: 100\r\n\r\n{}');client.input.close()
client.close(expected=1)
print('Truncated input EOF rejected: PASS')

initialize=json.dumps({'seq':1,'type':'request','command':'initialize'},separators=(',',':')).encode()
length=len(initialize)
for header in [f'Content-Length: {length}junk\r\n\r\n',
               f'Content-Length: {length}\r\nContent-Length: {length}\r\n\r\n',
               f'X-Content-Length: {length}\r\n\r\n',
               f'Content-Length: {length}\n\r\n\r\n',
               f'Content-Length: +{length}\r\n\r\n']:
    client=Client()
    client.input.write(header.encode()+initialize);client.input.close()
    client.close(expected=1)
    assert not client.replies
print('Malformed, duplicate and embedded length headers rejected: PASS')

client=Client()
client.input.write(f'content-length:\t{length} \r\n\r\n'.encode()+initialize);client.input.close()
assert client.receive()['success'];client.event('initialized');client.close()
print('Case-insensitive length header with optional whitespace: PASS')

timer=args.fixture.parent/'timer_fixture.xr'
client=Client()
assert client.send('initialize')['success'];client.event('initialized')
assert client.send('launch',{'program':str(timer.resolve()).replace('\\','/')})['success']
start=time.monotonic()
assert client.send('configurationDone')['success']
assert client.event('output')['body']['output']=='timer done\n'
assert time.monotonic()-start>=0.10,'timer resumed before its duration'
client.event('exited');client.event('terminated')
assert client.send('disconnect')['success'];client.close()
print('Real language timer resumes after elapsed duration: PASS')

client=Client()
assert client.send('initialize')['success'];client.event('initialized')
assert client.send('launch',{'program':str((args.fixture.parent/'cancel_timer_fixture.xr').resolve()).replace('\\','/')})['success']
assert client.send('configurationDone')['success']
start=time.monotonic()
assert client.send('terminate')['success']
client.event('terminated')
assert time.monotonic()-start<2,'cancellation waited for the five-second timer'
assert all(item['event']!='output' for item in client.events)
assert client.send('disconnect')['success'];client.close()
print('Cancel remains responsive during a pending timer: PASS')
