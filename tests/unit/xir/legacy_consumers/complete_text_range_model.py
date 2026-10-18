"""Independent complete text, range and byte-array program observations."""
import re

PATHS = {
 'range_precedence':'tests/diff/cases/basic/range_precedence.xr',
 'ranges':'tests/diff/cases/basic/ranges.xr',
 'multiconcat':'tests/diff/cases/semantics/string/multiconcat.xr',
 'string_indexof_start_shared_core':'tests/diff/cases/semantics/string/string_indexof_start_shared_core.xr',
 'string_surface_declared_methods':'tests/diff/cases/semantics/string/string_surface_declared_methods.xr',
 'arrays':'tests/diff/cases/basic/arrays.xr',
 'strings':'tests/diff/cases/basic/strings.xr',
 'interpolation_display_shapes':'tests/diff/cases/semantics/string/interpolation_display_shapes.xr',
}

def scalar(value):
    if value is None:
        return 'null'
    if type(value) is bool:
        return str(value).lower()
    return str(value)

def integer(text):
    return int(text) if re.fullmatch(r'[ \t\r\n]*[+-]?[0-9]+[ \t\r\n]*', text) else None

def floating(text):
    grammar = r'[ \t\r\n]*[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?[ \t\r\n]*'
    return float(text) if re.fullmatch(grammar, text) else None

def array_text(values):
    return '[' + ', '.join(scalar(x) for x in values) + ']'

def pad(value, length, fill=' ', left=True):
    needed = length - len(value)
    if needed <= 0 or not fill:
        return value
    added = ''.join(fill[i % len(fill)] for i in range(needed))
    return added + value if left else value + added

def replace(value, old, new, all_matches=False):
    return value if not old else value.replace(old, new, -1 if all_matches else 1)

def observations():
    out = {}
    n = 3
    a = [10, 20, 30, 40]
    endpoints = [(0,n+1,False),(0,n-1,False),(0,n*2,False),(1,n+1,True),(n-1,n*3,False),(0,len(a)-1,False),(0,n+1,False)]
    out['range_precedence'] = [str(lo) + ('..=' if inclusive else '..') + str(hi) for lo,hi,inclusive in endpoints]
    out['range_precedence'] += [sum(range(n+1)),sum(a[i] for i in range(len(a)-1)),len(range(n+1))==4,len(range(n))==3]
    r = list(range(2,9))
    index = lambda i: r[i] if 0 <= i < len(r) else None
    out['ranges'] = [len(r),2 in r,8 in r,9 in r,index(0),index(-1),index(-7),index(-8),'2..9',len(r),r[0],r[6],sum(r)]
    events = []
    def part(number):
        events.append('eval ' + str(number))
        return 'p' + str(number)
    assembled = 'a' + part(1) + 'b' + part(2) + scalar(True) + scalar(None) + 'Mode.Ready'
    out['multiconcat'] = events + [assembled,scalar(42),scalar(False),scalar(None)]
    text = 'one,two,three'
    out['string_indexof_start_shared_core'] = [text.find(','),text.find(',',4),text.find(',',8),
        'bounds-negative','bounds-excessive',4,'bounds-empty-excessive']
    s = 'héllo'
    raw = s.encode('utf-8')
    invalid = bytes([255])
    try:
        invalid.decode('utf-8')
        invalid_caught = False
    except UnicodeDecodeError:
        invalid_caught = True
    sample = 'Aé中'
    boundaries = [0]
    for rune in sample:
        boundaries.append(boundaries[-1] + len(rune.encode('utf-8')))
    out['string_surface_declared_methods'] = [len(s),len(raw),ord(s[0]),ord(s[1]),raw[0],ord('é'),ord('x'),
        'abc'[::-1],'aé😀'[::-1],integer('42'),integer(' -7 '),integer('x'),floating('3.5'),floating('nope'),
        len('')==0,len('a')==0,'self',bytes([104,105]).decode(),bytes([104,195,169]).decode(),invalid_caught,
        'Utf8Error.InvalidUtf8',invalid.decode('utf-8',errors='replace'),array_text([104,105]),'é','🙂',
        '|'.join(['a','β','中']),''.join(['a','β','中']),
        sample.encode()[:1].decode(),sample.encode()[1:3].decode(),sample.encode()[3:6].decode(),
        2 not in boundaries,'StringSliceError.InvalidByteRange',array_text([1,2,3])]
    a = list(range(1,6))
    values = [len(a)]
    a.append(6)
    values += [a[5],*(x*2 for x in a),len([x for x in a if x%2==0]),sum(a)]
    indexes = list(range(len(a)))[-4:-1]
    values += [len(indexes),a[indexes[0]]]
    a[indexes[1]] = 33
    values += [a[3],len(indexes),a[indexes[1]]]
    fill = list(range(1,7))
    fill[2:5] = [9]*3
    values += [fill[2],fill[5]]
    fill[-3:-1] = [7]*2
    values += [fill[3],fill[4]]
    fill[4:] = [4]*(len(fill)-4)
    values += [fill[4],fill[5]]
    octets = [1]*6
    octets[1:4] = [255]*3
    values += [octets[0],octets[1],octets[3],octets[4],octets.index(255),-1,255 in octets,42 in octets]
    packet = bytes(range(1,9))
    values += [int.from_bytes(packet[:4],'little'),int.from_bytes(packet,'little'),packet[1:4][0],packet[1:4][2]]
    repeated = [ord('A'),ord('B'),ord('C')]
    for _ in range(6):
        repeated.append(repeated[-3])
    values += [repeated[3],repeated[8]]
    out['arrays'] = values
    s = 'Hello, World'
    mixed = 'Hello 世界🌍'
    upper = lambda value: ''.join(chr(ord(c)-32) if 'a'<=c<='z' else c for c in value)
    lower = lambda value: ''.join(chr(ord(c)+32) if 'A'<=c<='Z' else c for c in value)
    values = [len(s),upper(s),lower(s),upper(mixed),lower(mixed),s[0:5],'abcdef'[3:6],'abcdef'[2:5],'abcdef'[3:5],
        'World' in s,s.find('World'),'a'+'b'+'c',s[1],'你好🌍'[1],'你好🌍'[2],'abc'[2],
        'é'.encode()[1],ord('世界'[1]),ord('A'),'A你🌍'[0],'A你🌍'[1],'A你🌍'[2],'你好🌍'[::-1],'a你b'[::-1],
        integer(' \t\r\n-17tail'),integer('abc'),floating(' \n-3.5e2tail'),floating('abc'),
        'ab'*3,'xy'*max(0,-2),''*5,'好'*2,pad('42',5,'0'),pad('hi',5,'!',False),pad('x',3),pad('x',3,left=False),
        pad('abc',5,'',False),pad('好',6,'.'),replace('hello world','world','xray'),replace('hello','','x'),
        replace('aaaa','aa','X'),replace('aaaa','aa','X',True),replace('abc','b','',True)]
    for value,separator in [('a,b,c,d',','),('a,,b,',','),('ab','')]:
        parts = list(value) if not separator else value.split(separator)
        values += [len(parts),*parts]
    out['strings'] = values
    out['interpolation_display_shapes'] = ['b='+scalar(True)+' c=Color.Green r='+scalar(1.5)+' n='+scalar(None),
        'lit='+scalar(False)+' f='+scalar(0.0)]
    return out

def expected():
    return [{'id':name,'original_source':PATHS[name],'observations':values,
        'stdout_utf8':''.join(scalar(x)+'\n' for x in values),'expected_exit_code':0,'stderr_utf8':''}
        for name,values in observations().items()]
