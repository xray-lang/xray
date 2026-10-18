"""Actual Source throws stop before local routing and handler side effects."""
import hashlib
import json
import re
import subprocess
from pathlib import Path

def throw_sites(Client,args):
    client=Client();record={'state':'RUNNING','transport':args.transport}
    def send(command,arguments=None,success=True):
        reply=client.send(command,arguments);assert reply['success'] is success,reply
        return reply.get('body',{})
    def output():
        return ''.join(item['body']['output'] for item in client.events
            if item['event']=='output' and item['body']['category']=='stdout')
    def marker(name):
        lines=[number for number,line in enumerate(args.fixture.read_text(encoding='utf8').splitlines(),1)
            if 'THROW_SITE: '+name in line]
        assert len(lines)==1,(name,lines)
        return lines[0]
    try:
        record['binary_sha256']=hashlib.sha256(args.xray.read_bytes()).hexdigest()
        record['fixture_sha256']=hashlib.sha256(args.fixture.read_bytes()).hexdigest()
        version=subprocess.run([str(args.xray),'--version','--json'],capture_output=True,check=True,timeout=20)
        record['cli_identity']=json.loads(version.stdout)
        send('initialize',{'supportsVariableType':True});client.event('initialized')
        send('setExceptionBreakpoints',{'filters':['all']})
        send(args.start_request,{'program':str(args.fixture.resolve())});send('configurationDone')
        expected=[('local','local','',1),('inner','nested','1\n7\n',1),
            ('rethrow','nested','1\n7\n',2),('generic','generic','1\n7\n2\n11\n',2),
            ('generic','generic','1\n7\n2\n11\n13\n',2)]
        references=[];saved=[];generic_names=[]
        for name,function,prefix,stage in expected:
            event=client.event('stopped');assert event['body']['reason']=='exception',event
            thread=event['body']['threadId'];frames=send('stackTrace',{'threadId':thread})['stackFrames']
            if function=='generic':
                assert re.fullmatch(r'generic\$\d+:\d+(?:@\d+:\d+)*',frames[0]['name']),frames
                assert frames[0]['name'] not in generic_names,frames
                generic_names.append(frames[0]['name'])
            else:assert frames[0]['name']==function,frames
            assert frames[0]['line']==marker(name),frames
            assert Path(frames[0]['source']['path']).resolve()==args.fixture.resolve()
            assert output()==prefix,(name,repr(output()),repr(prefix))
            exception=send('exceptionInfo',{'threadId':thread})
            assert exception['exceptionId']=='SiteFailure.Bad' and exception['breakMode']=='always',exception
            saved.append(exception)
            scopes=send('scopes',{'frameId':frames[0]['id']})['scopes']
            if function=='generic':
                local=next(scope['variablesReference'] for scope in scopes if scope['name']=='Locals')
                parameters=send('variables',{'variablesReference':local})['variables']
                value=next(value for value in parameters if value['name']=='value')
                assert (value['value'],value['type'])==(('13','i64') if len(generic_names)==1 else ('x','string')),value
            globals_scope=next(scope['variablesReference'] for scope in scopes if scope['name']=='Globals')
            values=send('variables',{'variablesReference':globals_scope})['variables']
            assert next(value['value'] for value in values if value['name']=='stage')==str(stage),values
            for reference in references:send('variables',{'variablesReference':reference},False)
            references.append(globals_scope)
            send('continue',{'threadId':thread})
        exited=client.event('exited');assert exited['body']['exitCode']==0,exited
        client.event('terminated');assert output()=='1\n7\n2\n11\n13\nx\n',repr(output())
        assert not any(item['event']=='stopped' for item in client.events),client.events
        for reference in references:send('variables',{'variablesReference':reference},False)
        send('disconnect');client.close();assert not client.stderr,client.stderr
        assert all(value['exceptionId']=='SiteFailure.Bad' for value in saved)
        assert hashlib.sha256(args.xray.read_bytes()).hexdigest()==record['binary_sha256']
        assert hashlib.sha256(args.fixture.read_bytes()).hexdigest()==record['fixture_sha256']
        record['state']='PASS';record['stops']=len(expected);record['stdout_utf8']=output();record['generic_names']=generic_names
        print('Source throw sites: local/nested/rethrow/generic, before-handler effects, owned values and retired references PASS')
    except BaseException as error:
        record['state']='FAIL';record['error']=repr(error);raise
    finally:
        if client.process.poll() is None:client.process.kill();client.process.wait(timeout=20)
        record['messages']=client.transcript;record['process_exit']=client.process.returncode
        if args.transcript:
            args.transcript.parent.mkdir(parents=True,exist_ok=True)
            args.transcript.write_text(json.dumps(record,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
