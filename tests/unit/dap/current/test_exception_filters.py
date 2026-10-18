"""Observed-VM exception policies on actual stdio and TCP transports."""
import hashlib,json

def exception_filters(Client,args):
    digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    text=args.fixture.read_text(encoding='utf8')
    def line(marker):
        matches=[n for n,t in enumerate(text.splitlines(),1) if 'FILTER_SITE: '+marker in t]
        assert len(matches)==1,(marker,matches)
        return matches[0]
    sites=[('RootHandled',False,True),('ChildHandled',True,True),('ChildStored',True,False),('Last',False,False)]
    policies=[[],['all'],['caught'],['uncaught'],['caught','uncaught'],['all','caught','uncaught']]
    record={'full_p16_qualified':False,'binary_sha256':digest(args.xray),'fixture_sha256':digest(args.fixture),'cases':[]}
    for filters in policies:
        client=Client();case={'filters':filters,'stops':[]};record['cases'].append(case)
        def send(command,arguments=None,success=True):
            reply=client.send(command,arguments);assert reply['success']==success,reply
            return reply.get('body',{})
        expected=[s for s in sites if 'all' in filters or ('caught' if s[2] else 'uncaught') in filters]
        try:
            caps=send('initialize');assert [f['filter'] for f in caps['exceptionBreakpointFilters']]==['all','caught','uncaught']
            client.event('initialized');send('setExceptionBreakpoints',{'filters':filters})
            send('setExceptionBreakpoints',{'filters':['native']},False)
            send(args.start_request,{'program':str(args.fixture.resolve())});send('configurationDone')
            for index,(name,child,caught) in enumerate(expected):
                stopped=client.event('stopped')['body'];assert stopped['reason']=='exception' and stopped['allThreadsStopped'],stopped
                thread=stopped['threadId'];assert (thread>1)==child,(thread,name)
                stack=send('stackTrace',{'threadId':thread})['stackFrames'];assert stack and stack[0]['line']==line(name),(name,stack)
                info=send('exceptionInfo',{'threadId':thread});assert info['exceptionId']=='FilterFailure.'+name,info
                mode='always' if 'all' in filters or caught else 'unhandled';assert info['breakMode']==mode,info
                case['stops'].append({'exception':name,'thread':thread,'line':stack[0]['line'],'breakMode':mode})
                if index+1==len(expected):
                    send('setExceptionBreakpoints',{'filters':[]})
                    assert send('exceptionInfo',{'threadId':thread})['breakMode']==mode
                send('continue',{'threadId':thread})
            client.event('terminated')
            received=[item['message'] for item in client.transcript if item['direction']=='receive']
            assert len([m for m in received if m.get('event')=='stopped'])==len(expected),received
            exited=[m for m in received if m.get('event')=='exited'];assert len(exited)==1 and exited[0]['body']['exitCode']!=0,exited
            output=''.join(m['body']['output'] for m in received if m.get('event')=='output')
            assert output=='1\n2\n3\n',repr(output)
            send('exceptionInfo',{'threadId':1},False);send('disconnect');client.close()
            case['state']='PASS';case['messages']=client.transcript
        finally:
            if client.process.poll() is None:client.process.kill();client.process.wait(timeout=20)
            if args.transcript:
                args.transcript.parent.mkdir(parents=True,exist_ok=True)
                args.transcript.write_text(json.dumps(record,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
    assert digest(args.xray)==record['binary_sha256'] and digest(args.fixture)==record['fixture_sha256']
    print('Real '+args.transport+':6 exception policies, root/Task local handlers and escapes, captured modes and original output/termination PASS')
