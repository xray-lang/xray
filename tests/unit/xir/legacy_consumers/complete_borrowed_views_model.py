"""Complete borrowed-view programs under shared/exclusive permissions and explicit copies."""
from pathlib import Path
import copy,hashlib

ROOT=Path(__file__).resolve().parents[4]
_INPUTS = {'spec/source/sections/003-2-type-system.md': '916835c38312eccdada94aff018d22d79f3c15ed998048754759764c53fc8620',
 'spec/source/sections/004-3-expressions.md': '1fdce404c935a4208d94f20fd2701b1dae48ca469a944a45b64237f7609eba9c',
 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'stdlib/prelude/builtin_symbols.def': '7c7e1749ecae49adc12a579614991f34236c02f61497b439cb61073f9893d18c',
 'stdlib/types/array.xr': 'ac31595b3801ee1df3c3f3ab6f1903c5f66ef084f0fa016dc19d24a2fa653223',
 'stdlib/types/string.xr': '95307d2e652f164c46b028ead9c6138f85ea783ee42143e4b6d8c60d89d607d7',
 'tests/diff/cases/semantics/collections/slice_window_shared_core.xr': 'f83700a58dbffbf9711b1683773d07b5ba2e99ce3d1eb02b3b764185a7387535',
 'tests/diff/cases/semantics/slice/array_expression_view.xr': 'bdb0b1b5e544b76184b16f6e2a859bc044ffad42aa9f4694015a85f064029198',
 'tests/diff/cases/semantics/slice/array_negative_both.xr': '8dc8d8fa56369020bf48d3a54f7e45620a9871d10701024907ad64c4b01abfa3',
 'tests/diff/cases/semantics/slice/array_negative_end.xr': '04099e778a548b71f84d888653193948a2c64b4021f112cea04e3260fbabce09',
 'tests/diff/cases/semantics/slice/array_negative_start.xr': '107736cb4308076e6880190c9ff1dbf91f0fd890bd2e9c149ada8aa47ab21020',
 'tests/diff/cases/semantics/slice/string_byte_slice_receiver_nll.xr': '3dff41d162190ec42b23f2adde719faadad5b2b180f0a39262ed05af554d4446',
 'tests/diff/cases/semantics/slice/string_byte_slice_view.xr': '305af8ac6a4c351040ae69ad981561a80864ac04a76081748175407518f215d8',
 'tests/unit/xir/legacy_consumers/fixtures/borrowed_views_complete/array_negative_both/root.xr': '0840998d27bb6ef69e77648cdb8fac96f329ad3155f42262716aee0b6f1d5879',
 'tests/unit/xir/legacy_consumers/fixtures/borrowed_views_complete/array_negative_end/root.xr': '6fe03628f32ab2d56dbdf6cad3d1f524b88ea3f2202e98cdf8bbccff756bccd9',
 'tests/unit/xir/legacy_consumers/fixtures/borrowed_views_complete/array_negative_start/root.xr': 'fcc0c217fc0e85de9cd08a96bcaff117ef7b20b564d4486b6cc0a002b33a522a',
 'tests/unit/xir/legacy_consumers/fixtures/borrowed_views_complete/slice_window_shared_core/root.xr': 'b90361090193126049ba7e6f105c11c62fc852da41d18b0628d20261cab18c1c',
 'tests/unit/xir/legacy_consumers/fixtures/explicit_read_and_mutable_view/root.xr': 'c945520f65120a36de44b6016774ab9c4ae6153f175aa5a2a69c45f232d9a308'}
_REQUIRED_INPUTS = ('spec/source/sections/003-2-type-system.md',
 'spec/source/sections/004-3-expressions.md',
 'spec/source/sections/005-4-statements.md',
 'spec/source/sections/015-14-built-in-type-methods.md',
 'stdlib/prelude/builtin_symbols.def',
 'stdlib/types/array.xr',
 'stdlib/types/string.xr',
 'tests/diff/cases/semantics/collections/slice_window_shared_core.xr',
 'tests/diff/cases/semantics/slice/array_expression_view.xr',
 'tests/diff/cases/semantics/slice/array_negative_both.xr',
 'tests/diff/cases/semantics/slice/array_negative_end.xr',
 'tests/diff/cases/semantics/slice/array_negative_start.xr',
 'tests/diff/cases/semantics/slice/string_byte_slice_receiver_nll.xr',
 'tests/diff/cases/semantics/slice/string_byte_slice_view.xr',
 'tests/unit/xir/legacy_consumers/fixtures/borrowed_views_complete/array_negative_both/root.xr',
 'tests/unit/xir/legacy_consumers/fixtures/borrowed_views_complete/array_negative_end/root.xr',
 'tests/unit/xir/legacy_consumers/fixtures/borrowed_views_complete/array_negative_start/root.xr',
 'tests/unit/xir/legacy_consumers/fixtures/borrowed_views_complete/slice_window_shared_core/root.xr',
 'tests/unit/xir/legacy_consumers/fixtures/explicit_read_and_mutable_view/root.xr')
_PROGRAMS = {'array_expression_view': 'tests/diff/cases/semantics/slice/array_expression_view.xr',
 'array_negative_both': 'tests/diff/cases/semantics/slice/array_negative_both.xr',
 'array_negative_end': 'tests/diff/cases/semantics/slice/array_negative_end.xr',
 'array_negative_start': 'tests/diff/cases/semantics/slice/array_negative_start.xr',
 'slice_window_shared_core': 'tests/diff/cases/semantics/collections/slice_window_shared_core.xr',
 'string_byte_slice_receiver_nll': 'tests/diff/cases/semantics/slice/string_byte_slice_receiver_nll.xr',
 'string_byte_slice_view': 'tests/diff/cases/semantics/slice/string_byte_slice_view.xr'}
_PORTS = {'array_expression_view': 'tests/unit/xir/legacy_consumers/fixtures/explicit_read_and_mutable_view/root.xr',
 'array_negative_both': 'tests/unit/xir/legacy_consumers/fixtures/borrowed_views_complete/array_negative_both/root.xr',
 'array_negative_end': 'tests/unit/xir/legacy_consumers/fixtures/borrowed_views_complete/array_negative_end/root.xr',
 'array_negative_start': 'tests/unit/xir/legacy_consumers/fixtures/borrowed_views_complete/array_negative_start/root.xr',
 'slice_window_shared_core': 'tests/unit/xir/legacy_consumers/fixtures/borrowed_views_complete/slice_window_shared_core/root.xr'}
_NAMES=frozenset({'array_expression_view','array_negative_both','array_negative_end','array_negative_start',
 'string_byte_slice_view','string_byte_slice_receiver_nll','slice_window_shared_core'})
_PORT_NAMES=_NAMES-{'string_byte_slice_view','string_byte_slice_receiver_nll'}

class BorrowConflict(ValueError):pass

def bounds(length,start=None,end=None):
    def endpoint(value,default):
        if value is None:return default
        return max(0,min(length,length+value if value<0 else value))
    first,last=endpoint(start,0),endpoint(end,length)
    return first,max(first,last)

class Owner:
    def __init__(self,items):self.items=copy.deepcopy(list(items));self.views=[]
    def _access(self,write=False):
        if any(v.active and (write or v.mutable) for v in self.views):raise BorrowConflict('owner conflicts with a live view')
    def read(self,index):self._access();return copy.deepcopy(self.items[index])
    def write(self,index,value):self._access(True);self.items[index]=copy.deepcopy(value)
    def view(self,start=None,end=None,mutable=False):
        if any(v.active and (mutable or v.mutable) for v in self.views):raise BorrowConflict('borrow conflicts with a live view')
        a,b=bounds(len(self.items),start,end);return View(self,a,b,mutable)
    def rebind(self,items):self._access(True);self.items=copy.deepcopy(list(items))

class View:
    def __init__(self,owner,start,end,mutable,parent=None):
        self.owner,self.start,self.end,self.mutable,self.parent=owner,start,end,mutable,parent
        self.active=True;self.children=[];owner.views.append(self)
        if parent:parent.children.append(self)
    def access(self,write=False):
        if not self.active:raise BorrowConflict('view has ended')
        if write and not self.mutable:raise BorrowConflict('shared view cannot write')
        if self.mutable and any(v.active and (write or v.mutable) for v in self.children):raise BorrowConflict('exclusive parent is reborrowed')
    def length(self):self.access();return self.end-self.start
    def read(self,index):
        self.access()
        if not 0<=index<self.end-self.start:raise IndexError(index)
        return copy.deepcopy(self.owner.items[self.start+index])
    def write(self,index,value):
        self.access(True)
        if not 0<=index<self.end-self.start:raise IndexError(index)
        self.owner.items[self.start+index]=copy.deepcopy(value)
    def snapshot(self):self.access();return copy.deepcopy(self.owner.items[self.start:self.end])
    def child(self,start=None,end=None,mutable=False):
        self.access(mutable);a,b=bounds(self.end-self.start,start,end)
        return View(self.owner,self.start+a,self.start+b,mutable,self)
    def close(self):
        if any(v.active for v in self.children):raise BorrowConflict('child still borrows this view')
        self.active=False

def render(value):
    if isinstance(value,list):return '['+', '.join(str(x) for x in value)+']'
    return str(value)

def observations():
    rows={};owner=Owner([10,20,30,40]);owner.write(1,21);view=owner.view(1,3)
    values=[view.read(0)];view.close();writable=owner.view(1,3,True);writable.write(1,31);writable.close();values.append(owner.read(2))
    read=owner.view(1,3);snapshot=read.snapshot();read.close();owner.write(1,22)
    values.append(snapshot[0]);snapshot[1]=32;values.append(owner.read(2));rows['array_expression_view']=values
    for name,ranges in {
      'array_negative_both':[(-3,-1),(-3,-1),(-999,-1),(-1,-999)],
      'array_negative_end':[(None,-1),(None,-1),(None,-999),(3,1)],
      'array_negative_start':[(-2,None),(-2,None),(-999,None),(999,None)]}.items():
        owner=Owner([1,2,3,4]);values=[]
        for start,end in ranges:
            view=owner.view(start,end);values.append(view.snapshot());view.close()
        rows[name]=values
    owner=Owner('ABCD'.encode());view=owner.view();values=[view.length(),view.read(0),view.read(1)]
    middle=view.child(1,3);values.extend([middle.length(),middle.read(0),middle.read(1)])
    snapshot=middle.snapshot();middle.close();view.close();values.append(snapshot[0]);rows['string_byte_slice_view']=values
    owner=Owner('hello'.encode());view=owner.view();values=[view.length()];view.close();owner.rebind('bye'.encode());values.append(len(owner.items));rows['string_byte_slice_receiver_nll']=values
    owner=Owner(index*10 for index in range(6));all_=owner.view(mutable=True);middle=all_.child(1,4,True)
    values=[middle.length(),middle.read(0),middle.read(2)];middle.write(1,99);values.append(middle.read(1))
    inner=middle.child(1,3,True);values.extend([inner.length(),inner.read(0),inner.read(1)]);inner.close();middle.close()
    for start,end in [(6,6),(2,2)]:
        empty=all_.child(start,end);values.append(empty.length());empty.close()
    whole=all_.child(0,6);values.extend([whole.length(),whole.read(5)]);whole.close();all_.close()
    if owner.read(2)!=99:raise AssertionError('write did not reach the complete original owner')
    rows['slice_window_shared_core']=values
    if set(rows)!=_NAMES:raise ValueError('Complete view program identities changed')
    return rows

def replace_once(text,before,after):
    if text.count(before)!=1:raise ValueError('View migration lost an exact complete-source boundary')
    return text.replace(before,after)

def port_text(name,text):
    if name=='array_expression_view':
        text=replace_once(text,'// anchor: slice-array-expression-view\n','')
        text=replace_once(text,'    var view: Slice<i64> = arr[1:3]\n    arr[1] = 21\n',
            '    arr[1] = 21\n    var view: Slice<i64> = arr[1:3]\n')
        text=replace_once(text,'    view[1] = 31\n','    var writable: MutSlice<i64> = arr[1:3]\n    writable[1] = 31\n')
        text=replace_once(text,'    var copy = copy(arr[1:3])\n','    var owned: Array<i64> = copy(arr[1:3])\n')
        text=replace_once(text,'print(copy[0])','print(owned[0])')
        return replace_once(text,'    copy[1] = 32\n','    owned[1] = 32\n')
    names={'array_negative_both':('middle','clampedStart','reversed'),
           'array_negative_end':('endDrop','hugeDrop','reversed'),
           'array_negative_start':('tail','hugeTail','pastEnd')}
    if name in names:
        for view in names[name]:text=replace_once(text,'print('+view+')','print(copy('+view+'))')
        expression={'array_negative_both':'arr[-3:-1]','array_negative_end':'arr[:-1]','array_negative_start':'arr[-2:]'}[name]
        return replace_once(text,'    print(copy('+expression+'))\n','    var explicitCopyView: Slice<i64> = '+expression+'\n    print(copy(explicitCopyView))\n')
    if name=='slice_window_shared_core':
        for view in ('all','middle','inner'):text=replace_once(text,'var '+view+': Slice<i64>','var '+view+': MutSlice<i64>')
        for before,after in [('all.window(1, 3)','all[1:4]'),('middle.window(1, 2)','middle[1:3]'),
          ('all.window(6, 0)','all[6:6]'),('all.window(2, 0)','all[2:2]'),('all.window(0, 6)','all[0:6]'),
          ('print(values[2])','print(middle[1])'),('    print(whole[5])\n','    print(whole[5])\n    assert(values[2] == 99)\n')]:text=replace_once(text,before,after)
        return text
    raise ValueError('No reviewed complete view migration for this identity')

def expected_rows():
    if (set(_INPUTS)!=set(_REQUIRED_INPUTS) or len(_INPUTS)!=len(_REQUIRED_INPUTS) or
        set(_PROGRAMS)!=_NAMES or len(set(_PROGRAMS.values()))!=len(_NAMES) or set(_PORTS)!=_PORT_NAMES):
        raise ValueError('Complete view model lost an exact input/program/port')
    for name,digest in _INPUTS.items():
        path=Path(name)
        if path.is_absolute() or path.as_posix()!=name or '..' in path.parts or path.parts[0] not in {'tests','spec','stdlib'}:raise ValueError('View input is not delivered')
        if hashlib.sha256((ROOT/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:raise ValueError('View source or normative authority changed: '+name)
    for name,path in _PORTS.items():
        target=('tests/unit/xir/legacy_consumers/fixtures/explicit_read_and_mutable_view/root.xr' if name=='array_expression_view' else
                'tests/unit/xir/legacy_consumers/fixtures/borrowed_views_complete/'+name+'/root.xr')
        if path!=target:raise ValueError('View port lost its canonical fixture identity')
        original=(ROOT/_PROGRAMS[name]).read_bytes().replace(b'\r\n',b'\n').decode('utf-8')
        if (ROOT/path).read_bytes().replace(b'\r\n',b'\n')!=port_text(name,original).encode('utf-8'):raise ValueError('View full-source port changed beyond its reviewed transformation')
    rows=[]
    for name,values in sorted(observations().items()):
        source=_PROGRAMS[name]
        expected='tests/diff/cases/semantics/'+('collections/' if name=='slice_window_shared_core' else 'slice/')+name+'.xr'
        if source!=expected:raise ValueError('View program lost its complete original identity')
        rows.append({'id':'complete_borrowed_views_'+name,'source':_PORTS.get(name,source),'original_source':source,'required_target':None,
         'expected':{'exit_code':0,'stdout_utf8':''.join(render(value)+'\n' for value in values),'stderr_utf8':''},'observation_count':len(values),
         'model':'Complete original program under explicit shared/exclusive views, last-use reborrowing and independent owned snapshots. Full original owner-write assertion retained after borrow end; no physical lifetime/provider qualification.'})
    return rows
