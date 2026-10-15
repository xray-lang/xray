"""Independent complete cleanup events and lazy iterator state transitions."""
from pathlib import Path
import hashlib

ROOT=Path(__file__).resolve().parent.parents[3]
_INPUTS = {'spec/source/sections/003-2-type-system.md': '916835c38312eccdada94aff018d22d79f3c15ed998048754759764c53fc8620',
 'spec/source/sections/004-3-expressions.md': '3aeabb14d154ef235fe51c665a7d367c2e51dc2c6f5261a9e0b194b228a9b945',
 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915',
 'spec/source/sections/006-5-declarations.md': '4b07469dd5109d4c6d0b99896e72c735e60e1821f716352ffd0e4da4e75d563b',
 'spec/source/sections/009-8-error-handling.md': '3679ded015ad0222df39070b598fe26c8099247f9e410305a3f1527b9e63161b',
 'spec/source/sections/011-10-concurrency-and-coroutines.md': '27c232a9575d0d40e800ea5730e5d2b96e8f2e62e646731a3d9de2e0eea35aea',
 'spec/source/sections/019-18-error-codes.md': '2c4416085d5d38f37fb0e434d863cba1ff5d5758d44efb29402fa5158282d9ad',
 'src/shared/xr_error_messages.h': '9cb2a61e702eb0f768e4a9e7fb1a1a3d7176cc603dfb955ca1e2415e6e8c615c',
 'stdlib/types/panic_info.xr': 'ae274d24d1e292dd84fe902fdec2ae93ad23596aef523613d2863bd08a946fb4',
 'tests/diff/cases/semantics/cleanup/defer_async_await.xr': '23d2da8b26019c7aa2a9d538020ef8e4226e03b7813193a23f84e0736b669353',
 'tests/diff/cases/semantics/cleanup/defer_block_assignment.xr': '2b0878462c2213f97c79d5b570d48982da5e8f0f85494639a375acfc30e09cdc',
 'tests/diff/cases/semantics/cleanup/defer_coroutine_local_panic_handler.xr': '1a0cc08ae89faabb4af397f6b57893796a60e627b54a356aa8423acdb66d1752',
 'tests/diff/cases/semantics/cleanup/defer_error_absorbed_inside_body.xr': '0a82ee68ab4c21aaaef9769cc95c1af54bbe09f4f48206a4aeb8d1b190005d9a',
 'tests/diff/cases/semantics/cleanup/defer_late_binding.xr': '2d76d665daeabd82d415a830bc89f95c34eec40229f4574c1031427f8a051eb0',
 'tests/diff/cases/semantics/cleanup/defer_nested_cleanup_and_local_control.xr': '6278f66dbd7c79d0e86f58e13f311c8040c19c98fe34f2f28e0cfce2f3be24b3',
 'tests/diff/cases/semantics/cleanup/defer_panic_caught_inside_body.xr': '214bcfbb98495ffed80aae0a86f5165e1138529ae8f1fcd443d8c99a54e5c20a',
 'tests/diff/cases/semantics/cleanup/defer_receiver_late_binding.xr': '37db838a4013658ea2f1a145d6e113e99ec442932ff1708d4ae1026b8692b57f',
 'tests/diff/cases/semantics/cleanup/defer_uncaught_panic.xr': '19736698a8c4e291a82b9cfd7e169631c6e9de7c7ab9a474973e172f0a1bceb5',
 'tests/diff/cases/semantics/generator/early_stop.xr': 'dd94528a6212158be4a953e0b3a992f8ff10a5bba29abd312f34e3a8c8282cea',
 'tests/diff/cases/semantics/generator/generator_in_coroutine.xr': 'd52372f8065a9c196d46edbb62df43276159f0fe31c324e064040be22ae21ca8',
 'tests/diff/cases/semantics/generator/iterator_exhausted.xr': '75ebf19581482a71e30232325e172a13fa472d15ee0aeef9a0cdb839a3c4d042',
 'tests/diff/cases/semantics/generator/iterator_param_loop_carried.xr': '9c86622d8e9877de859296903c581606f6be53aee565dc98ffb383540d98da60',
 'tests/diff/cases/semantics/generator/nested_generator.xr': '014c0d8aecedc942c7ca60e8af414eb351aaedd104c424860ba9eed770e76a7b',
 'tests/diff/cases/semantics/generator/value_yield.xr': '9863bc58d5f99dd6bfe5ef7543a0ab94375a6d6949843b696cc6547da5b76320',
 'tests/diff/cases/semantics/generator/yield_throw.xr': '4e9416214d2bef47b0e06fe4f1ee173864599b66c5aa106f8f7ec1735b208310',
 'tests/unit/xir/legacy_consumers/fixtures/cleanup_snapshot_binding/root.xr': '105fdacc5ac75f8f8b3bcb38f49a20fd4f74b0b3cbd2e09a530aba91d2f4695e'}
_REQUIRED_INPUTS = ('spec/source/sections/003-2-type-system.md',
 'spec/source/sections/004-3-expressions.md',
 'spec/source/sections/005-4-statements.md',
 'spec/source/sections/006-5-declarations.md',
 'spec/source/sections/009-8-error-handling.md',
 'spec/source/sections/011-10-concurrency-and-coroutines.md',
 'spec/source/sections/019-18-error-codes.md',
 'src/shared/xr_error_messages.h',
 'stdlib/types/panic_info.xr',
 'tests/diff/cases/semantics/cleanup/defer_async_await.xr',
 'tests/diff/cases/semantics/cleanup/defer_block_assignment.xr',
 'tests/diff/cases/semantics/cleanup/defer_coroutine_local_panic_handler.xr',
 'tests/diff/cases/semantics/cleanup/defer_error_absorbed_inside_body.xr',
 'tests/diff/cases/semantics/cleanup/defer_late_binding.xr',
 'tests/diff/cases/semantics/cleanup/defer_nested_cleanup_and_local_control.xr',
 'tests/diff/cases/semantics/cleanup/defer_panic_caught_inside_body.xr',
 'tests/diff/cases/semantics/cleanup/defer_receiver_late_binding.xr',
 'tests/diff/cases/semantics/cleanup/defer_uncaught_panic.xr',
 'tests/diff/cases/semantics/generator/early_stop.xr',
 'tests/diff/cases/semantics/generator/generator_in_coroutine.xr',
 'tests/diff/cases/semantics/generator/iterator_exhausted.xr',
 'tests/diff/cases/semantics/generator/iterator_param_loop_carried.xr',
 'tests/diff/cases/semantics/generator/nested_generator.xr',
 'tests/diff/cases/semantics/generator/value_yield.xr',
 'tests/diff/cases/semantics/generator/yield_throw.xr',
 'tests/unit/xir/legacy_consumers/fixtures/cleanup_snapshot_binding/root.xr')
_PROGRAMS = {'defer_async_await': 'tests/diff/cases/semantics/cleanup/defer_async_await.xr',
 'defer_block_assignment': 'tests/diff/cases/semantics/cleanup/defer_block_assignment.xr',
 'defer_coroutine_local_panic_handler': 'tests/diff/cases/semantics/cleanup/defer_coroutine_local_panic_handler.xr',
 'defer_error_absorbed_inside_body': 'tests/diff/cases/semantics/cleanup/defer_error_absorbed_inside_body.xr',
 'defer_late_binding': 'tests/diff/cases/semantics/cleanup/defer_late_binding.xr',
 'defer_nested_cleanup_and_local_control': 'tests/diff/cases/semantics/cleanup/defer_nested_cleanup_and_local_control.xr',
 'defer_panic_caught_inside_body': 'tests/diff/cases/semantics/cleanup/defer_panic_caught_inside_body.xr',
 'defer_receiver_late_binding': 'tests/diff/cases/semantics/cleanup/defer_receiver_late_binding.xr',
 'defer_uncaught_panic': 'tests/diff/cases/semantics/cleanup/defer_uncaught_panic.xr',
 'early_stop': 'tests/diff/cases/semantics/generator/early_stop.xr',
 'generator_in_coroutine': 'tests/diff/cases/semantics/generator/generator_in_coroutine.xr',
 'iterator_exhausted': 'tests/diff/cases/semantics/generator/iterator_exhausted.xr',
 'iterator_param_loop_carried': 'tests/diff/cases/semantics/generator/iterator_param_loop_carried.xr',
 'nested_generator': 'tests/diff/cases/semantics/generator/nested_generator.xr',
 'value_yield': 'tests/diff/cases/semantics/generator/value_yield.xr',
 'yield_throw': 'tests/diff/cases/semantics/generator/yield_throw.xr'}
_NAMES=frozenset({'defer_async_await','defer_block_assignment','defer_coroutine_local_panic_handler',
    'defer_error_absorbed_inside_body','defer_late_binding','defer_nested_cleanup_and_local_control',
    'defer_panic_caught_inside_body','defer_receiver_late_binding','defer_uncaught_panic',
    'early_stop','generator_in_coroutine','iterator_exhausted','iterator_param_loop_carried',
    'nested_generator','value_yield','yield_throw'})
NEXT_MESSAGE='Iterator.next() on an exhausted iterator: every next() must follow a hasNext() that returned true'
NTH_MESSAGE='Iterator.nth(index) ran past the end: the iterator was exhausted before reaching index'


class ValueErrorChannel(Exception): pass
class PanicChannel(Exception): pass


class CleanupScope:
    """Model observable LIFO events, without claiming a product allocation strategy."""
    def __init__(self): self.actions=[]
    def __enter__(self): return self
    def defer(self,action): self.actions.append(action)
    def __exit__(self,*exception):
        while self.actions: self.actions.pop()()
        return False


class Pull:
    def __init__(self,values):
        self.values=iter(values);self.ready=False;self.exhausted=False;self.value=None
    def has_next(self):
        if self.exhausted:return False
        if not self.ready:
            try:self.value=next(self.values)
            except StopIteration:self.exhausted=True;return False
            self.ready=True
        return True
    def next(self):
        if not self.has_next():raise PanicChannel(NEXT_MESSAGE)
        self.ready=False;return self.value
    def nth(self,index):
        for _ in range(index+1):
            if not self.has_next():raise PanicChannel(NTH_MESSAGE)
            value=self.next()
        return value


def cleanup_observations():
    traces={};out=[]
    identifier=7;connection={'id':identifier}
    with CleanupScope() as scope:
        scope.defer(lambda:out.append('close conn '+str(connection['id'])))
        scope.defer(lambda:out.append('defer-builtin '+str(identifier)))
        out.append('before-await '+str(identifier));result=identifier+1
        scope.defer(lambda:out.append('defer-after '+str(result)))
        out.append('after-await '+str(result));returned=result*10
    out += [str(returned),'done'];traces['defer_async_await']=out
    out=[];cell=[1]
    def assign():cell[0]=99;out.append('cleanup '+str(cell[0]))
    with CleanupScope() as scope:scope.defer(assign);out.append('body '+str(cell[0]))
    traces['defer_block_assignment']=out
    out=[]
    def absorb_panic(message):
        try:raise PanicChannel('division by zero')
        except PanicChannel:out.append(message)
    with CleanupScope() as scope:scope.defer(lambda:absorb_panic('cleanup caught panic'));returned=7
    out.append(str(returned));traces['defer_coroutine_local_panic_handler']=out
    out=[]
    def absorb_error():
        try:raise ValueErrorChannel('close')
        except ValueErrorChannel:out.append('cleanup-error-absorbed')
    try:
        with CleanupScope() as scope:scope.defer(absorb_error);raise ValueErrorChannel('BodyErr.Boom')
    except ValueErrorChannel as error:
        if str(error)!='BodyErr.Boom':raise
        out.append('body-error-survives-cleanup')
    out.append('done');traces['defer_error_absorbed_inside_body']=out
    out=[];cell=[1]
    with CleanupScope() as scope:
        scope.defer(lambda:out.append('late='+str(cell[0])))
        saved=cell[0];scope.defer(lambda:out.append('saved='+str(saved)))
        cell[0]=99;out.append('body x='+str(cell[0]))
    out.append('done');traces['defer_late_binding']=out
    out=[]
    def nested_cleanup():
        with CleanupScope() as scope:
            scope.defer(lambda:out.append('nested'))
            index=0
            while index<3:
                index+=1
                if index==1:continue
                out.append('loop '+str(index));break
            out.append('outer')
    with CleanupScope() as scope:scope.defer(nested_cleanup);out.append('body')
    traces['defer_nested_cleanup_and_local_control']=out
    out=[]
    with CleanupScope() as scope:scope.defer(lambda:absorb_panic('panic-absorbed-inside-defer'));out.append('body-ran')
    out.append('done');traces['defer_panic_caught_inside_body']=out
    out=[];receiver=[{'id':1}]
    with CleanupScope() as scope:
        scope.defer(lambda:out.append('closing conn '+str(receiver[0]['id'])))
        receiver[0]={'id':2};out.append('body done')
    traces['defer_receiver_late_binding']=out
    out=[]
    try:
        with CleanupScope() as outer:
            outer.defer(lambda:out.append('outer-defer'))
            with CleanupScope() as inner:
                inner.defer(lambda:out.append('inner-defer'));raise PanicChannel('division by zero')
    except PanicChannel:pass
    traces['defer_uncaught_panic']=out
    return traces


def generator_observations():
    traces={};out=[]
    def trace(count):
        out.append('enter')
        for index in range(count):out.append('produce '+str(index));yield index
        out.append('body-end')
    for heading,count,last in [('-- drained',3,None),('-- break on first',3,0),('-- break in the middle',5,2)]:
        out.append(heading)
        for value in trace(count):
            out.append('take '+str(value))
            if value==last:break
    out.append('-- never driven');idle=trace(3);out.append('created')
    out.append('-- restart');first=sum(trace(2));second=sum(trace(2));out.append('sums '+str(first)+' '+str(second))
    traces['early_stop']=out
    traces['generator_in_coroutine']=['await '+str(sum(range(5))),
        'scope '+str(sum(range(4))+21*2),'main '+str(sum(range(3))),'early '+str(next(iter(range(9)))+100)]
    out=[];iterator=Pull([5])
    while iterator.has_next():out.append('yielded '+str(iterator.next()))
    render=lambda v:str(v).lower() if type(v) is bool else str(v)
    out.append('has '+render(iterator.has_next()))
    for label,operation,include_message in [('next',iterator.next,True),('next-again',iterator.next,False)]:
        try:operation()
        except PanicChannel as panic:out.append(label+' 432'+(' '+str(panic) if include_message else ''))
    out.append('has-after '+render(iterator.has_next()));empty=Pull([]);out.append('empty-has '+render(empty.has_next()))
    try:empty.next()
    except PanicChannel:out.append('empty-next 432')
    out += ['direct '+str(Pull([5]).next()),'nth-in-range '+str(Pull([5]).nth(0))]
    try:Pull([5]).nth(3)
    except PanicChannel as panic:out.append('nth 432 '+str(panic))
    traces['iterator_exhausted']=out
    evens=[];seen=0
    for index in range(7):
        if index%2==0:seen+=1;evens.append(index*10+seen)
    traces['iterator_param_loop_carried']=[str(sum(range(1,5))),str(sum(range(1,1))),str(next(iter(range(1,5)))),str(sum(evens)),str(sum(range(1,5)))]
    out=[]
    def inner(count):
        for index in range(count):out.append('inner '+str(index));yield index
        out.append('inner-end')
    def doubled(count):
        for value in inner(count):out.append('outer '+str(value));yield value*2
        out.append('outer-end')
    def triple(count):
        for value in doubled(count):yield value+1
    out.append('-- drained');total=0
    for value in doubled(3):out.append('take '+str(value));total+=value
    out.append('sum '+str(total));out.append('-- outer abandoned')
    for value in doubled(4):
        out.append('take '+str(value))
        if value==2:break
    out.append('-- three levels')
    for value in triple(2):out.append('deep '+str(value))
    traces['nested_generator']=out
    out=[]
    def counter(count):
        out.append('start '+str(count))
        for index in range(count):out.append('yield '+str(index));yield index
        out.append('done '+str(count))
    total=0
    for value in counter(3):out.append('take '+str(value));total+=value
    out.append('sum '+str(total));partial=0
    for value in counter(5):
        out.append('partial '+str(value));partial+=value
        if value==1:break
    out.append('partial-sum '+str(partial));traces['value_yield']=out
    out=[]
    def throwing():yield 1;raise ValueErrorChannel('boom')
    try:
        for value in throwing():out.append('x '+str(value))
        out.append('after try')
    except ValueErrorChannel as error:out.append('caught '+str(error))
    traces['yield_throw']=out
    return traces


def observations():
    values=cleanup_observations()|generator_observations()
    if set(values)!=_NAMES:raise ValueError('Cleanup/generator model lost a complete program')
    return values


def expected_rows():
    if (set(_INPUTS)!=set(_REQUIRED_INPUTS) or len(_REQUIRED_INPUTS)!=len(set(_REQUIRED_INPUTS))
            or set(_PROGRAMS)!=_NAMES or len(set(_PROGRAMS.values()))!=len(_NAMES)):
        raise ValueError('Cleanup/generator model lost an exact source or authority')
    for name,digest in _INPUTS.items():
        path=Path(name)
        if path.is_absolute() or path.as_posix()!=name or '..' in path.parts or path.parts[0] not in {'tests','spec','src','stdlib'}:
            raise ValueError('Cleanup/generator source must belong to the delivered repository')
        if hashlib.sha256((ROOT/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:
            raise ValueError('Cleanup/generator source or authority changed: '+name)
    if not set(_PROGRAMS.values())<=set(_INPUTS):raise ValueError('Missing complete cleanup/generator source binding')
    original = _PROGRAMS['defer_late_binding']
    selected = 'tests/unit/xir/legacy_consumers/fixtures/cleanup_snapshot_binding/root.xr'
    before = 'const saved = copy(x)'
    after = 'const saved = x'
    text = (ROOT/original).read_bytes().replace(b'\r\n',b'\n').decode('utf-8')
    if text.count(before) != 1 or (ROOT/selected).read_bytes().replace(b'\r\n',b'\n') != text.replace(before,after).encode('utf-8'):
        raise ValueError('Cleanup snapshot port must preserve the entire original program with one value-binding change')
    values=observations()
    return [{'id':'complete_cleanup_generator_'+name,'source':selected if name=='defer_late_binding' else source, 'original_source':source,'required_target':None,
        'expected':{'exit_code':1 if name=='defer_uncaught_panic' else 0,
            'stdout_utf8':''.join(line+'\n' for line in values[name]),
            'stderr_utf8':'[Uncaught Panic] E0420: division by zero\n' if name=='defer_uncaught_panic' else ''},
        'model':'Full original source, independent logical cleanup order/lazy pull state; no physical reclamation, scheduler or fault qualification from printed events.',
        'observation_count':len(values[name])} for name,source in sorted(_PROGRAMS.items())]
