"""Reject incomplete compiler fault evidence without simulating compiler calls."""
import copy
import unittest
from run_source_rejection_faults import review_records


def records():
    result=[]
    for shard in range(2):
        ordinals=list(range(shard,5,2))
        lines=['sdk-fault ordinal='+str(i)+' code=1 oom=1 injected=1 attempts='+str(i+10)+' physical=0/0' for i in ordinals]
        lines+=['sdk-summary sites=5 shard='+str(shard)+' shards=2 covered='+str(len(ordinals))+' physical=0/0']
        result.append({'exit_code':0,'timed_out':False,'stdout':('\n'.join(lines)+'\n').encode(),'stderr':b''})
    return result


class FaultEvidence(unittest.TestCase):
    def test_exact_ordinal_partition(self):
        self.assertEqual(sum(row['covered'] for row in review_records(5,2,records())),5)

    def test_incomplete_or_unqualified_records(self):
        changes={
            'missing_ordinal':lambda r:r[0].update(stdout=r[0]['stdout'].replace(b'sdk-fault ordinal=2 code=1 oom=1 injected=1 attempts=12 physical=0/0\n',b'')),
            'duplicate_ordinal':lambda r:r[0].update(stdout=r[0]['stdout'].replace(b'ordinal=2',b'ordinal=0')),
            'wrong_shard':lambda r:r[0].update(stdout=r[0]['stdout'].replace(b'shard=0',b'shard=1')),
            'not_OOM':lambda r:r[1].update(stdout=r[1]['stdout'].replace(b'oom=1',b'oom=0')),
            'not_injected':lambda r:r[1].update(stdout=r[1]['stdout'].replace(b'injected=1',b'injected=0')),
            'attempt_not_reached':lambda r:r[0].update(stdout=r[0]['stdout'].replace(b'attempts=10',b'attempts=0')),
            'failed_process':lambda r:r[1].update(exit_code=1),
            'timed_out':lambda r:r[1].update(timed_out=True),
            'missing_summary':lambda r:r[0].update(stdout=b'\n'.join(r[0]['stdout'].splitlines()[:-1])),
            'physical_leak':lambda r:r[0].update(stdout=r[0]['stdout'].replace(b'physical=0/0',b'physical=1/80')),
        }
        for name,change in changes.items():
            with self.subTest(name=name):
                value=copy.deepcopy(records());change(value)
                with self.assertRaises(ValueError):review_records(5,2,value)

    def test_missing_shard(self):
        with self.assertRaises(ValueError):review_records(5,2,records()[:1])


if __name__=='__main__':unittest.main()
