"""Rebuild complete nested construction fields and current native policy preimages."""
from pathlib import Path
import argparse,copy,importlib.util,re,struct
from checked_scope_wire import receive_scopes,receive_offset
HERE=Path(__file__).resolve().parent
def load(name):
    spec=importlib.util.spec_from_file_location(name,HERE/(name+'.py'));module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
def arrays(path):
    return {m[1]:bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{2})',m[2]))
        for m in re.finditer(r'static const uint8_t (\w+)\[\] = \{(.*?)\};',path.read_text(encoding='utf-8'),re.S)}
def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--write',action='store_true');args=parser.parse_args()
    fields=load('derive_construction72_vectors');encoder=load('derive_callable71_checked_vectors');previous=load('derive_root_construction75_vectors')
    model=previous.model();words=lambda *v:struct.pack('<'+'I'*len(v),*v)
    body,_=encoder.encode_body(model,0);assert body[-8:]==bytes(8)
    facts=model['template_facts'];assert [row['constant_mask'] for row in facts]==[4,0,0,0,0]
    template=words(1,5)+b''.join(words(0,row['constant_mask'],0,0,0) for row in facts)
    checked=body[:-8]+words(1,3,2,4,5)+words(0)+template
    instance=copy.deepcopy(model);instance['generics']=None;instance['nodes'][0][4]=[2,2]
    projected,_=encoder.encode_body(instance,0);assert projected[-8:]==bytes(8)
    origins=words(5)+b''.join(words(i,0,0) for i in range(5))+words(0)
    inner_start=len(projected)-8+7*4
    recursive=projected[:-8]+words(1,0,2,0,0)+words(0,2)+checked+origins
    old=arrays(HERE/'root_construction75_goldens.h')
    assert fields.frame(checked,28,75)==old['root_construction75_checked']
    assert fields.frame(recursive,28,75)==old['root_construction75_instance']
    current_checked,checked_scopes=receive_scopes(checked)
    current_projected,projected_scopes=receive_scopes(projected)
    current_recursive=current_projected[:-8]+words(1,0,2,0,0)+words(0,2)+current_checked+origins
    assert len(current_checked)==len(checked)+4*len(checked_scopes)
    assert len(current_recursive)==len(recursive)+4*(len(projected_scopes)+len(checked_scopes))
    positions={'checked':checked_scopes,'instance':projected_scopes+[inner_start+p for p in checked_scopes]}
    rows=[('root_construction77_checked',fields.frame(current_checked,29,77)),
          ('root_construction77_instance',fields.frame(current_recursive,29,77)),
          ('root_construction71_previous_current77',old['root_construction71_previous_current75'])]
    historical74=fields.frame(checked,28,74)
    assert historical74==arrays(HERE/'root_construction72_goldens.h')['root_construction74_previous']
    rows.append(('root_construction74_previous',historical74))
    text=(HERE/'root_construction75_goldens.h').read_text(encoding='utf-8');macros=[]
    for name,value in re.findall(r'#define (ROOT_CONSTRUCTION75_\w+) (\d+)u',text):
        locations=positions['instance' if 'RECURSIVE' in name else 'checked']
        macros.append((name.replace('75','77'),receive_offset(int(value),locations)))
    assert len(macros)==2
    fields.header(HERE/'root_construction77_goldens.h','ROOT_CONSTRUCTION77_GOLDENS_H',rows,args.write,macros)
    identities=load('derive_source_product_identity')
    old_identity=[item for mapping in identities.frames(25,76,29,31).values() for item in mapping.items()]
    assert arrays(HERE/'xir_hash76_identity.h')==dict(old_identity)
    current=[item for mapping in identities.frames(25,77,29,31).values() for item in mapping.items()]
    fields.header(HERE/'xir_hash77_identity.h','XIR_HASH77_IDENTITY_H',current,args.write)
    print('Independent current29/77 root fields: scopes in both outer and nested modules; old28/75 exact; current policy25 unchanged ABI.')
if __name__=='__main__':main()
