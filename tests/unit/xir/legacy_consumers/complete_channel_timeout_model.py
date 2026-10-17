"""Whole timed-channel original through finite FIFO and single-winner events."""
from pathlib import Path
import hashlib
from complete_async_events_model import Channel
ROOT=Path(__file__).resolve().parents[4]
SOURCE='tests/regression/11_coroutine/1123_channel_timeout.xr'
IDENTITY='complete_channel_timeout_1123'
KIND='finite_complete_channel_timeout_fifo'
AUTHORITIES=('spec/source/sections/003-2-type-system.md', 'spec/source/sections/005-4-statements.md', 'spec/source/sections/006-5-declarations.md', 'spec/source/sections/009-8-error-handling.md', 'spec/source/sections/011-10-concurrency-and-coroutines.md', 'spec/source/sections/014-13-built-in-functions.md', 'spec/source/sections/015-14-built-in-type-methods.md', 'stdlib/types/channel.xr', 'stdlib/types/coroutine.xr', 'tests/unit/xir/legacy_consumers/complete_async_events_model.py')
_INPUTS={'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677', 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915', 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d', 'spec/source/sections/009-8-error-handling.md': '3679ded015ad0222df39070b598fe26c8099247f9e410305a3f1527b9e63161b', 'spec/source/sections/011-10-concurrency-and-coroutines.md': '27c232a9575d0d40e800ea5730e5d2b96e8f2e62e646731a3d9de2e0eea35aea', 'spec/source/sections/014-13-built-in-functions.md': '2a4af98d2d429babb3cdcd875279f0d798efcabb852afd2c366e69a12ca50380', 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e', 'stdlib/types/channel.xr': 'ff2be02bcc28be44f7927b746d316eec7fef4e815f2693207ef07176bbbf1284', 'stdlib/types/coroutine.xr': 'f85c868bf12b2b90988ead5cf746fd8e522d5c2d6398a64882b47637c72dc7a6', 'tests/unit/xir/legacy_consumers/complete_async_events_model.py': '45a54d201c792ed69189e26e3e3f759a7bca543719324fa72ce4ed81d257cd8f', 'tests/regression/11_coroutine/1123_channel_timeout.xr': '1c2037b29d6d6bd9edc7c9a9267b1d9a9a93da6622ca8069a41583408d2fe42e'}


def fixed_prefix_trace():
    ready=Channel(1);ready.send(42);first,value=ready.timed_recv_without_partner()
    empty=Channel(1);second,_=empty.timed_recv_without_partner()
    space=Channel(1);third=space.timed_send_without_partner(100);_,sent=space.recv()
    full=Channel(1);full.send(1);fourth=full.timed_send_without_partner(2)
    lines=['test1: recvTimeout with data','  result: '+str(value)+' ok: '+str(first=='Value').lower(),
           'test2: recvTimeout timeout','  '+second.lower(),
           'test3: sendTimeout with space','  success: '+str(third=='Sent').lower(),'  value: '+str(sent),
           'test4: sendTimeout timeout','  success: '+str(fourth=='Sent').lower(),
           'test5: producer-consumer with timeout']
    return {'lines':lines,'queues_after_first_four':[ready.queue,empty.queue,space.queue,full.queue],
            'first_four_channels_still_open':not any(ch.closed for ch in (ready,empty,space,full))}


from functools import lru_cache


def successors(state):
    producer,consumer,queue,pending,count,closed,log=state
    out=[]
    def step(label,p=producer,c=consumer,q=queue,v=pending,n=count,z=closed,l=log):
        assert 0<=len(q)<=2 and 0<=n<=3
        assert c!=1 or (not q and not z)
        out.append((label,(p,c,q,v,n,z,l)))
    if producer in (0,1,2):
        value=producer+1;next_producer=producer+1 if producer<2 else 5
        if consumer==1:step('send-direct-'+str(value),p=next_producer,c=2,v=value)
        elif len(queue)<2:step('send-buffer-'+str(value),p=next_producer,q=queue+(value,))
        else:
            assert producer==2
            step('wait-send3',p=3)
    elif producer==3:
        assert len(queue)==2
        step('send3-timeout-wins',p=4)
    elif producer==4:step('print-producer-timeout3',p=5,l=log+(('timeout',3),))
    elif producer==5:step('close',p=6,c=3 if consumer==1 else consumer,z=True)
    if consumer==0:
        if queue:
            remaining=queue[1:];next_producer=producer
            if producer==3:remaining+=(3,);next_producer=5
            step('receive-buffer-'+str(queue[0]),p=next_producer,c=2,q=remaining,v=queue[0])
        elif closed:step('receive-closed',c=3)
        else:step('wait-receive',c=1)
    elif consumer==1:step('receive-timeout-wins',c=3)
    elif consumer==2:step('print-consumed-'+str(pending),c=0,v=None,n=count+1,l=log+(('consume',pending),))
    return out


@lru_cache(maxsize=1)
def enumerate_traces():
    start=(0,0,(),None,0,False,());work=[(start,())];seen={start};final={};edges=0
    while work:
        state,steps=work.pop()
        if state[0]==6 and state[1]==3:
            final[(state[-1],state[4])]=steps;continue
        for label,following in successors(state):
            edges+=1
            if following not in seen:seen.add(following);work.append((following,steps+(label,)))
    return final,len(seen),edges



def render(log,count):
    lines=fixed_prefix_trace()['lines']
    for event,value in log:
        if event=='consume':lines.append('  consumed: '+str(value))
        elif event=='timeout' and value==3:lines.append('  producer timeout at 3')
        else:raise ValueError('Unreviewed complete-channel event')
    lines.append('  total consumed: '+str(count))
    return ''.join(line+'\n' for line in lines)


def output_options():
    traces,_,_=enumerate_traces()
    return sorted(render(log,count) for log,count in traces)


def validate_inputs():
    if set(_INPUTS)!={SOURCE,*AUTHORITIES}:raise ValueError('Whole channel timeout source or authority dropped')
    for name,digest in _INPUTS.items():
        if hashlib.sha256((ROOT/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:
            raise ValueError('Complete channel timeout input changed: '+name)


def expected_rows():
    validate_inputs();traces,states,edges=enumerate_traces();options=output_options()
    representative=render((('consume',1),('consume',2),('consume',3)),3)
    return [{'id':IDENTITY,'source':SOURCE,'original_source':SOURCE,'required_target':None,
        'expected':{'exit_code':0,'stderr_utf8':'','stdout_utf8':representative,
                    'stdout_expectation_kind':KIND,'stdout_utf8_options':options},
        'observation_count':14,'observation_count_scope':'Line count of the complete representative;all owned whole paths have12..14lines',
        'complete_output_line_counts':sorted({len(text.splitlines()) for text in options}),
        'required_semantic_trace':{'states':states,'edges':edges,'whole_terminal_observations':len(traces),
            'witnesses':[{'events':log,'consumed_count':count,'steps':steps} for (log,count),steps in sorted(traces.items())],
            'fixed_first_four':fixed_prefix_trace(),'original_capacities':[1,1,1,1,2],
            'original_send_timeout_milliseconds':[1000,100,500],'original_receive_timeout_milliseconds':[1000,100,200],
            'original_producer_items':[1,2,3],'original_go_calls':2,'original_plain_awaits':2,'original_close_calls':1,
            'original_static_print_expressions':15,'complete_normal_print_execution_counts':[12,13,14],
            'producer_timeout_only_item3':True,'producer_timeout_consumed_counts':[0,2],
            'actual_timer_latency_fairness_physical_release_or_runtime_FI_safety_provider_mixed_qualified':False},
        'separate_original_named_collection_obligation':{'names':['test_channel_timeout'],'qualified_by_root_run':False},
        'model':'Whole unchanged1123 root andnamedwrapper/allfivechannels/twogo/twoawaits/alltimeouts/range1..4/close/all15staticprints retained. Firstfour cases independently derive10fixedlines. Finite event enumeration keeps FIFO capacity2,onewinner perpendingoperation,directdelivery,bufferedrecv promoting pendingthirdsend,timeoutcommit separatefromprint,close draining androotjoiningbothworkers.7completeoutputs12..14lines:withoutthirdsendtimeout consumersuccessprefix1..3;withtimeout3 count0or2,three printpositions when2,never1becausebuffered2cannotexpire. Finaltotal equalsactual consumed prints. Representative14line all3success isnot a universal deadline guarantee. No droppedlines/arbitraryregex/soleVMnativeequality. Actual clocks/fairness/resources/FI/safety/providers/mixed andoriginalCLI named hook remain open.'}]


def expected_outputs(oracle):
    owned=expected_rows()[0]
    if (oracle.get('id')!=IDENTITY or oracle.get('source')!=SOURCE or oracle.get('original_source')!=SOURCE or
            oracle.get('expected')!=owned['expected'] or oracle.get('observation_count')!=14 or
            oracle.get('complete_output_line_counts')!=[12,13,14]):
        raise ValueError('Unowned or changed complete channel timeout domain')
    return tuple(text.encode('utf-8') for text in owned['expected']['stdout_utf8_options'])


def matches(oracle,result):
    outputs=expected_outputs(oracle)
    return result is not None and not result.timed_out and result.returncode==0 and not result.stderr and result.stdout in outputs
