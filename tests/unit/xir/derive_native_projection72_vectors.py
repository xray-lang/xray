"""Validate complete fixed prior policies and the actual current identity.

Current header identities are checked here. Prior69/70 oracles retain their
fixed entire frames and literals; they no longer claim current macro authority.
No product owner, generated output, compiler probe or writer is an oracle.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct

DOMAIN = b'xray:xir-c11-codegen-policy:v1'
VERSIONS = {'65': (25,65,21,26,29), '69': (25,69,22,28,29),
            '70': (25,70,22,28,29), '71': (26,71,22,28,29),
            '72': (27,72,22,28,29)}
DIGESTS = {'65': {'linked': 'b8ba8fb63ea3d838ab97a435519b5516f1115c913ee1517a951b57dafe005cd9', 'original': '751fa4a1bc2f0d27f7d3e3468746f43a0cf10f94239257ef2faad77b3b5164cd'}, '69': {'linked': '6b273a3b64d3ad414d97b6a0715b1215b6a156b61100945906f80cd9963f2d38', 'original': '88c27a62c59ab29355582f0b55ea199ad12c975409b6da1718c9c9b29cc9a119'}, '70': {'linked': 'b48d8387b256aea331c1c7923c6f45c1477836e9dd848d54b1343c9c8a32fa55', 'original': '6a10817c6c32b6634e464afbde45f580f6f9fd1bf9d352a6665ea33b8b990d28'}, '71': {'linked': 'df0ae0c35a8f4acfe213e45a69a048e63c93b0342562ec11f1ce4edd78940caf', 'original': '37633c7c9d2007f99e0f2846cb0da2fd494f58a27a89705617ec9d48c8902018'}, '72': {'linked': '31ed1da7a1205bc7507d07e6e55d631078e4e8b4fce0ea05d4fe7f4e77e10541', 'original': '9566ac5b37a0ef460aabfe2230a32cad4de26ea6f0fe1aa86c6a7f6627523a91'}}


def literal(text, symbol):
    match = re.search(r'\b' + re.escape(symbol) + r'\[[^]]*\]\s*=\s*\{([^}]+)\}', text)
    assert match is not None, symbol
    return bytes(int(word, 16) for word in re.findall(r'0x[0-9a-fA-F]{2}', match[1]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path)
    parser.add_argument('--expectations', type=Path)
    parser.add_argument('--models', type=Path)
    args = parser.parse_args()
    directory = Path(__file__).resolve().parent
    root = args.source_root.resolve() if args.source_root else directory.parents[2]
    header = (args.expectations or directory / 'xir_native_projection_expectations.h').read_text(encoding='utf-8')
    model = json.loads((args.models or directory / 'native_projection72_identity_models.json').read_text(encoding='utf-8'))
    assert len(model['records']) == 10
    for value, file, macro in (
        (28,'xxir_checked.h','XR_XIR_CHECKED_SCHEMA'),
        (73,'xxir_checked.h','XR_XIR_CHECKED_CONTRACT'),
        (23,'xxir_value.h','XR_XIR_VALUE_ABI_VERSION'),
        (29,'xxir_call.h','XR_XIR_CALL_ABI_VERSION'),
        (30,'xxir_program.h','XR_XIR_PROGRAM_ABI_VERSION'),
    ):
        text = (root / 'src/xir' / file).read_text(encoding='utf-8')
        assert re.findall(r'^#define\s+' + macro + r'\s+(\d+)u?\s*$', text, re.M) == [str(value)]
    records = []
    for role in ('linked','original'):
        prefix = (role + '_source').encode('ascii')
        preimages = {}
        for version, versions in VERSIONS.items():
            words = [1,*versions,len(prefix)]
            frame = DOMAIN + struct.pack('<7I', *words) + prefix
            digest = hashlib.sha256(frame).hexdigest()
            assert digest == DIGESTS[version][role]
            expected = {'role':role, 'version':version, 'words':words,
                        'prefix':prefix.decode('ascii'), 'length':len(frame),
                        'preimage':frame.hex(), 'sha256':digest}
            rows = [row for row in model['records'] if row['role'] == role and row['version'] == version]
            assert rows == [expected]
            suffix = '' if version == '65' else version
            assert literal(header,role + '_policy' + suffix).hex() == digest
            if version in ('71','72'):
                assert literal(header,role + '_policy' + version + '_preimage') == frame
            assert len(frame) == (71 if role == 'linked' else 73)
            preimages[version] = frame
            records.append(expected)
        offset = len(DOMAIN) + 4
        assert preimages['71'][:offset] == preimages['72'][:offset]
        assert preimages['71'][offset+8:] == preimages['72'][offset+8:]
        assert len({hashlib.sha256(value).digest() for value in preimages.values()}) == 5
    assert 'CHECK(!memcmp(input->codegen_policy_id.bytes,linked ? linked_policy73 : original_policy73,32));' in header
    for suffix in ('','69','70','71','72'):
        assert 'CHECK(memcmp(input->codegen_policy_id.bytes,linked ? linked_policy' + suffix + ' : original_policy' + suffix + ',32));' in header
    for role in ('linked','original'):
        prefix=(role+'_source').encode('ascii')
        frame=DOMAIN+struct.pack('<7I',1,28,73,23,29,30,len(prefix))+prefix
        assert literal(header,role+'_policy73_preimage')==frame
        assert literal(header,role+'_policy73')==hashlib.sha256(frame).digest()
    print(json.dumps({'status':'INDEPENDENT_CURRENT28_73_FULL_FRAMES_PASS',
                      'records':records, 'current_macro_authority_checked':True,
                      'all_prior_runtime_negative_responsibilities_retained':True,
                      'product_output_used_as_oracle':False}, indent=2))


if __name__ == '__main__':
    main()
