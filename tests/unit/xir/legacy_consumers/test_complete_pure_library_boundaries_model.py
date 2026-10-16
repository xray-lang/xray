"""Control full library outputs, independent AES answers and exact input bytes."""
from pathlib import Path
import hashlib,hmac,json
import unittest
from unittest import mock
import complete_pure_library_boundaries_model as m


class CompletePureLibraryBoundaries(unittest.TestCase):
    def test_every_complete_original_observation(self):
        traces=m.observations();self.assertEqual(set(traces),m._NAMES)
        self.assertEqual([len(traces[k]) for k in ['crypto_timing_safe_equal_direct','toml_pure_module_direct','io_read_stdin_shared_core']],[22,19,4])
        self.assertEqual(sum(len(traces[name]) for name in m._ORIGINAL_NAMES),45)
        self.assertEqual(traces['crypto_timing_safe_equal_direct'][:12],[True,False,False,True,True,False,False,False,True,False,True,True])
        self.assertEqual(traces['toml_pure_module_direct'],['xray',2,True,'apple','red',2,'Nail',True,'TomlDiagnostic',True,2,'Duplicate key','Alice',True,0,True,True,True,True])
        self.assertEqual(traces['io_read_stdin_shared_core'],[20,False,False,False])
        self.assertEqual(len([row for row in m.expected_rows() if row['original_source'] not in m._SYSTEM_PROGRAMS.values()]),3)

    def test_all_four_published_nist_aes256_known_answers_both_directions(self):
        key=bytes.fromhex('603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4')
        vectors=[('6bc1bee22e409f96e93d7e117393172a','f3eed1bdb5d2a03c064b5a7e3db181f8'),
            ('ae2d8a571e03ac9c9eb76fac45af8e51','591ccb10d410ed26dc5ba74a31362870'),
            ('30c81c46a35ce411e5fbc1191a0a52ef','b6ed21b99ca6f4f9f153e7b1beafed1d'),
            ('f69f2445df4f9b17ad2b417be66c3710','23304b7a39f9f3ff067d8d8f9e24ecc7')]
        for plain,cipher in vectors:
            with self.subTest(plain=plain):
                self.assertEqual(m.aes_block(key,bytes.fromhex(plain)).hex(),cipher)
                self.assertEqual(m.aes_block(key,bytes.fromhex(cipher),decrypt=True).hex(),plain)

    def test_finite_field_substitution_and_inverse_are_bijections(self):
        self.assertEqual(m.multiply(0x57,0x13),0xfe)
        self.assertEqual(m.SBOX[0],0x63);self.assertEqual(m.SBOX[0x53],0xed)
        for value in range(256):
            self.assertEqual(m.INVERSE_SBOX[m.SBOX[value]],value)
            if value:self.assertEqual(m.multiply(value,m.field_power(value,254)),1)

    def test_authentication_and_padding_roundtrip_at_every_block_boundary(self):
        for size in [0,1,15,16,17,31,32]:
            plain=bytes(range(size))
            for iv in [bytes(16),bytes(range(16)),bytes([255])*16]:
                with self.subTest(size=size,iv=iv.hex()):
                    wire=m.encrypt(b'secret',plain,iv)
                    self.assertEqual(len(wire),2*(48+16*(size//16+1)))
                    self.assertEqual(m.decrypt(b'secret',wire),plain)
                    self.assertIsNone(m.decrypt(b'wrong-key',wire))

    def test_bad_iv_cipher_and_tag_are_rejected_before_decryption(self):
        wire=bytes.fromhex(m.encrypt(b'secret',b'hello world',bytes(16)))
        for offset in range(len(wire)):
            changed=bytearray(wire);changed[offset]^=1
            with mock.patch.object(m,'aes_block',side_effect=AssertionError('Unauthenticated decryption executed')):
                self.assertIsNone(m.decrypt(b'secret',changed.hex()))

    def test_authenticated_invalid_padding_and_bad_wire_shapes(self):
        enc_key,mac_key=m.keys_for(b'secret');iv=bytes(16)
        cipher=m.aes_block(enc_key,b'A'*15+b'\x00');signed=iv+cipher
        wire=(signed+hmac.new(mac_key,signed,hashlib.sha256).digest()).hex()
        self.assertIsNone(m.decrypt(b'secret',wire))
        self.assertIsNone(m.decrypt(b'secret','xyz'))
        with self.assertRaisesRegex(ValueError,'CryptoError.InvalidLength'):m.decrypt(b'secret','00'*48)

    def test_digest_and_mac_public_known_answers(self):
        trace=m.crypto_trace()
        self.assertEqual(trace[12],'900150983cd24fb0d6963f7d28e17f72')
        self.assertEqual(trace[14],'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad')
        self.assertEqual(trace[-2],'5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843')
        self.assertTrue(trace[-1])

    def test_typed_toml_duplicate_recovery_retains_the_first_value(self):
        data,errors,lines=m.toml_report('name = "Alice"\nname = "Bob"\nage = 30')
        self.assertEqual(data,{'name':'Alice','age':30});self.assertEqual(lines,3)
        self.assertEqual(errors,[m.Diagnostic('DuplicateKey',2,'Duplicate key')])
        self.assertEqual(m.toml_report('x = [1, 2')[1][0].kind,'SyntaxError')
        with self.assertRaises(ValueError):m.toml_report('[broken')

    def test_toml_scalar_serialization_preserves_escaping_and_types(self):
        value={'name':'a"b\\c\n中文','version':1,'enabled':True}
        text=m.stringify_flat(value)
        self.assertEqual(m.tomllib.loads(text),value)
        self.assertIn('enabled = true',text)
        with self.assertRaises(ValueError):m.stringify_flat({'nested':{}})

    def test_original_stdin_is_binary_and_line_endings_remain_observable(self):
        self.assertEqual(m.stdin_trace(b'alpha\nbeta\nomega\n'),[17,True,True,True])
        self.assertEqual(m.stdin_trace(m.STDIN_BYTES),[20,False,False,False])
        self.assertEqual(m.stdin_trace(b''),[0,False,False,False])
        with self.assertRaises(UnicodeDecodeError):m.stdin_trace(b'\xff')

    def test_every_source_authority_and_raw_stdin_is_pinned(self):
        saved=Path.read_bytes
        for name in set(m._INPUTS)|set(m._RAW_INPUTS):
            with self.subTest(input=name):
                def changed(path):return saved(path)+b'\nchanged\n' if path==m.ROOT/name else saved(path)
                with mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):m.expected_rows()
        def normalized(path):return saved(path).replace(b'\r\n',b'\n') if path==m.ROOT/m._STDIN else saved(path)
        with mock.patch.object(Path,'read_bytes',normalized),self.assertRaises(ValueError):m.expected_rows()

    def test_complete_identity_maps_and_raw_input_premises_cannot_be_erased(self):
        swapped=dict(m._PROGRAMS);names=sorted(swapped);swapped[names[0]],swapped[names[1]]=swapped[names[1]],swapped[names[0]]
        for attribute,value in [('_PROGRAMS',swapped),('_PROGRAMS',{}),('_INPUTS',{}),('_RAW_INPUTS',{}),('STDIN_BYTES',b'alpha\nbeta\nomega\n')]:
            with self.subTest(attribute=attribute),mock.patch.object(m,attribute,value),self.assertRaises(ValueError):m.expected_rows()

    def test_real_routes_retain_whole_original_source_and_exact_stdin(self):
        cases={row['id']:row for row in json.loads((Path(__file__).parent/'product_driver_cases.json').read_text())['cases']}
        for expected in m.expected_rows():
            row=cases[expected['id']]
            self.assertEqual(row['entry'],'run');self.assertEqual(row['source_context'],'original_absolute')
            self.assertEqual(row['source'],expected['original_source'])
            self.assertEqual(row['stdout_utf8'],expected['expected']['stdout_utf8'])
            self.assertEqual(row['stdin_hex'],expected.get('required_stdin_hex',''))



    def test_four_complete_math_random_domain_traces(self):
        expected={'math_core_direct_type_preserve':['f64','42.0','f64',True,'f64','3.0','f64','7.0','f64','5.0','4.0','9.0','10.0','f64','5.5',1,2,2,1,
         '0.0','1.0','0.0','0.0','0.0','0.0','0.0','0.0','2.0','3.0','1.0','0.0','1.0','0.0','5.0','3.0','3.0','0.0','0.0','2.5','0.0','0.0',-1,True,True,False],
         'rounding_boundary':['0.3','5.55111512312578e-17','-2.77555756156289e-17','1.4142135623731','2.0','0.01','0.333333333333333','0.666666666666667'],
         'crypto_random_system_direct':[True]*11,'runtime_domain_bytes':[True]*3}
        self.assertEqual(sum(map(len,expected.values())),67);self.assertEqual(m.system_observations(),expected)
        self.assertEqual(sum(map(len,m.observations().values())),112);self.assertEqual(len(m.expected_rows()),7)

    def test_binary64_literals_and_left_associative_residual_known_bits(self):
        f=m.FloatValue.literal;fraction=m.Fraction
        a,b,c=[f(value) for value in [fraction(1,10),fraction(1,5),fraction(3,10)]]
        self.assertEqual([a.bits,b.bits,c.bits],[0x3fb999999999999a,0x3fc999999999999a,0x3fd3333333333333])
        added=m.float_binary(a,b,'add');self.assertEqual(added.bits,0x3fd3333333333334)
        self.assertEqual(m.float_binary(added,c,'sub').bits,0x3c90000000000000)
        self.assertEqual(m.float_binary(m.float_binary(c,a,'sub'),b,'sub').bits,0xbc80000000000000)
        self.assertEqual(m.float_binary(f(1),f(3),'div').bits,0x3fd5555555555555)

    def test_exact_rational_rounding_is_nearest_even_at_binary_ties(self):
        fraction=m.Fraction;unit=0x3ff0000000000000
        self.assertEqual(m.fraction_bits(1+fraction(1,2**53)),unit)
        self.assertEqual(m.fraction_bits(1+fraction(3,2**53)),unit+2)
        self.assertEqual(m.fraction_bits(-(1+fraction(3,2**53))),(1<<63)|unit+2)
        for value,answer in [(fraction(1,2),0),(fraction(3,2),2),(fraction(5,2),2),(fraction(7,2),4)]:self.assertEqual(m.nearest_even(value),answer)
        with self.assertRaises(ValueError):m.floor_log(m.Fraction(1),1)
        for value in [True,0.1]:
            with self.subTest(value=value),self.assertRaises(ValueError):m.fraction_bits(value)

    def test_sqrt_rounding_is_bracketed_by_exact_adjacent_midpoints(self):
        self.assertEqual(m.sqrt_bits(2),0x3ff6a09e667f3bcd)
        for value in [m.Fraction(2),m.Fraction(9,4),m.Fraction(1,4),m.Fraction(25)]:
            bits=m.sqrt_bits(value);root=m.bits_fraction(bits);lower=(m.bits_fraction(bits-1)+root)/2;upper=(root+m.bits_fraction(bits+1))/2
            self.assertLess(lower*lower,value);self.assertGreater(upper*upper,value)
        with self.assertRaises(ValueError):m.sqrt_bits(-1)

    def test_human_float_format_uses_fifteen_digits_and_g_thresholds(self):
        for value,text in [(m.Fraction(3,10),'0.3'),(m.Fraction(1,10000),'0.0001'),(m.Fraction(1,100000),'1e-05'),
          (10**14,'100000000000000.0'),(10**15,'1e+15'),(m.Fraction(1999999999999999,10**15),'2.0')]:
            with self.subTest(value=value):self.assertEqual(m.human_float(m.fraction_bits(value)),text)
        self.assertEqual(m.human_float(1<<63),'-0.0');self.assertEqual(m.human_float(0),'0.0')
        self.assertNotEqual(m.rounding_trace()[0],'0.30000000000000004')
        with self.assertRaises(ValueError):m.human_float(0x7ff8000000000000)

    def test_math_return_families_follow_declared_f64_context(self):
        for name,args in [('abs',(-42,)),('min',(3,7,5)),('max',(3,7,5)),('clamp',(5,10,0))]:
            with self.subTest(name=name):self.assertIsInstance(m.math_value(name,*args),m.FloatValue);self.assertEqual(m.math_value(name,*args).typename(),'f64')
        self.assertEqual(m.math_value('abs',-(2**63)).fraction(),2**63)
        for name,value in [('floor',m.Fraction(9,5)),('ceil',m.Fraction(6,5)),('round',m.Fraction(8,5)),('trunc',m.Fraction(9,5)),('sign',-10)]:
            with self.subTest(name=name):self.assertIsInstance(m.math_value(name,value),m.IntegerValue);self.assertEqual(m.math_value(name,value).typename(),'i64')
        with self.assertRaises(ValueError):m.math_value('abs',True)

    def test_reversed_clamp_bounds_and_nan_predicates_keep_float_contract(self):
        for value,answer in [(5,5),(15,10),(-1,0)]:self.assertEqual(m.math_value('clamp',value,10,0).fraction(),answer)
        nan=m.math_value('sqrt',-1);self.assertTrue(m.math_value('isNaN',nan));self.assertFalse(m.math_value('isFinite',nan))
        self.assertTrue(m.math_value('min',1,nan).nan());self.assertTrue(m.math_value('isFinite',1))
        with self.assertRaises(ValueError):m.math_value('cos',1)

    def test_all_original_math_call_occurrences_and_aliases_remain(self):
        from collections import Counter
        import re
        text=(m.ROOT/m._SYSTEM_PROGRAMS['math_core_direct_type_preserve']).read_text(encoding='utf-8')
        declared=re.findall(r'\bmath\.([A-Za-z0-9_]+)\s*\(',text)
        for alias,name in [('mmin','min'),('mmax','max'),('cclamp','clamp')]:declared.extend([name]*len(re.findall(r'\b'+alias+r'\s*\(',text)))
        values,calls=m.math_core_trace();self.assertEqual(len(values),45);self.assertEqual(len(calls),42);self.assertEqual(Counter(calls),Counter(declared))
        self.assertEqual(calls.count('sqrt'),2);self.assertEqual(calls.count('min'),3)

    def test_uuid_masks_cover_every_input_byte_without_assuming_entropy(self):
        domains=m.uuid_domains();self.assertEqual(len(domains),36);self.assertEqual(domains[14],frozenset('4'));self.assertEqual(domains[19],frozenset('89ab'))
        for position in [6,8]:
            for byte in range(256):
                data=bytearray(16);data[position]=byte;text=m.uuid_from_bytes(data)
                self.assertEqual([len(part) for part in text.split('-')],[8,4,4,4,12]);self.assertEqual(text[14],'4');self.assertIn(text[19],'89ab')
                self.assertTrue(all(char in domain for char,domain in zip(text,domains)))
        with self.assertRaises(ValueError):m.uuid_from_bytes(b'')

    def test_random_hex_symbolic_domain_keeps_length_and_lowercase_alphabet(self):
        for count in [8,16]:
            domains=m.random_hex_domains(count);self.assertEqual(len(domains),2*count);self.assertTrue(all(domain==frozenset('0123456789abcdef') for domain in domains))
        for count in [0,-1,True]:
            with self.subTest(count=count),self.assertRaises(ValueError):m.random_hex_domains(count)
        self.assertEqual(m.random_trace(),[True]*11)

    def test_domain_predicates_require_same_base_and_positive_live_charge(self):
        before=m.DomainRead('before',True);during=m.DomainRead('before',True,m.channel_charge_minimum(16))
        self.assertTrue(during.guaranteed_greater(before));self.assertFalse(before.guaranteed_greater(before));self.assertFalse(m.DomainRead('other',True,100).guaranteed_greater(before))
        self.assertFalse(m.DomainRead('unknown_sign',False,10).at_least_zero());self.assertFalse(m.DomainRead('invalid_charge',True,-1).at_least_zero())
        a,b=m.DomainRead('postclose_a',True),m.DomainRead('postclose_b',True);self.assertTrue(a.at_least_zero() and b.at_least_zero());self.assertFalse(a.guaranteed_greater(b))
        for count in [-1,True]:
            with self.subTest(count=count),self.assertRaises(ValueError):m.channel_charge_minimum(count)

    def test_new_exact_whole_source_bindings_and_observation_denominators(self):
        rows=[row for row in m.expected_rows() if row['original_source'] in m._SYSTEM_PROGRAMS.values()]
        self.assertEqual(len(rows),4);self.assertEqual(sum(row['observation_count'] for row in rows),67)
        for row in rows:self.assertEqual(row['source'],row['original_source']);self.assertEqual(row['expected']['exit_code'],0)

if __name__=='__main__':unittest.main()
