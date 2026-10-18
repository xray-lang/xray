"""Fail-closed controls for canonical selection and independent observations."""
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest

import complete_target_selection as selection


class Reader:
    @staticmethod
    def read_args(source):
        path = source.with_suffix('.args')
        return json.loads(path.read_text()) if path.is_file() else []

    @staticmethod
    def read_stdin(source):
        path = source.with_suffix('.stdin')
        return path.read_bytes() if path.is_file() else b''


class TargetSelectionControls(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)
        self.assertEqual(self.root.resolve().parent, Path(tempfile.gettempdir()).resolve())
        (self.root/'old.xr').write_bytes(b'print(1)\n')
        (self.root/'new.xr').write_bytes(b'print(7)\n')
        self.row = {'case_id':'old.xr', 'source_lf_sha256':self.hash('old.xr'),
                    'producer_packages':['P10'],
                    'original_differential_obligation':self.context('old.xr')}

    def hash(self, name):
        return selection.digest((self.root/name).read_bytes().replace(b'\r\n', b'\n'))

    def context(self, name):
        source = self.root/name
        result = {'arguments':Reader.read_args(source), 'stdin_hex':Reader.read_stdin(source).hex()}
        for key,suffix in [('arguments_sidecar','.args'),('stdin_sidecar','.stdin')]:
            path = source.with_suffix(suffix)
            item = {'path':path.relative_to(self.root).as_posix(), 'state':'PRESENT' if path.is_file() else 'MISSING'}
            if path.is_file():
                raw=path.read_bytes();item.update(bytes=len(raw),sha256=selection.digest(raw),
                                                lf_sha256=selection.digest(raw.replace(b'\r\n',b'\n')))
            result[key]=item
        return result

    def port(self):
        self.row['target_disposition']={'kind':'POSITIVE_VALUE_AFTER_FROZEN_PORT'}
        self.row['target_source_port']={'fixture':'new.xr','lf_sha256':self.hash('new.xr'),
            'qualification':'NOT_RUN','owner_package':'P10','frozen_basis':['Independent frozen value contract']}
        return selection.select(self.row,self.root,Reader)

    def oracle(self, source='new.xr', output='7\n'):
        return {'id':'independent','source':source,'original_source':'old.xr',
                'expected':{'exit_code':0,'stdout_utf8':output,'stderr_utf8':''}}

    def observed(self, output=b'7\n', code=0, error=b'', timeout=False):
        return SimpleNamespace(returncode=code,stdout=output,stderr=error,timed_out=timeout)

    def test_original_identity_and_context_remain(self):
        selected=selection.select(self.row,self.root,Reader)
        self.assertEqual((selected['case_id'],selected['selected_source']),('old.xr','old.xr'))
        self.assertFalse(selected['canonical_port_selected'])

    def test_reviewed_port_is_the_only_selected_source(self):
        selected=self.port()
        self.assertEqual((selected['case_id'],selected['original_source'],selected['selected_source']),
                         ('old.xr','old.xr','new.xr'))

    def test_corrupt_original_source_rejects(self):
        (self.root/'old.xr').write_bytes(b'changed')
        with self.assertRaises(ValueError):selection.select(self.row,self.root,Reader)

    def test_corrupt_target_source_rejects(self):
        self.port();(self.root/'new.xr').write_bytes(b'changed')
        with self.assertRaises(ValueError):selection.select(self.row,self.root,Reader)

    def test_missing_required_port_never_falls_back(self):
        self.row['target_disposition']={'kind':'POSITIVE_VALUE_AFTER_FROZEN_PORT'}
        with self.assertRaises(ValueError):selection.select(self.row,self.root,Reader)

    def test_new_empty_sidecar_is_still_a_change(self):
        (self.root/'old.stdin').write_bytes(b'')
        with self.assertRaises(ValueError):selection.select(self.row,self.root,Reader)

    def test_same_parsed_arguments_cannot_hide_changed_sidecar_bytes(self):
        (self.root/'old.args').write_bytes(b'["a"]');self.row['original_differential_obligation']=self.context('old.xr')
        (self.root/'old.args').write_bytes(b'[ "a" ]')
        with self.assertRaises(ValueError):selection.select(self.row,self.root,Reader)

    def test_declared_target_context_overrides_only_after_exact_checks(self):
        self.port();(self.root/'new.args').write_text('["alpha","beta"]');(self.root/'new.stdin').write_bytes(b'x\x00y')
        self.row['target_source_port']['differential_input_obligation']=self.context('new.xr')
        selected=selection.select(self.row,self.root,Reader)
        self.assertEqual(selected['arguments'],['alpha','beta']);self.assertEqual(selected['stdin_hex'],'780079')
        self.assertEqual(selected['original_arguments'],[])

    def test_corrupt_companion_rejects(self):
        self.port();(self.root/'lib.xr').write_bytes(b'body')
        self.row['target_source_port']['companion_inputs']=[{'path':'lib.xr','lf_sha256':'0'*64}]
        with self.assertRaises(ValueError):selection.select(self.row,self.root,Reader)

    def test_foreign_path_rejects(self):
        self.port();self.row['target_source_port']['fixture']='../outside.xr'
        with self.assertRaises(ValueError):selection.select(self.row,self.root,Reader)

    def test_old_source_model_cannot_qualify_changed_target(self):
        selected=self.port()
        self.assertIsNone(selection.expected_for(selected,[],[self.oracle('old.xr')]))

    def test_target_model_cannot_borrow_another_original_identity(self):
        selected=self.port();model=self.oracle();model['original_source']='another.xr'
        self.assertIsNone(selection.expected_for(selected,[],[model]))

    def test_old_sidecar_and_backend_parity_cannot_qualify_canonical_target(self):
        selected=self.port();observed=self.observed()
        self.assertEqual(selection.observation_status(selected,None,observed,observed,True,b'7\n'),
                         'EXECUTED_CANONICAL_TARGET_INDEPENDENT_ORACLE_PENDING')

    def test_exact_independent_model_is_required_for_pass(self):
        selected=self.port();oracle=selection.expected_for(selected,[],[self.oracle()]);observed=self.observed()
        self.assertEqual(selection.observation_status(selected,oracle,observed,observed,True),
                         'PASS_INDEPENDENT_SELECTED_RUN_BYTE_SCOPE')
        self.assertEqual(selection.observation_status(selected,oracle,observed,observed,False),
                         'FAIL_INDEPENDENT_SELECTED_CONSUMER')
        self.assertEqual(selection.observation_status(selected,oracle,self.observed(error=b'bad'),observed,True),
                         'FAIL_INDEPENDENT_SELECTED_CONSUMER')

    def test_conflicting_independent_models_reject(self):
        selected=self.port()
        with self.assertRaises(ValueError):selection.expected_for(selected,[],[self.oracle(),self.oracle(output='8\n')])

    def test_canonical_preserved_oracle_needs_owned_selected_binding(self):
        selected=self.port();oracle={'id':'preserved','execution_source':'new.xr','inputs':[],
                                    'expected':self.oracle()['expected']}
        with self.assertRaises(ValueError):selection.expected_for(selected,[oracle],[])
        oracle['inputs']=[{'path':'new.xr','lf_sha256':selected['selected_source_lf_sha256']}]
        self.assertEqual(selection.expected_for(selected,[oracle],[])['id'],'preserved')

    def test_any_compiler_failure_cannot_qualify_frozen_rejection(self):
        selected=self.port();selected['rejection_oracle_required']=True;failure=self.observed(code=1,error=b'wrong error')
        self.assertIsNone(selection.expected_for(selected,[],[self.oracle()]))
        self.assertEqual(selection.observation_status(selected,None,failure,failure,False),
                         'EXECUTED_FROZEN_TARGET_REJECTION_ORACLE_PENDING')

    def test_original_stdout_sidecar_stays_unqualified(self):
        selected=selection.select(self.row,self.root,Reader);observed=self.observed()
        self.assertEqual(selection.observation_status(selected,None,observed,observed,True,b'7\n'),
                         'OBSERVED_ORIGINAL_STDOUT_MATCH_OTHER_DUTIES_OPEN')


if __name__=='__main__':
    unittest.main()
