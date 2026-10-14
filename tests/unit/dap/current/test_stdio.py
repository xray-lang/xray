"""Real subprocess DAP framing and current Source/VM request assertions."""
import argparse
import json
import os
from pathlib import Path
import queue
import subprocess
import threading
import time

parser=argparse.ArgumentParser()
parser.add_argument('--xray',type=Path,required=True)
parser.add_argument('--fixture',type=Path,required=True)
args=parser.parse_args()

def wire(message):
    body=json.dumps(message,ensure_ascii=False,separators=(',',':')).encode('utf-8')
    return f'Content-Length: {len(body)}\r\n\r\n'.encode()+body

class Client:
    def __init__(self):
        self.process=subprocess.Popen([str(args.xray),'dap','--stdio'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
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
                    byte=self.process.stdout.read(1)
                    if not byte:
                        if header: raise AssertionError('truncated output header')
                        self.messages.put(None);return
                    header+=byte
                    assert len(header)<257
                length=int(header.split(b':',1)[1].strip())
                body=b''
                while len(body)<length:
                    part=self.process.stdout.read(length-len(body))
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
            for byte in data:self.process.stdin.write(bytes([byte]));self.process.stdin.flush()
        else:self.process.stdin.write(data);self.process.stdin.flush()
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
        if not self.process.stdin.closed:self.process.stdin.close()
        self.process.wait(timeout=20)
        stderr=self.process.stderr.read()
        assert self.process.returncode==expected,(self.process.returncode,stderr)
        self.reader.join(timeout=2)

client=Client()
assert client.send('initialize',{'supportsVariableType':True,'supportsVariablePaging':True},fragment=True)['success']
client.event('initialized')
assert client.send('launch',{'program':str(args.fixture.resolve()).replace('\\','/')})['success']
assert client.send('setFunctionBreakpoints',{'breakpoints':[{'name':'leaf'}]})['body']['breakpoints'][0]['verified']
assert client.send('configurationDone')['success']
client.event('stopped')
frames=client.send('stackTrace',{'threadId':1})['body']['stackFrames']
assert len(frames)==2 and frames[0]['name']=='leaf' and frames[0]['line']==0
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
print('Real CLI/stdio Source launch, fragmented framing, stop/inspect/output/disconnect: PASS')

client=Client()
client.process.stdin.write(wire({'seq':1,'type':'request','command':'initialize'}));client.process.stdin.flush();client.process.stdin.close()
reply=client.receive();assert reply['type']=='response' and reply['success']
client.event('initialized');client.close()
print('Buffered final frame delivered before peer EOF: PASS')

client=Client()
client.process.stdin.write(b'Content-Length: 100\r\n\r\n{}');client.process.stdin.close()
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
    client.process.stdin.write(header.encode()+initialize);client.process.stdin.close()
    client.close(expected=1)
    assert not client.replies
print('Malformed, duplicate and embedded length headers rejected: PASS')

client=Client()
client.process.stdin.write(f'content-length:\t{length} \r\n\r\n'.encode()+initialize);client.process.stdin.close()
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
