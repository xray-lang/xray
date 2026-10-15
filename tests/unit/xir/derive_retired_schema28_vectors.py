"""Preserve complete independently authored Schema28/semantic75 rejection inputs."""
from pathlib import Path
import argparse,hashlib,json,struct,tempfile
from derive_construction72_vectors import header

HERE=Path(__file__).resolve().parent
def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--write',action='store_true');args=parser.parse_args()
    model=json.loads((HERE/'retired_schema28_models.json').read_text(encoding='utf-8'));rows=[]
    assert model['source_commit']=='9b1e33fb98710033054bd86a2433f313ccdfa64c' and model['product_writer_used']==False
    for row in model['vectors']:
        data=bytes.fromhex(row['hex']);assert hashlib.sha256(data).hexdigest()==row['sha256']
        assert data[:8]==b'XRCHK\0\0\0' and struct.unpack_from('<II',data,8)==(28,75)
        assert struct.unpack_from('<Q',data,24)[0]==len(data)-64
        assert hashlib.sha256(data[:32]+data[64:]).digest()==data[32:64]
        rows.append((row['symbol'],data))
    assert len(rows)==model['vector_count'] and len({name for name,_ in rows})==len(rows)
    path=HERE/'xir_retired_schema28_golden.h';end='#endif // XIR_RETIRED_SCHEMA28_GOLDEN_H\n'
    table='#include <stddef.h>\n'
    table+='typedef struct RetiredSchema28Vector { const uint8_t *bytes; size_t length; } RetiredSchema28Vector;\n'
    table+='static const RetiredSchema28Vector retired_schema28_vectors[] = {\n'
    table+=''.join('    {'+name+', sizeof('+name+')},\n' for name,_ in rows)+'};\n'
    with tempfile.TemporaryDirectory(prefix='retired-scope-fields-') as temporary:
        target=Path(temporary)/path.name;header(target,'XIR_RETIRED_SCHEMA28_GOLDEN_H',rows,True)
        text=target.read_text(encoding='utf-8');target.write_text(text.replace(end,table+end),encoding='utf-8',newline='\n')
        for generated in Path(temporary).glob('*.h'):
            delivered=HERE/generated.name;expected=generated.read_bytes()
            if args.write:delivered.write_bytes(expected)
            else:assert delivered.read_bytes()==expected,delivered.name
    print(json.dumps({'retired_schema':28,'semantic':75,'complete_vectors':len(rows),'product_writer_used':False}))
if __name__=='__main__':main()
