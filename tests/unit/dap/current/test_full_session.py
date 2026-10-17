"""A complete public CLI conversation over owned typed Source state."""
import hashlib
import json
from pathlib import Path
import subprocess

ORIGINAL_CAPABILITIES=(
    'supportsConfigurationDoneRequest','supportsConditionalBreakpoints','supportsHitConditionalBreakpoints',
    'supportsLogPoints','supportsFunctionBreakpoints','supportsTerminateRequest','supportsRestartRequest',
    'supportsSetVariable','supportsEvaluateForHovers','supportsDisassembleRequest',
    'supportTerminateDebuggee','supportsPauseRequest','supportsExceptionInfoRequest')

def full_session(Client,args):
    client=Client();record={'state':'RUNNING','transport':args.transport,'original_capability_count':13}
    digest=lambda path:hashlib.sha256(Path(path).read_bytes()).hexdigest()
    def send(command,arguments=None,success=True):
        reply=client.send(command,arguments);assert reply['success'] is success,reply
        return reply.get('body',{})
    def marker(name):
        found=[n for n,line in enumerate(args.fixture.read_text(encoding='utf8').splitlines(),1) if 'DAP_STOP: '+name in line]
        assert len(found)==1,(name,found)
        return found[0]
    def frames(thread):
        value=send('stackTrace',{'threadId':thread})['stackFrames']
        assert value and all(Path(frame['source']['path']).resolve()==args.fixture.resolve() for frame in value),value
        return value
    def scopes(frame):
        value=send('scopes',{'frameId':frame})['scopes']
        return {entry['name']:entry['variablesReference'] for entry in value}
    def variables(reference):
        return send('variables',{'variablesReference':reference})['variables']
    def named(reference):return {entry['name']:entry for entry in variables(reference)}
    def evaluate(frame,text,expected):
        value=send('evaluate',{'frameId':frame,'expression':text,'context':'hover'})
        assert value['result']==expected,(text,value,expected)
        return value
    def stopped(reason):
        event=client.event('stopped');assert event['body']['reason']==reason,event
        return event['body']['threadId']
    def ended(code):
        event=client.event('exited');assert event['body']['exitCode']==code,event
        client.event('terminated')
    def breakpoints(hold):
        lines=([marker('hold_loop')] if hold else [])+[marker('leaf_read')]
        reply=send('setBreakpoints',{'source':{'path':str(args.fixture.resolve())},
            'breakpoints':[{'line':line} for line in lines]})['breakpoints']
        assert len(reply)==len(lines) and all(entry['verified'] and entry['line']==line
            for entry,line in zip(reply,lines)),reply
    def release_hold():
        thread=stopped('breakpoint');entry=frames(thread)
        assert entry[0]['line']==marker('hold_loop'),entry
        send('pause',{'threadId':thread},False)
        breakpoints(False)
        send('continue',{'threadId':thread})
        send('pause',{'threadId':thread});thread=stopped('pause')
        threads=send('threads')['threads'];assert thread in [entry['id'] for entry in threads],threads
        entry=frames(thread);views=scopes(entry[0]['id'])
        assert named(views['Globals'])['hold']['value']=='true'
        send('setVariable',{'variablesReference':views['Globals'],'name':'hold','value':'false'})
        send('continue',{'threadId':thread})
    try:
        record['binary_sha256']=digest(args.xray);record['fixture_sha256']=digest(args.fixture)
        version=subprocess.run([str(args.xray),'--version','--json'],capture_output=True,check=True,timeout=20)
        record['cli_identity']=json.loads(version.stdout)
        capabilities=send('initialize',{'supportsVariableType':True,'supportsVariablePaging':True})
        missing=[name for name in ORIGINAL_CAPABILITIES if not capabilities.get(name)]
        assert missing==(['supportTerminateDebuggee'] if args.transport=='stdio' else []),missing
        record['missing_original_capabilities']=missing
        record['open_exception_filters']=['caught','uncaught','native']
        client.event('initialized')
        send('setExceptionBreakpoints',{'filters':['all']})
        send('setExceptionBreakpoints',{'filters':['uncaught']},False)
        send(args.start_request,{'program':str(args.fixture.resolve())})
        functions=send('setFunctionBreakpoints',{'breakpoints':[{'name':'outer'}]})['breakpoints']
        assert len(functions)==1 and functions[0]['verified'],functions
        breakpoints(True)
        send('configurationDone');release_hold()
        thread=stopped('breakpoint');entry=frames(thread)
        assert entry[0]['name']=='outer',entry
        send('next',{'threadId':thread});thread=stopped('step');call=frames(thread)
        assert call[0]['name']=='outer' and call[0]['line']==marker('outer_call'),call
        evaluate(call[0]['id'],'input','20')
        send('setFunctionBreakpoints',{'breakpoints':[]})
        send('stepIn',{'threadId':thread});thread=stopped('step');inside=frames(thread)
        assert inside[0]['name']=='leaf' and inside[1]['name']=='outer',inside
        evaluate(inside[0]['id'],'value','20')
        send('continue',{'threadId':thread});thread=stopped('breakpoint');leaf=frames(thread)
        assert leaf[0]['name']=='leaf' and leaf[0]['line']==marker('leaf_read'),leaf
        assert leaf[1]['name']=='outer' and leaf[1]['line']==marker('outer_call'),leaf
        instructions=send('disassemble',{'memoryReference':leaf[0]['instructionPointerReference'],'instructionCount':4})['instructions']
        assert instructions and instructions[0]['address']==leaf[0]['instructionPointerReference'],instructions
        assert len({entry['address'] for entry in instructions})==len(instructions) and all(entry['instruction'] for entry in instructions),instructions
        frame=leaf[0]['id'];views=scopes(frame);assert 'Locals' in views and 'Globals' in views,views
        local=views['Locals'];global_scope=views['Globals'];values=named(local)
        assert values['small']['value']=='1' and values['text']['value']=='old\0value'.replace('\0','\x00'),values
        assert named(global_scope)['shared']['value']=='2'
        old_array=values['items']['variablesReference'];assert old_array
        initial_array=[v['value'] for v in variables(old_array)];assert initial_array==['1','2']
        evaluate(leaf[1]['id'],'input','20');evaluate(frame,'len(items)+value','22')
        send('setVariable',{'variablesReference':local,'name':'small','value':'127'})
        send('variables',{'variablesReference':old_array},False)
        send('setVariable',{'variablesReference':local,'name':'optional','value':'null'})
        send('setVariable',{'variablesReference':local,'name':'optional','value':'7'})
        send('setVariable',{'variablesReference':local,'name':'items','value':'[]'})
        send('setVariable',{'variablesReference':local,'name':'items','value':'[3,7]'})
        current=named(local);array=current['items']['variablesReference'];assert array
        send('setVariable',{'variablesReference':array,'name':'[1]','value':'300'})
        send('variables',{'variablesReference':array},False)
        send('setVariable',{'variablesReference':array,'name':'[0]','value':'9'},False)
        fresh=named(local)['items']['variablesReference']
        assert [v['value'] for v in variables(fresh)]==['3','300']
        send('variables',{'variablesReference':old_array},False)
        assert initial_array==['1','2']
        send('setVariable',{'variablesReference':global_scope,'name':'shared','value':'65535'})
        send('setVariable',{'variablesReference':local,'name':'text','value':'"new\\0中"'})
        saved_text=named(local)['text']['value'];assert saved_text=='new\0中'.replace('\0','\x00'),repr(saved_text)
        for name,text in [('small','128'),('small','true'),('tag','"changed"')]:
            send('setVariable',{'variablesReference':local,'name':name,'value':text},False)
        send('setVariable',{'variablesReference':global_scope,'name':'shared','value':'65536'},False)
        send('evaluate',{'frameId':frame,'expression':'missing_name'},False)
        evaluate(frame,'small','127');evaluate(frame,'shared','65535');evaluate(frame,'len(items)+value','22')
        send('stepOut',{'threadId':thread});thread=stopped('step');outer=frames(thread)
        assert outer[0]['name']=='outer' and outer[0]['line']==marker('outer_return'),outer
        evaluate(outer[0]['id'],'result','23')
        for previous in leaf:send('scopes',{'frameId':previous['id']},False)
        for reference in (local,global_scope,old_array,array,fresh):send('variables',{'variablesReference':reference},False)
        send('continue',{'threadId':thread});thread=stopped('exception')
        exception=send('exceptionInfo',{'threadId':thread})
        assert exception['exceptionId']=='SessionFailure.Bad' and exception['breakMode']=='always',exception
        send('continue',{'threadId':thread});ended(0)
        output=''.join(e['body']['output'] for e in client.events if e['event']=='output' and e['body']['category']=='stdout')
        assert output=='127\nnew\0中\n65535\n2\n26\ncaught\n'.replace('\0','\x00'),repr(output)
        record['stdout_utf8']=output;client.events=[e for e in client.events if e['event']!='output']
        breakpoints(True);send('restart');release_hold()
        thread=stopped('breakpoint');again=frames(thread)
        assert again[0]['name']=='leaf' and again[0]['line']==marker('leaf_read'),again
        fresh_views=scopes(again[0]['id']);restored=named(fresh_views['Locals'])
        assert restored['small']['value']=='1' and named(fresh_views['Globals'])['shared']['value']=='2'
        assert not set(f['id'] for f in leaf).intersection(f['id'] for f in again),again
        send('terminate');ended(1)
        for reference in fresh_views.values():send('variables',{'variablesReference':reference},False)
        send('scopes',{'frameId':again[0]['id']},False)
        send('exceptionInfo',{'threadId':thread},False)
        assert not any(e['event']=='output' for e in client.events),client.events
        send('disconnect');client.close();assert not client.stderr,client.stderr
        assert saved_text=='new\0中'.replace('\0','\x00') and exception['exceptionId']=='SessionFailure.Bad'
        assert initial_array==['1','2']
        assert digest(args.xray)==record['binary_sha256'] and digest(args.fixture)==record['fixture_sha256']
        positive={item['message']['command'] for item in client.transcript
            if item['direction']=='receive' and item['message']['type']=='response' and item['message']['success']}
        expected={'initialize','configurationDone','setBreakpoints','setFunctionBreakpoints','setExceptionBreakpoints',
            'threads','stackTrace','scopes','variables','continue','next','stepIn','stepOut','pause','evaluate',
            'setVariable','exceptionInfo','disassemble','restart','terminate','disconnect',args.start_request}
        assert positive==expected,(positive,expected)
        record['positive_original_requests']=sorted(positive)
        record['state']='PASS';record['full_p16_qualified']=False
        print('Public CLI '+args.transport+': step/lexical/global/typed write/error/restart/cancel/retired references PASS')
    except BaseException as error:
        record['state']='FAIL';record['error']=repr(error);raise
    finally:
        if client.process.poll() is None:client.process.kill();client.process.wait(timeout=20)
        record['process_exit']=client.process.returncode;record['messages']=client.transcript
        if args.transcript:
            args.transcript.parent.mkdir(parents=True,exist_ok=True)
            args.transcript.write_text(json.dumps(record,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
