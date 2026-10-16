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
parser.add_argument('--scenario',choices=('legacy','disconnect-clean','stop-on-entry','line-breakpoint','logpoint','mutate-oracle','owned-oracle',
    'source-step-in','source-next','source-step-out','condition-oracle','hit-oracle','replace-oracle'),default='legacy')
parser.add_argument('--oracle',type=Path)
args=parser.parse_args()
if args.scenario!='legacy' and args.scenario!='disconnect-clean':assert args.oracle

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

def legacy_wire():
    client=Client()
    assert client.send('initialize',{'supportsVariableType':True,'supportsVariablePaging':True},fragment=True)['success']
    client.event('initialized')
    assert client.send(args.start_request,{'program':str(args.fixture.resolve()).replace('\\','/')})['success']
    assert client.send('setFunctionBreakpoints',{'breakpoints':[{'name':'leaf'}]})['body']['breakpoints'][0]['verified']
    assert client.send('configurationDone')['success']
    client.event('stopped')
    frames=client.send('stackTrace',{'threadId':1})['body']['stackFrames']
    assert len(frames)==2 and frames[0]['name']=='leaf' and frames[0]['line']==2 and frames[0]['column']==9
    assert frames[1]['line']==7 and frames[1]['column']==1
    assert all(frame['source']['name']==args.fixture.name and
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


def oracle_case(identifier):
    data=json.loads(args.oracle.read_text(encoding='utf-8'))
    case=next(case for case in data['cases'] if case['id']==identifier)
    assert case['source']==args.fixture.name
    return case

def marker_line(marker):
    lines=args.fixture.read_text(encoding='utf-8').splitlines()
    matches=[number for number,line in enumerate(lines,1) if f'DAP_STOP: {marker}' in line]
    assert len(matches)==1,(marker,matches)
    return matches[0]

def original_session():
    client=Client()
    initialized=client.send('initialize',{'supportsVariableType':True,'supportsVariablePaging':True})
    assert initialized['success'] and initialized['body']['supportsConfigurationDoneRequest']
    assert initialized['body']['supportsPauseRequest'] and initialized['body']['supportsEvaluateForHovers']
    client.event('initialized')
    return client

def original_finish(client,case,retired=(),retired_frames=(),expected_stdout=None):
    exited=client.event('exited')
    assert exited['body']['exitCode']==0,exited
    client.event('terminated')
    stdout=''.join(item['body']['output'] for item in client.events
                   if item['event']=='output' and item['body']['category']=='stdout')
    expected=case['stdout_utf8'] if expected_stdout is None else expected_stdout
    assert stdout==expected,(stdout,expected)
    for reference in retired:assert not client.send('variables',{'variablesReference':reference})['success']
    for frame in retired_frames:assert not client.send('scopes',{'frameId':frame})['success']
    assert client.send('disconnect')['success']
    client.close()

def original_wire():
    client=original_session()
    try:
        if args.scenario=='disconnect-clean':
            assert client.send('disconnect')['success']
            client.close()
            assert not any(item['event']=='output' for item in client.events)
            print('Original test_disconnect_clean: no launch, real response and clean process exit PASS')
            return
        identifier={'logpoint':'conditional-and-hit','owned-oracle':'owned-values'}.get(args.scenario,'stack-and-step')
        case=oracle_case(identifier)
        launch={'program':args.fixture.resolve().as_posix()}
        if args.scenario=='stop-on-entry':launch['stopOnEntry']=True
        assert client.send('launch',launch)['success']
        if args.scenario in ('line-breakpoint','mutate-oracle','owned-oracle'):
            marker='owned_values' if args.scenario=='owned-oracle' else 'inner_update'
            line=marker_line(marker)
            reply=client.send('setBreakpoints',{'source':{'path':launch['program']},'breakpoints':[{'line':line}]})
            assert reply['success'] and reply['body']['breakpoints'][0]['verified'],reply
            assert reply['body']['breakpoints'][0]['line']==line
        elif args.scenario=='logpoint':
            line=marker_line('loop_body')
            reply=client.send('setBreakpoints',{'source':{'path':launch['program']},
                'breakpoints':[{'line':line,'logMessage':'index={index},sum={sum}'}]})
            assert reply['success'] and reply['body']['breakpoints'][0]['verified'],reply
        assert client.send('configurationDone')['success']
        retired=[];retired_frames=[]
        if args.scenario in ('stop-on-entry','line-breakpoint','mutate-oracle','owned-oracle'):
            stopped=client.event('stopped')
            assert stopped['body']['reason']==('entry' if args.scenario=='stop-on-entry' else 'breakpoint'),stopped
            assert not any(item['event']=='output' for item in client.events)
            frames=client.send('stackTrace',{'threadId':stopped['body']['threadId']})['body']['stackFrames']
            assert frames and all(frame['id']>0 for frame in frames)
            retired_frames=[frame['id'] for frame in frames]
            for policy in (False,None,1,'true'):
                refused=client.send('disconnect',{'terminateDebuggee':policy})
                assert not refused['success'],refused
                retained=client.send('stackTrace',{'threadId':stopped['body']['threadId']})
                assert retained['success'] and retained['body']['stackFrames']==frames,retained
            if args.scenario!='stop-on-entry':
                expected=next(stop for stop in case['stops'] if stop['marker']==marker)
                assert frames[0]['name']==expected['function'] and frames[0]['line']==line
                assert Path(frames[0]['source']['path']).resolve()==args.fixture.resolve()
                scope=client.send('scopes',{'frameId':frames[0]['id']})['body']['scopes'][0]['variablesReference']
                values=client.send('variables',{'variablesReference':scope})['body']['variables']
                observed={value['name']:value['value'] for value in values}
                if args.scenario=='owned-oracle':
                    by_name={value['name']:value for value in values}
                    assert observed['title']==expected['locals']['title']
                    for name in ('values','snapshot'):
                        reference=by_name[name]['variablesReference'];retired.append(reference)
                        items=client.send('variables',{'variablesReference':reference})['body']['variables']
                        assert [int(item['value']) for item in items]==expected['locals'][name],items
                    assert not client.send('setVariable',{'variablesReference':scope,'name':'snapshot','value':'values'})['success']
                    assert not client.send('setVariable',{'variablesReference':scope,'name':'title','value':'"new"'})['success']
                    assert not client.send('setVariable',{'variablesReference':scope,'name':'values','value':'true'})['success']
                else:
                    assert all(observed[name]==str(value) for name,value in expected['locals'].items()),observed
                    caller=expected['caller'];assert frames[1]['name']==caller['function']
                    outer_scope=client.send('scopes',{'frameId':frames[1]['id']})['body']['scopes'][0]['variablesReference']
                    outer_values=client.send('variables',{'variablesReference':outer_scope})['body']['variables']
                    outer={value['name']:value['value'] for value in outer_values}
                    assert all(outer[name]==str(value) for name,value in caller['locals'].items()),outer
                    evaluated=client.send('evaluate',{'expression':'doubled + 2','frameId':frames[0]['id'],'context':'hover'})
                    assert evaluated['success'] and evaluated['body']['result']=='42',evaluated
                    unchanged=client.send('evaluate',{'expression':'doubled','frameId':frames[0]['id']})
                    assert unchanged['success'] and unchanged['body']['result']=='40',unchanged
                    length=client.send('evaluate',{'expression':'len(label)','frameId':frames[0]['id'],'context':'hover'})
                    assert length['success'] and length['body']['result']=='5',length
                    if args.scenario=='mutate-oracle':
                        assert client.send('setVariable',{'variablesReference':scope,'name':'doubled','value':'99'})['success']
                        assert client.send('scopes',{'frameId':frames[0]['id']})['success']
                        fresh=client.send('stackTrace',{'threadId':stopped['body']['threadId']})['body']['stackFrames']
                        changed=client.send('evaluate',{'expression':'doubled','frameId':fresh[0]['id']})
                        assert changed['success'] and changed['body']['result']=='99',changed
            assert client.send('continue',{'threadId':stopped['body']['threadId']})['success']
        original_finish(client,case,retired,retired_frames,expected_stdout='103\n' if args.scenario=='mutate-oracle' else None)
        if args.scenario=='logpoint':
            visits=next(stop for stop in case['stops'] if stop['marker']=='loop_body')['visits']
            expected=[f"index={visit['index']},sum={visit['sum']}\n" for visit in visits]
            observed=[item['body']['output'] for item in client.events
                      if item['event']=='output' and item['body']['category']=='console']
            assert observed==expected,(observed,expected)
            assert not any(item['event']=='stopped' for item in client.events)
        print(f'Original stdio {args.scenario}: fixed Source oracle, real framed process and exact target output PASS')
    finally:
        if client.process.poll() is None:
            client.process.kill()
            client.process.wait(timeout=20)

def original_breakpoints(client,breakpoints):
    reply=client.send('setBreakpoints',{'source':{'path':args.fixture.resolve().as_posix()},'breakpoints':breakpoints})
    assert reply['success'],reply
    actual=reply['body']['breakpoints']
    assert len(actual)==len(breakpoints) and all(item['verified'] for item in actual),reply
    assert [item['line'] for item in actual]==[item['line'] for item in breakpoints],reply


def original_frames(client,thread):
    reply=client.send('stackTrace',{'threadId':thread})
    assert reply['success'],reply
    frames=reply['body']['stackFrames']
    assert frames and all(frame['id']>0 for frame in frames),frames
    assert Path(frames[0]['source']['path']).resolve()==args.fixture.resolve(),frames
    return frames


def original_locals(client,frame,expected):
    reply=client.send('scopes',{'frameId':frame['id']})
    assert reply['success'],reply
    scope=reply['body']['scopes'][0]['variablesReference']
    reply=client.send('variables',{'variablesReference':scope})
    assert reply['success'],reply
    observed={value['name']:value['value'] for value in reply['body']['variables']}
    assert all(observed.get(name)==str(value) for name,value in expected.items()),(observed,expected)


def original_control_wire():
    client=original_session()
    try:
        stepping=args.scenario.startswith('source-')
        case=oracle_case('stack-and-step' if stepping else 'conditional-and-hit')
        assert client.send('launch',{'program':args.fixture.resolve().as_posix()})['success']
        marker=('inner_update' if args.scenario=='source-step-out' else 'outer_call') if stepping else 'loop_body'
        breakpoint={'line':marker_line(marker)}
        if args.scenario=='condition-oracle':breakpoint['condition']='index == 2'
        elif args.scenario in ('hit-oracle','replace-oracle'):breakpoint['hitCondition']='3'
        if args.scenario=='replace-oracle':breakpoint['condition']='index == 2'
        original_breakpoints(client,[breakpoint])
        assert client.send('configurationDone')['success']
        stopped=client.event('stopped')
        assert stopped['body']['reason']=='breakpoint',stopped
        thread=stopped['body']['threadId']
        frames=original_frames(client,thread)
        assert frames[0]['line']==marker_line(marker),frames
        assert not any(item['event']=='output' for item in client.events)
        retired_frames=[frame['id'] for frame in frames]
        if stepping:
            expected=next(stop for stop in case['stops'] if stop['marker']==marker)
            assert frames[0]['name']==expected['function'],frames
            original_locals(client,frames[0],expected['locals'])
            if args.scenario=='source-step-in':
                assert not client.send('evaluate',{'frameId':frames[0]['id'],'expression':'missing_name'})['success']
                assert not client.send('variables',{'variablesReference':2147483647})['success']
                original_locals(client,frames[0],expected['locals'])
            original_breakpoints(client,[])
            command={'source-step-in':'stepIn','source-next':'next','source-step-out':'stepOut'}[args.scenario]
            assert client.send(command,{'threadId':thread})['success']
            stopped=client.event('stopped')
            assert stopped['body']['reason']=='step' and stopped['body']['threadId']==thread,stopped
            fresh=original_frames(client,thread)
            assert not set(retired_frames).intersection(frame['id'] for frame in fresh),fresh
            for frame in retired_frames:assert not client.send('scopes',{'frameId':frame})['success']
            assert not client.send('evaluate',{'frameId':retired_frames[0],'expression':'value'})['success']
            if args.scenario=='source-step-in':
                assert fresh[0]['name']=='inner' and fresh[1]['name']=='outer',fresh
                assert len(fresh)==len(frames)+1,fresh
                original_locals(client,fresh[0],{'value':20})
                original_locals(client,fresh[1],expected['locals'])
            else:
                expected=next(stop for stop in case['stops'] if stop['marker']=='outer_return')
                assert fresh[0]['name']==expected['function'] and fresh[0]['line']==marker_line('outer_return'),fresh
                assert len(fresh)==len(frames)-(1 if args.scenario=='source-step-out' else 0),fresh
                original_locals(client,fresh[0],expected['locals'])
            retired_frames.extend(frame['id'] for frame in fresh)
        else:
            visits=next(stop for stop in case['stops'] if stop['marker']==marker)['visits']
            original_locals(client,frames[0],visits[2])
            if args.scenario in ('hit-oracle','replace-oracle'):
                if args.scenario=='replace-oracle':
                    original_breakpoints(client,[{'line':marker_line(marker),'condition':'index == 3','hitCondition':'==1'}])
                assert client.send('continue',{'threadId':thread})['success']
                stopped=client.event('stopped')
                assert stopped['body']['reason']=='breakpoint' and stopped['body']['threadId']==thread,stopped
                fresh=original_frames(client,thread)
                assert fresh[0]['name']=='accumulate' and fresh[0]['line']==marker_line(marker),fresh
                original_locals(client,fresh[0],visits[3])
                for frame in retired_frames:assert not client.send('scopes',{'frameId':frame})['success']
                retired_frames.extend(frame['id'] for frame in fresh)
                if args.scenario=='replace-oracle':original_breakpoints(client,[])
        assert client.send('continue',{'threadId':thread})['success']
        original_finish(client,case,retired_frames=retired_frames)
        assert not any(item['event']=='stopped' for item in client.events),client.events
        print(f'Original Source {args.scenario}: independent control, fixed locals/output and retired frames PASS')
    finally:
        if client.process.poll() is None:
            client.process.kill()
            client.process.wait(timeout=20)


if args.scenario=='legacy':legacy_wire()
elif args.scenario.startswith('source-') or args.scenario in ('condition-oracle','hit-oracle','replace-oracle'):original_control_wire()
else:original_wire()
