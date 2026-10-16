"""Independent complete cluster wire and typed-control consumer expectations.

The local protocol declaration owns the finite packet grammar and detector
policy. Python byte packing, HMAC and a recomputed rational sample window are
independent of Xray execution. No sockets, TLS, scheduling, timing security,
resource, cancellation or provider qualification is inferred.
"""
from collections import deque
from fractions import Fraction
from pathlib import Path
import hashlib,hmac,math,re
from independent_observation_format import render

ROOT=Path(__file__).resolve().parents[4]
_NAMES=('cluster_typed_control_surface','cluster_wire_protocol_pure_direct')
ORIGINALS=tuple('tests/diff/cases/semantics/stdlib/'+name+'.xr' for name in _NAMES)
_AUTHORITIES=(
    'spec/source/sections/003-2-type-system.md','spec/source/sections/004-3-expressions.md',
    'spec/source/sections/006-5-declarations.md','spec/source/sections/014-13-built-in-functions.md',
    'spec/source/sections/015-14-built-in-type-methods.md','spec/source/sections/016-15-standard-library.md',
    'spec/source/sections/018-17-compilation-pipeline.md','stdlib/cluster/cluster.xr',
    'stdlib/types/array.xr','stdlib/types/string.xr','stdlib/crypto/crypto.xr','stdlib/math/math.xr',
    'tests/unit/xir/legacy_consumers/independent_observation_format.py')
_INPUTS={'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677',
 'spec/source/sections/004-3-expressions.md': '1fdce404c935a4208d94f20fd2701b1dae48ca469a944a45b64237f7609eba9c',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/014-13-built-in-functions.md': '2a4af98d2d429babb3cdcd875279f0d798efcabb852afd2c366e69a12ca50380',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'spec/source/sections/016-15-standard-library.md': 'c776ecb898f3dfbbac76e6e9eb08da0697041af5b118eb55bfd294a9630ba2fa',
 'spec/source/sections/018-17-compilation-pipeline.md': '91e9ef4bc0ce560d6621a1dbf8bd6a3f23c58e073e97469b10b7dece5866e440',
 'stdlib/cluster/cluster.xr': 'b24a3cab298cef90b6d0ea5326a3cd4e124b58e94568a602acafe6c7e75e7161',
 'stdlib/crypto/crypto.xr': 'd622f2ba35963bf7ae52a5db06349adfdbdcdf5a43462a4fc497abdfb60df05d',
 'stdlib/math/math.xr': '0126a1eb1db1760b5950da3f6be0c47f504f65070afaff3757f7b45f27a86115',
 'stdlib/types/array.xr': 'ac31595b3801ee1df3c3f3ab6f1903c5f66ef084f0fa016dc19d24a2fa653223',
 'stdlib/types/string.xr': '95307d2e652f164c46b028ead9c6138f85ea783ee42143e4b6d8c60d89d607d7',
 'tests/diff/cases/semantics/stdlib/cluster_typed_control_surface.xr': 'c726d5e2775db521e06274dc6bf7ed86a4ec3489b9489da4ff0215965ae6fca2',
 'tests/diff/cases/semantics/stdlib/cluster_wire_protocol_pure_direct.xr': '1987c57a4d5d4b8ed398e61a3b98544fe200c5b81665671e4af3e7a2f2fe154b',
 'tests/unit/xir/legacy_consumers/independent_observation_format.py': '09843df7bf351411b39877696ae17e489ee982062827d3a6ab8e8cad7ab1f68b'}
KINDS=frozenset(range(1,9))|{10}


def encode_frame(kind,payload):
    if kind not in KINDS or len(payload)+1>16777216:return None
    return (len(payload)+1).to_bytes(4,'big')+bytes([kind])+payload


def decode_frame(data):
    if len(data)<5:return None
    size=int.from_bytes(data[:4],'big')
    if not 1<=size<=16777216 or size+4!=len(data) or data[4] not in KINDS:return None
    return data[4],data[5:]


def valid_name(name,limit):
    return 0<len(name)<=limit and all(32<=c<=126 for c in name.encode('utf-8'))


def valid_topic(topic):
    return (0<len(topic)<=127 and all(33<=c<=126 for c in topic.encode('utf-8'))
            and all(topic.split('.')) and '*' not in topic and '>' not in topic)


def decode_text(data):
    try:return data.decode('utf-8')
    except UnicodeError:return None


def encode_transport(hop,topic,envelope):
    if not 0<=hop<=255 or not valid_topic(topic) or len(envelope)<64:return None
    return encode_frame(7,bytes([hop,len(topic)])+topic.encode()+envelope)


def decode_transport(data):
    frame=decode_frame(data)
    if frame is None or frame[0]!=7:return None
    body=frame[1]
    if len(body)<2 or not body[1] or 2+body[1]+64>len(body):return None
    topic=decode_text(body[2:2+body[1]])
    if topic is None or not valid_topic(topic):return None
    return body[0],topic,body[2+body[1]:]


def encode_handshake(name,nonce,flags,proof=None):
    if not valid_name(name,63) or len(nonce)!=16 or not 0<=flags<=0xffffffff:return None
    if proof is not None and len(proof)!=32:return None
    body=bytes([6,len(name)])+name.encode()+nonce+(proof or b'')+flags.to_bytes(4,'big')
    return encode_frame(1 if proof is None else 2,body)


def decode_handshake(body,ack=False):
    fixed=54 if ack else 22
    if len(body)<fixed or body[0]!=6 or len(body)!=fixed+body[1]:return None
    name=decode_text(body[2:2+body[1]])
    if name is None or not valid_name(name,63):return None
    at=2+body[1];nonce=body[at:at+16];flags=int.from_bytes(body[-4:],'big')
    return (name,nonce,body[at+16:at+48],flags) if ack else (name,nonce,flags)


def encode_done(proof):return encode_frame(3,proof) if len(proof)==32 else None

def decode_done(body):return body if len(body)==32 else None

def encode_heartbeat(kind,timestamp):
    if kind not in {5,6}:return None
    return encode_frame(kind,timestamp.to_bytes(8,'big',signed=True))

def decode_heartbeat(body):return int.from_bytes(body,'big',signed=True) if len(body)==8 else None


def encode_monitor(name):
    return encode_frame(8,bytes([len(name)])+name.encode()) if valid_name(name,127) else None


def decode_monitor(body):
    if not body or len(body)!=1+body[0]:return None
    name=decode_text(body[1:])
    return name if name is not None and valid_name(name,127) else None


def encode_exit(name,reason):
    data=reason.encode('utf-8')
    if not valid_name(name,127) or len(data)>255:return None
    return encode_frame(10,bytes([len(name)])+name.encode()+bytes([len(data)])+data)


def decode_exit(body):
    if len(body)<2 or 1+body[0]>=len(body):return None
    at=1+body[0]
    if len(body)!=at+1+body[at]:return None
    name,reason=decode_text(body[1:at]),decode_text(body[at+1:])
    return (name,reason) if name is not None and valid_name(name,127) and reason is not None else None


def proof(secret,nonce):return hmac.digest(secret.encode('utf-8'),nonce,'sha256') if len(nonce)==16 else None

def proof_equal(left,right):return len(left)==len(right)==32 and left==right


def display(value):
    if isinstance(value,bytes):
        text=', '.join(str(x) for x in value[:32])
        return '['+text+(', ...('+str(len(value)-32)+' more)' if len(value)>32 else '')+']'
    return render({'expected':value})


class Detector:
    """Recompute the last100 intervals; do not copy product ring-buffer sums."""
    def __init__(self,expected=5000):
        self.window=deque(maxlen=100);self.last=0;self.updates=0;self.calls=0;self.expected=expected

    def record(self,now):
        self.calls+=1
        if self.last>0:self.window.append(max(now-self.last,0));self.updates+=1
        self.last=now

    @property
    def stats(self):
        if not self.window:return Fraction(self.expected),Fraction(100)
        mean=Fraction(sum(self.window),len(self.window))
        variance=sum((Fraction(x)-mean)**2 for x in self.window)/len(self.window)
        return mean,max(variance,Fraction(1))

    def value(self,now):
        if not self.last or len(self.window)<2:return 0.0
        mean,variance=self.stats
        scaled=1.7155*(max(now-self.last,0)-float(mean))/max(math.sqrt(float(variance)),1)
        # log(1 + exp(scaled)) is stable even beyond the host exp range.
        return min((max(scaled,0)+math.log1p(math.exp(-abs(scaled))))/math.log(10),15.0)


def heartbeat_dead(phi,samples,elapsed,missed,timeout,maximum):
    return phi>8.0 if samples>=3 else elapsed>timeout and missed+1>=maximum


def typed_accepts(config,info,node,tls_options):
    if any(value is not None for value in (config,info,node,tls_options)):
        raise ValueError('Only the original four nullable absent arguments are modeled')
    return True


def wire_trace():
    values=[];assertions=[];packets={}
    def check(label,actual,expected):
        assertions.append({'label':label,'actual':actual,'expected':expected,'passed':actual==expected})
        if actual!=expected:raise ValueError('Original cluster assertion failed: '+label)
    def packet(label,data,length,indices):
        packets[label]=data.hex();check(label+'.length',len(data),length)
        for index,expected in indices:check(label+'['+str(index)+']',data[index],expected)
    raw=encode_frame(4,bytes([170,187,204]));packet('raw',raw,8,list(enumerate([0,0,0,4,4,170,187,204])))
    decoded=decode_frame(raw);values.extend([raw,decoded[0],decoded[1],decode_frame(bytes.fromhex('0000000404aabb')) is None,
        decode_frame(bytes.fromhex('0000000304aabbcc')) is None,encode_frame(9,b'') is None,decode_frame(bytes.fromhex('0000000109')) is None])
    envelope=bytes(range(64));transport=encode_transport(3,'bench.payload',envelope)
    packet('transport',transport,84,[(3,80),(4,7),(5,3),(6,13),(20,0),(83,63)])
    decoded=decode_transport(transport);values.extend([transport,decoded[0],decoded[1],len(decoded[2]),decoded[2][0],decoded[2][63],
        encode_transport(3,'events.*',bytes(64)) is None,encode_transport(3,'events.user',bytes(63)) is None])
    request=encode_handshake('node-alpha',bytes(range(16,32)),16909060)
    packet('request',request,37,[(3,33),(4,1),(5,6),(6,10),(33,1),(34,2),(35,3),(36,4)])
    decoded=decode_handshake(decode_frame(request)[1]);values.extend([request,*decoded,
        decode_handshake(bytes([5,1,97])+bytes(20)) is None,decode_handshake(bytes([6,1,97])+bytes(20)) is not None])
    ack=encode_handshake('node-beta',bytes(range(32,48)),168496141,bytes(range(128,160)))
    packet('ack',ack,68,[(3,64),(4,2),(5,6),(6,9),(64,10),(65,11),(66,12),(67,13)])
    decoded=decode_handshake(decode_frame(ack)[1],True);values.extend([ack,*decoded,
        decode_handshake(bytes([6,1,97])+bytes(52),True) is not None,decode_handshake(bytes(54),True) is None])
    done=encode_done(bytes(range(192,224)));packet('done',done,37,[(3,33),(4,3),(5,192),(36,223)])
    values.extend([done,decode_done(decode_frame(done)[1]),decode_done(bytes(31)) is None])
    heartbeat=encode_heartbeat(5,72623859790382856)
    packet('heartbeat',heartbeat,13,[(3,9),(4,5),*[(5+i,i+1) for i in range(8)]])
    values.extend([heartbeat,decode_heartbeat(decode_frame(heartbeat)[1]),decode_heartbeat(bytes(7)) is None])
    monitor=encode_monitor('worker.alpha');packet('monitor',monitor,18,[(3,14),(4,8),(5,12)])
    values.extend([monitor,decode_monitor(decode_frame(monitor)[1]),decode_monitor(bytes([1])) is None])
    exited=encode_exit('worker.alpha','normal');packet('exit',exited,25,[(3,21),(4,10),(5,12),(18,6)])
    values.extend([exited,*decode_exit(decode_frame(exited)[1]),decode_exit(bytes([1,97,1])) is None])
    digest=proof('projection-secret',bytes(range(16,32)))
    expected=bytes.fromhex('deb1d34e704143aea767d3481e679477edd8260b8013ba095f8e72686ed70fb0')
    values.extend([digest,proof_equal(digest,expected),proof_equal(bytes(31),bytes(31))])
    detector=Detector(5000)
    for now in [1000,6000,11000]:detector.record(now)
    values.extend([len(detector.window),detector.value(11000)<0.001,0.30<detector.value(16000)<0.31])
    for i in range(120):detector.record(16000+i*5000)
    values.extend([len(detector.window),detector.updates%100])
    values.extend(heartbeat_dead(*args) for args in [(99.0,2,14999,2,15000,3),(99.0,3,0,0,15000,3),
        (0.0,2,15001,2,15000,3),(0.0,3,99999,99,15000,3)])
    if (len(values),len(assertions),detector.calls,detector.updates)!=(53,59,123,122):
        raise ValueError('Complete cluster original observations,assertions or record loop changed')
    return {'observations':values,'original_assertions':assertions,'full_packet_hex':packets,
            'detector_record_calls':detector.calls,'detector_interval_updates':detector.updates}


def expected_rows():
    if (set(_INPUTS)!=set(_AUTHORITIES)|set(ORIGINALS) or _NAMES!=('cluster_typed_control_surface','cluster_wire_protocol_pure_direct')
            or ORIGINALS!=tuple('tests/diff/cases/semantics/stdlib/'+n+'.xr' for n in _NAMES)):
        raise ValueError('Cluster complete source or authority identities changed')
    for path,digest in _INPUTS.items():
        if hashlib.sha256((ROOT/path).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:
            raise ValueError('Cluster complete source or normative input changed: '+path)
    typed=(ROOT/ORIGINALS[0]).read_text(encoding='utf-8');wire=(ROOT/ORIGINALS[1]).read_text(encoding='utf-8')
    if len(re.findall(r'^print\(',wire,re.M))!=53 or wire.count('assertEqual(')!=52 or wire.count('detector.record(')!=4:
        raise ValueError('Original full wire body lost output,assertion or record sites')
    if (typed.count('print(')!=1 or typed.count('return true')!=1 or
            not all(typed.count(name+'?')==1 for name in ['ClusterConfig','ClusterInfo','ClusterNodeInfo','ClusterTlsOptions'])):
        raise ValueError('Original typed control body lost its nullable declaration obligations')
    trace=wire_trace();rows=[]
    for name,source,values in zip(_NAMES,ORIGINALS,[[typed_accepts(None,None,None,None)],trace['observations']]):
        row={'id':'complete_cluster_wire_'+name,'source':source,'required_target':None,
            'expected':{'exit_code':0,'stdout_utf8':''.join(display(x)+'\n' for x in values),'stderr_utf8':''},
            'observation_count':len(values),
            'model':'Complete unchanged original entry;independent typed nullable call or cluster byte grammar/HMAC and recomputed rational Phi window. Array displays preserve first32/suffix and complete packet vectors. No sockets/TLS/constant-time/security/scheduler/resources/FI/lifetime/provider qualification.'}
        if source==ORIGINALS[1]:row.update(required_original_assertions=trace['original_assertions'],full_packet_hex=trace['full_packet_hex'],
            required_original_detector_record_calls=123,required_original_detector_interval_updates=122)
        else:row['required_nullable_parameter_types']=['ClusterConfig?','ClusterInfo?','ClusterNodeInfo?','ClusterTlsOptions?']
        rows.append(row)
    return rows
