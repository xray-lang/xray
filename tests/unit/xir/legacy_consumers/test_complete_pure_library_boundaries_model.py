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
        self.assertEqual(sum(map(len,traces.values())),45)
        self.assertEqual(traces['crypto_timing_safe_equal_direct'][:12],[True,False,False,True,True,False,False,False,True,False,True,True])
        self.assertEqual(traces['toml_pure_module_direct'],['xray',2,True,'apple','red',2,'Nail',True,'TomlDiagnostic',True,2,'Duplicate key','Alice',True,0,True,True,True,True])
        self.assertEqual(traces['io_read_stdin_shared_core'],[20,False,False,False])
        self.assertEqual(len(m.expected_rows()),3)

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


if __name__=='__main__':unittest.main()
