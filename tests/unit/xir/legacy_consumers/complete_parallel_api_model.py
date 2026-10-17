"""Whole parallel API observations across contract-permitted plan lane counts."""
from pathlib import Path
from functools import lru_cache
import hashlib,itertools,json

ROOT=Path(__file__).resolve().parents[4]
PROGRAMS={'parallel_api_reference':'tests/diff/cases/semantics/stdlib/parallel_api_reference.xr',
          'parallel_plan_close_lifecycle':'tests/diff/cases/semantics/stdlib/parallel_plan_close_lifecycle.xr'}
KIND='finite_complete_parallel_plan_lanes'
IDENTITIES=frozenset('complete_'+name for name in PROGRAMS)
FACTS='tests/unit/xir/legacy_consumers/parallel_plan_contract_facts.json'
AUTHORITIES=('spec/source/sections/003-2-type-system.md',
 'spec/source/sections/005-4-statements.md',
 'spec/source/sections/006-5-declarations.md',
 'spec/source/sections/009-8-error-handling.md',
 'spec/source/sections/011-10-concurrency-and-coroutines.md',
 'spec/source/sections/014-13-built-in-functions.md',
 'spec/source/sections/015-14-built-in-type-methods.md',
 'spec/source/sections/016-15-standard-library.md',
 'stdlib/parallel/parallel.xr',
 'stdlib/types/atomic.xr')
_INPUTS={'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677',
 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/009-8-error-handling.md': '3679ded015ad0222df39070b598fe26c8099247f9e410305a3f1527b9e63161b',
 'spec/source/sections/011-10-concurrency-and-coroutines.md': '27c232a9575d0d40e800ea5730e5d2b96e8f2e62e646731a3d9de2e0eea35aea',
 'spec/source/sections/014-13-built-in-functions.md': '2a4af98d2d429babb3cdcd875279f0d798efcabb852afd2c366e69a12ca50380',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'spec/source/sections/016-15-standard-library.md': 'c776ecb898f3dfbbac76e6e9eb08da0697041af5b118eb55bfd294a9630ba2fa',
 'stdlib/parallel/parallel.xr': '013000e9964fa136e73f47210f4afadf0f27798dea74baff6761ea93fbcc7cad',
 'stdlib/types/atomic.xr': '4da0e8355fdbb9e22a2cc5552f234d171b389302ce29698a19399ec40b2f67e4',
 'tests/diff/cases/semantics/stdlib/parallel_api_reference.xr': '7f7798834c8e4c529dd348f860ad4d87def0c13677727f7315bc431df9de2578',
 'tests/diff/cases/semantics/stdlib/parallel_plan_close_lifecycle.xr': '48cd46bde2cf2996b36d71f880bdf6807a6ee09ec90de77e9d4561aaf59c424b',
 'tests/unit/xir/legacy_consumers/parallel_plan_contract_facts.json': 'c82843ff274b0c14ee06d921be8589c3f34931d721e6dc139252cbc2605766c8'}


class ClosedPlan(Exception):pass


class LogicalPlan:
    def __init__(self,requested,actual,initialize):
        if type(actual) is not int or type(requested) is not int or not 1<=actual<=requested<=3:
            raise ValueError('Selected programs request at most three actual plan lanes')
        self.lanes=actual;self.states=[initialize(lane) for lane in range(actual)]
        self.closed=False;self.active=False;self.ends=0;self.logical_clears=0

    def begin(self):
        if self.closed:raise ClosedPlan()
        if self.active:raise ValueError('A selected plan cannot overlap dispatches')
        self.active=True

    def end(self):
        if not self.active:raise ValueError('No admitted active dispatch to end')
        self.active=False;self.ends+=1

    def assignments(self,items,lanes):
        if len(lanes)!=len(items) or any(type(lane) is not int or not 0<=lane<self.lanes for lane in lanes):
            raise ValueError('Every complete input item needs one existing plan state')

    def map(self,items,lanes,body):
        self.assignments(items,lanes);self.begin()
        try:return [body(self.states[lane],item) for item,lane in zip(items,lanes)]
        finally:self.end()

    def reduce(self,items,lanes,initial,body):
        # Selected reducers are integer addition; no arbitrary effect interpreter.
        return initial+sum(self.map(items,lanes,body))

    def for_each(self,items,body):
        self.begin()
        try:
            for item in items:body(item)
        finally:self.end()

    def close(self):
        if self.active:raise ValueError('Active close is a separate existing obligation')
        if not self.closed:
            self.closed=True;self.states.clear();self.logical_clears+=1


def close_trace(actual_lanes,assignment=None):
    if assignment is None:assignment=tuple(i%actual_lanes for i in range(3)) if type(actual_lanes) is int and actual_lanes>0 else ()
    plan=LogicalPlan(2,actual_lanes,lambda lane:lane+1)
    result=plan.reduce(tuple(range(3)),assignment,0,lambda state,item:state+item)
    plan.close();callback_values=[];branch='miss'
    try:plan.for_each(tuple(range(1)),lambda item:callback_values.append(item))
    except ClosedPlan:branch='closed'
    plan.close()
    return {'values':[result,branch,'done'],'actual_plan_lanes':actual_lanes,'item_lanes':list(assignment),
        'reducer_items':3,'successful_dispatch_ends':plan.ends,'postclose_callback_values':callback_values,
        'logical_state_clear_count':plan.logical_clears,'physical_release_or_actual_parallelism_qualified':False}


def api_trace(actual_lanes,reduce_lanes=None,map_lanes=None):
    if reduce_lanes is None:reduce_lanes=tuple(i%actual_lanes for i in range(4)) if type(actual_lanes) is int and actual_lanes>0 else ()
    if map_lanes is None:map_lanes=tuple(i%actual_lanes for i in range(3)) if type(actual_lanes) is int and actual_lanes>0 else ()
    seen=sum(range(5));mapped=[item*item for item in range(4)]
    before=[99];output=[item+10 for item in range(1,4)]
    total=10+sum(range(5));initialized=[]
    def initialize(lane):initialized.append(lane);return 100+lane
    plan=LogicalPlan(3,actual_lanes,initialize)
    init_sum=sum(initialized)
    plan_sum=plan.reduce(tuple(range(4)),reduce_lanes,0,lambda state,item:state+item)
    plan_values=plan.map(tuple(range(3)),map_lanes,lambda state,item:state+item)
    plan.close()
    values=[seen,len(mapped),mapped[0],mapped[3],len(output),output[0],output[2],total,
            init_sum,plan_sum,100<=plan_values[0]<=102,102<=plan_values[2]<=104]
    return {'values':values,'requested_plan_lanes':3,'actual_plan_lanes':actual_lanes,
        'initialized_logical_lanes':initialized,'init_sum':init_sum,'plan_sum':plan_sum,
        'reduce_item_lanes':list(reduce_lanes),'map_item_lanes':list(map_lanes),'ordered_plan_map_values':plan_values,
        'mapInto_before':before,'mapInto_after':output,'free_callback_item_counts':[5,4,3,5],
        'plan_reduce_items':4,'plan_map_items':3,'successful_plan_dispatch_ends':plan.ends,
        'logical_state_clear_count':plan.logical_clears,
        'printed_init_sum_does_not_prove_each_runtime_initializer_identity_or_count':True,
        'actual_lane_start_parallelism_resources_lifetime_FI_safety_provider_mixed_qualified':False}


def render(values):
    return ''.join((str(value).lower() if type(value) is bool else str(value))+'\n' for value in values)


@lru_cache(maxsize=2)
def output_options(name):
    if name=='parallel_plan_close_lifecycle':
        values={render(close_trace(lanes,assignment)['values']) for lanes in (1,2)
                for assignment in itertools.product(range(lanes),repeat=3)}
    elif name=='parallel_api_reference':
        values={render(api_trace(lanes,assignment)['values']) for lanes in (1,2,3)
                for assignment in itertools.product(range(lanes),repeat=4)}
    else:raise ValueError('Unreviewed complete parallel original')
    return tuple(sorted(values))


def validate_inputs():
    if set(_INPUTS)!=set(PROGRAMS.values())|set(AUTHORITIES)|{FACTS}:
        raise ValueError('Parallel API domain lost a complete original or authority')
    for name,digest in _INPUTS.items():
        if hashlib.sha256((ROOT/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:
            raise ValueError('Complete parallel API input changed: '+name)
    facts=json.loads((ROOT/FACTS).read_text(encoding='utf-8'))
    if not all(facts[key] for key in ('workers_is_maximum_request_not_identity_count','structured_completion',
                                      'map_keeps_input_order','mapInto_overwrites_not_appends','plan_state_persists','close_is_idempotent')):
        raise ValueError('Published parallel contract needs explicit review')


def expected_rows():
    validate_inputs();rows=[]
    for name,source in PROGRAMS.items():
        reference=api_trace(3) if name=='parallel_api_reference' else close_trace(2)
        options=list(output_options(name));count=12 if name=='parallel_api_reference' else 3
        rows.append({'id':'complete_'+name,'source':source,'original_source':source,'required_target':None,
            'expected':{'exit_code':0,'stderr_utf8':'','stdout_utf8':render(reference['values']),
                'stdout_expectation_kind':KIND,'stdout_utf8_options':options},'observation_count':count,
            'reference_source_trace_not_mandatory_worker_schedule':reference,
            'required_complete_output_options':len(options),
            'model':'Whole original parallel API/Plan program;all imports/options/ranges/callbacks/Atomic operations/mapInto initial99/close/catch/prints retained. Published workers is amaximum anditem/lane order is not fixed. Enumerate actual lanes1..requested andall one-state-per-item assignments,not onlybootstrap modulo reference. Plan init sum andsubsequentreduce share the same constructed state vector;reject uncoupled Cartesian outputs. Full12line API domain15 orfull3line close domain4;no arbitrary regex/truncation orassumed actual multithreading. Source reference7/409 are representatives,not universal worker counts. Real initializer/item identity counts,scheduler parallelism,cleanup/FI/resources/lifetime/safety/provider/mixed remain open.'})
    return rows


def expected_outputs(oracle):
    owned=next((row for row in expected_rows() if row['id']==oracle.get('id')),None)
    if (owned is None or oracle.get('source')!=owned['source'] or oracle.get('original_source')!=owned['original_source'] or
            oracle.get('expected')!=owned['expected'] or oracle.get('observation_count')!=owned['observation_count']):
        raise ValueError('Unowned or changed complete parallel output domain')
    return tuple(text.encode('utf-8') for text in owned['expected']['stdout_utf8_options'])


def matches(oracle,result):
    outputs=expected_outputs(oracle)
    return result is not None and not result.timed_out and result.returncode==0 and not result.stderr and result.stdout in outputs
