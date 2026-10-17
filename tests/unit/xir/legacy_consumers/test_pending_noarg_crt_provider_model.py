from pathlib import Path
import unittest
from unittest import mock
import pending_noarg_crt_provider_model as model
import complete_ffi_memory_model as formal_model

class PendingNoargCrtProvider(unittest.TestCase):

    def test_reviewed_ucrt_seed_is_not_a_guessed_or_product_derived_oracle(self):
        self.assertEqual(model.ucrt_rand_sequence(7,1),[61])
        self.assertEqual(model.ucrt_rand_sequence(0,4),[38,7719,21238,2437])
        self.assertEqual(model.ucrt_rand_sequence(7,4),[61,17422,15215,7040])
        self.assertEqual(model.ucrt_rand_sequence(2**32-1,4),[35,29739,3374,11141])

    def test_ucrt_seed_width_count_and_bool_are_checked_before_any_oracle(self):
        for seed,count in ((-1,1),(2**32,1),(True,1),(7,0),(7,33),(7,True),(7.0,1)):
            with self.subTest(seed=seed,count=count),self.assertRaises(ValueError):model.ucrt_rand_sequence(seed,count)
        self.assertEqual(len(model.ucrt_host_probe_rows()),24)
        self.assertEqual(model.ucrt_host_probe_rows()[8],'7:1:61')

    def test_pending_rand_expectation_rejects_unknown_provider_and_keeps_complete_declarations(self):
        for provider in ('windows-x86_64','ISO-C','glibc','msvcrt',''):
            with self.assertRaises(ValueError):model.pending_noarg_crt_expectation(provider)
        row=model.pending_noarg_crt_expectation(model._UCRT_PROFILE)
        self.assertEqual(row['conditional_stdout_utf8'],'61\n')
        self.assertEqual([(x['name'],x['arguments'],x['result']) for x in row['source_calls']],[('srand',1,'()'),('rand',0,'i32')])
        self.assertEqual(row['source_unused_declarations'][0]['name'],'xrayffi_unused_float')
        self.assertFalse(row['registered_product_oracle']);self.assertFalse(row['removed_from_missing_expectations'])

    def test_all_pending_provider_sources_and_authorities_are_exactly_pinned(self):
        saved=Path.read_bytes
        for name in model._PENDING_NOARG_FFI_INPUTS:
            def changed(path):return saved(path)+b'changed' if path==model.ROOT/name else saved(path)
            with self.subTest(path=name),mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):
                model.validate_pending_noarg_provider_inputs()
        with mock.patch.object(model,'_PENDING_NOARG_FFI_INPUTS',{}),self.assertRaises(ValueError):model.validate_pending_noarg_provider_inputs()

    def test_complete_foreign_original_is_not_replaced_or_silently_added_to_formal_rows(self):
        text=(model.ROOT/model._PENDING_NOARG_FFI_SOURCE).read_text(encoding='utf-8')
        for declaration in ('fn srand(seed: u32) -> ()','fn rand() -> i32','fn xrayffi_unused_float(x: f64) -> f64'):
            self.assertIn(declaration,text)
        self.assertIn('unsafe { srand(7) }',text)
        self.assertIn('print(unsafe { rand() })',text)
        self.assertEqual(text.count('fn '),3);self.assertEqual(text.count('print('),1)
        self.assertNotIn(model._PENDING_NOARG_FFI_SOURCE,{row['source'] for row in formal_model.expected_rows()})

    def test_host_probe_shape_does_not_grant_xray_binding_or_all_provider_qualification(self):
        text=(model.ROOT/model._PENDING_NOARG_FFI_FIXTURE/'probe.c').read_text(encoding='utf-8')
        self.assertIn('int (__cdecl *next_value)(void) = rand;',text)
        self.assertIn('void (__cdecl *seed_value)(unsigned int) = srand;',text)
        self.assertIn('extern double xrayffi_unused_float(double value);',text)
        self.assertNotIn('xrayffi_unused_float(',text.split('int main(void)')[1])
        row=model.pending_noarg_crt_expectation(model._UCRT_PROFILE)
        for key in ('actual_Xray_CRT_module_identity_and_same_thread_state_qualified',
                    'actual_Xray_noarg_void_descriptors_and_unused_link_manifest_qualified',
                    'all_other_providers_FI_resources_lifetime_safety_mixed_qualified'):
            self.assertFalse(row[key])

if __name__ == '__main__': unittest.main()
