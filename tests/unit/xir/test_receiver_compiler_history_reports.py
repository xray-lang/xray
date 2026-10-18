"""Independent small failure trees and rejection cases for compiler FI receipts."""
import copy,json,subprocess,sys,tempfile,time,unittest
from pathlib import Path
import run_receiver_compiler_faults as reports

def point(history,attempts):
    return {'event':'point','ordinal':history[0],'history':history,'fired':len(history),'attempts':attempts,
            'status':7,'injected':True,'live':80,'owner_live':80,'allocated':100,'peak':80,'work':100,
            'allocations':1,'compiler_blocks':0,'compiler_bytes':0}

class HistoryReports(unittest.TestCase):
    def verify(self,rows,roots=None):
        return reports.verify_histories(rows,0,1,set()if roots is None else roots,time.monotonic()+2)
    def tree(self):
        # A second OOM creates allocations absent from both the normal run
        # and the first-failure path. Flat original-ordinal coverage misses them.
        return [point([0],2),point([0,1],4),point([0,1,2],3),point([0,1,3],4)]
    def test_complete_new_suffix_tree(self):
        self.assertEqual(self.verify(self.tree()),(4,3))
    def test_sibling_failures_are_distinct(self):
        self.assertEqual(self.verify([point([0],3),point([0,1],2),point([0,2],3)]),(3,2))
    def test_normal_primary_only_is_incomplete(self):
        with self.assertRaises(RuntimeError):self.verify([point([0],2)])
    def test_missing_new_allocation_history(self):
        rows=self.tree();del rows[2]
        with self.assertRaises(RuntimeError):self.verify(rows)
    def test_duplicate_history(self):
        rows=self.tree();rows.insert(2,copy.deepcopy(rows[1]))
        with self.assertRaises(RuntimeError):self.verify(rows)
    def test_unreached_requested_fault(self):
        rows=self.tree();rows[2]['fired']=2
        with self.assertRaises(RuntimeError):self.verify(rows)
    def test_uncharged_forged_long_history(self):
        rows=self.tree();rows[2]['history']=[0,1,2,3]
        with self.assertRaises(RuntimeError):self.verify(rows)
    def test_foreign_primary_ordinal(self):
        rows=self.tree();rows[1]['ordinal']=1
        with self.assertRaises(RuntimeError):self.verify(rows)
    def test_duplicate_worker_root(self):
        with self.assertRaises(RuntimeError):self.verify(self.tree(),{0})
    def test_leaking_owner(self):
        rows=self.tree();rows[1]['live']=81
        with self.assertRaises(RuntimeError):self.verify(rows)
    def test_non_oom_status(self):
        rows=self.tree();rows[1]['status']=0
        with self.assertRaises(RuntimeError):self.verify(rows)
    def test_original_census_cap_is_not_enlarged(self):
        rows=self.tree();rows[1]['attempts']=65001
        with self.assertRaises(RuntimeError):self.verify(rows)

def allocator_audit(executable):
    with tempfile.TemporaryDirectory(prefix='receiver-allocator-history-')as folder:
        path=Path(folder)/'audit.jsonl'
        subprocess.run([executable,'--history-audit',str(path)],check=True,timeout=20)
        rows=[json.loads(line)for line in path.read_text(encoding='utf-8').splitlines()]
        assert rows[0]=={'event':'audit','schema':'receiver-allocator-history.v1','normal_sites':1}
        assert rows[-1]=={'event':'complete','points':1,'histories':4,'compiler_blocks':0,'compiler_bytes':0}
        assert [row['history']for row in rows[1:-1]]==[[0],[0,1],[0,1,2],[0,1,3]]
        assert reports.verify_histories(rows[1:-1],0,1,set(),time.monotonic()+2)==(4,3)
    print('Real compiler allocator: four complete histories, two allocations introduced by second OOM, fresh owners and physical0 PASS')

if __name__=='__main__':
    if len(sys.argv)==3 and sys.argv[1]=='--allocator-audit':allocator_audit(sys.argv[2])
    else:unittest.main()
