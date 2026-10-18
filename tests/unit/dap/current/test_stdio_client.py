"""One framed DAP client shared by the original wire and real process consumers."""
import json
import queue
import re
import socket
import subprocess
import threading

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

class ClientBase:
    def __init__(self,config,read_until=None,process_factory=subprocess.Popen):
        self.peer=None
        mode=['--port','0'] if config.transport=='tcp' else ['--stdio']
        self.process=process_factory([str(config.xray),'dap',*mode],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        self.input=self.process.stdin;self.output=self.process.stdout
        if config.transport=='tcp':
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
        self.transcript=[]
        self.read_until=read_until
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
                value=json.loads(body);self.messages.put(value)
                if self.read_until and value.get('type')=='event' and value.get('event')==self.read_until:return
        except BaseException as error:self.messages.put(error)
    def receive(self):
        value=self.messages.get(timeout=20)
        if isinstance(value,BaseException):raise value
        assert value is not None,'unexpected EOF'
        self.read_seq+=1
        assert value['seq']==self.read_seq
        self.transcript.append({'direction':'receive','message':value})
        if value['type']=='event':self.events.append(value)
        else:self.replies.append(value)
        return value
    def send(self,command,arguments=None,fragment=False):
        self.seq+=1
        value={'seq':self.seq,'type':'request','command':command}
        if arguments is not None:value['arguments']=arguments
        self.transcript.append({'direction':'send','message':value})
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
        stderr=self.process.stderr.read();self.stderr=stderr
        assert self.process.returncode==expected,(self.process.returncode,stderr)
        self.reader.join(timeout=2)
        if self.peer:
            self.output.close();self.peer.close()
            assert not self.process.stdout.read(),'TCP mode wrote protocol bytes to stdout'
