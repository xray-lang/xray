"""Reject incomplete reports without inventing product execution results."""
import copy
import unittest
from unittest import mock
import task_rejection_model as model

IDENTITIES = {'checked_schema':28, 'semantic_identity':75, 'value_abi':25}


def report():
    base = {'status':0,'public_stage':4,'check_stage':6,'check_status':0,'source_status':0,
            'product_published':True,'packet_owned':True,'producer_dropped':True,
            'result_i64':False,'result_class':False,'function_count':1,'storage':0,'sendable':0,'qualified':True}
    negative = dict(base,ordinal=0,status=9,check_stage=1,check_status=3,source_status=3,
                    product_published=False,packet_owned=False,producer_dropped=False,function_count=0)
    return dict(IDENTITIES,schema=1,scope='CONTROLLED_CHECKED_TASK_RESULT',qualified=True,
                physical_zero_verified=False,ledger_baseline=80,ledger_final=80,
                cases=[negative,dict(base,ordinal=1,result_class=True,sendable=3),
                       dict(base,ordinal=2,result_i64=True)])


class RejectionControls(unittest.TestCase):
    def test_complete_protocol_is_valid(self):
        model.validate_report(report(),IDENTITIES)

    def test_incomplete_protocols_are_rejected(self):
        mutations = {
            'owner_failed':lambda r:r.update(qualified=False),
            'wrong_stage':lambda r:r['cases'][0].update(check_stage=3),
            'unsupported_is_not_rejection':lambda r:r['cases'][0].update(check_status=11),
            'published_negative':lambda r:r['cases'][0].update(product_published=True),
            'control_missing':lambda r:r['cases'].pop(),
            'control_duplicated':lambda r:r['cases'][2].update(ordinal=1),
            'borrowed_packet':lambda r:r['cases'][1].update(packet_owned=False),
            'producer_alive':lambda r:r['cases'][1].update(producer_dropped=False),
            'class_is_sendable':lambda r:r['cases'][1].update(sendable=0),
            'scalar_not_sendable':lambda r:r['cases'][2].update(sendable=3),
            'wrong_class':lambda r:r['cases'][1].update(result_class=False),
            'wrong_storage':lambda r:r['cases'][1].update(storage=3),
            'ledger_leak':lambda r:r.update(ledger_final=81),
            'physical_claim':lambda r:r.update(physical_zero_verified=True),
            'wrong_identity':lambda r:r.update(value_abi=0),
            'untyped_status':lambda r:r['cases'][1].update(status=False),
        }
        for name,change in mutations.items():
            with self.subTest(name=name):
                changed=copy.deepcopy(report());change(changed)
                with self.assertRaises(ValueError):model.validate_report(changed,IDENTITIES)

    def test_missing_source_binding_is_rejected(self):
        with mock.patch.object(model,'_INPUTS',{}):
            with self.assertRaises(ValueError):model.expected()

    def test_wrong_selected_identity_is_rejected(self):
        row={'source_lf_sha256':'a'*64}
        selection={'case_id':model.ORIGINAL,'rejection_oracle_required':True,
                   'selected_source':model.ORIGINAL,'selected_source_lf_sha256':'a'*64}
        with mock.patch.object(model,'expected',return_value=row):
            with self.assertRaises(ValueError):model.for_selection(selection)


if __name__ == '__main__':unittest.main()
