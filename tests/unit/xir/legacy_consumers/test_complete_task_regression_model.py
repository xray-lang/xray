"""Independent complete task entry traces, named bodies and state distinctions."""
from pathlib import Path
import unittest
from unittest import mock
import complete_task_regression_model as model


class TaskRegressionTraces(unittest.TestCase):
    def test_every_complete_entry_trace(self):
        expected={
          '1100_cancelled':'false\n',
          '1101_channel_basic':'true\ntrue\ntrue\ntrue\nfalse\n10\n20\n30\ntrue\nfalse\ndone\n',
          '1102_go_await':'30\n30\n60\ndone\n',
          '1103_defer':'start\nwork done\ncleanup 3\ncleanup 2\ncleanup 1\nend\n',
          '1104_coroutine_combined':'=== 协程综合测试 ===\nproducer done\n6\nfalse\n=== 测试完成 ===\ncleanup done\n',
          '1105_defer_args':'start\nwork done\nresult\n42\ncleanup 2\ncleanup 1\nend\n',
          '1106_select':'100\n200\nno data\ndone\n',
          '1107_go_deferred':'start\n1\n2\n3\n1\nend\n',
          '1108_await_all':'4\n9\n16\n29\ndone\n',
          '1110_scope':'10\n20\n1\ndone\n',
          '1111_comprehensive':'1\n2\n6\nworking\nresource2\nresource1\n100\n',
          '1112_go_lambda_call':'100\n200\n300\ndone\n',
          '1113_select_send':'sent 100\nsent 200\nbuffer full\n100\n200\ntrue\ndone\n',
          '1114_select_after':'timeout\n42\nimmediate timeout\ndone\n',
          '1115_cancel':'t1 cancelled: true\nt1 status: TaskStatus.Cancelled\nt2 success: true\nt2 result: 42\ndone\n',
          '1118_comprehensive':'=== go/await ===\ntask1 result: 42\n=== status ===\ndone: true\nstatus: TaskStatus.Success\nresult: 100\n=== cancel ===\ncancelled: true\ncancelled status: TaskStatus.Cancelled\n=== channel ===\nrecv: 1 2 3\n=== defer ===\nworking...\ncleanup!\n=== await all ===\nall results: [1, 2, 3]\n=== await any ===\nfirst result: winner\n=== done ===\n',
          '1119_await_all_syntax':'test1: await all basic\n  r0: 10\n  r1: 20\n  r2: 30\n  sum: 60\ntest2: await all two tasks\n  r0: 10\n  r1: 20\n',
          '1120_scope_basic':'=== scope 基础测试 ===\nscope 结束\n任务数量: 6\n所有任务完成\n=== 嵌套 scope 测试 ===\n嵌套 scope 结果数量: 5\n',
          '1121_defer_block':'=== defer 块语法测试 ===\n开始\n结束\ndefer 块\n=== 多个 defer 块 ===\n执行中\n结束\ndefer 2\ndefer 1\n=== defer 混合测试 ===\n执行\ndefer 块 A\ncleanup 函数\n',
          '1122_coro_naming':'','1125_coro_local':'',
          '1128_yield':'test1: main yield\ntest1: after yield\ntest2: result = yielded\ntest3: 10 20 30\ntest4: sum = 10\ntest5: sums = 45 45 45\n',
          '1130_linked_scope':'','1131_linked_go_expr':''}
        traces=model.observations()
        self.assertEqual(set(expected),set(traces));self.assertEqual(sum(map(len,traces.values())),133)
        for name,text in expected.items():
            with self.subTest(program=name):self.assertEqual(''.join(model.render(v)+'\n' for v in traces[name]),text)

    def test_completion_order_is_not_assumed_to_be_lazy_or_fifo(self):
        self.assertEqual(model.ordered_values([30,20,10]),[30,20,10])
        self.assertEqual(model.ordered_values([1,1,2]),[1,1,2])
        self.assertEqual(set(model.joined_counter_paths()),{6})
        self.assertEqual(len(model.joined_counter_paths()),6)

    def test_repeat_await_and_completed_cancel_preserve_one_result(self):
        calls=[]
        task=model.Task(lambda:calls.append('called') or [1,2])
        first=task.await_value();first.append(9);task.cancel()
        self.assertEqual(task.await_value(),[1,2]);self.assertEqual(calls,['called'])
        self.assertEqual(task.executions,1);self.assertEqual(task.poll()[0],'Success')

    def test_blocked_task_cancellation_does_not_fabricate_a_value(self):
        calls=[];ready=[];task=model.Task(lambda:calls.append('called') or 42,lambda:bool(ready))
        task.finish();self.assertEqual(task.state,'Running');self.assertEqual(task.poll(),('Pending',None))
        task.cancel();self.assertEqual(task.await_result(),('Cancelled',None));self.assertEqual(calls,[])
        with self.assertRaises(ValueError):task.await_value()

    def test_select_model_refuses_to_invent_a_competing_ready_priority(self):
        first,second=model.Channel(1),model.Channel(1);first.try_send(1);second.try_send(2)
        with self.assertRaises(ValueError):model.receive_only_ready([first,second],'default')
        self.assertEqual(first.queue,[1]);self.assertEqual(second.queue,[2])
        for name in ['1109_await_any','1116_task_status','1117_await_timeout']:
            self.assertNotIn(name,model._NAMES)

    def test_complete_cleanup_order_and_typed_status_display(self):
        self.assertEqual(model.close_with_defer(['body'],[['first'],['second',42]]),['body','second',42,'first'])
        self.assertEqual(model.line('status:',model.Enum('TaskStatus','Success')),'status: TaskStatus.Success')

    def test_timing_and_yield_requirements_are_retained_without_fake_measurements(self):
        rows={x['id']:x for x in model.expected_rows()}
        self.assertEqual(rows['complete_task_regression_1114_select_after']['required_runtime_timing_predicates'],
            [{'expression':'elapsed >= 20','required':True},{'expression':'elapsed < 500','required':True}])
        self.assertEqual(rows['complete_task_regression_1128_yield']['required_original_yield_checkpoints'],39)

    def test_declaration_only_modules_have_separate_full_named_body_requirements(self):
        entries={x['id']:x for x in model.expected_rows()};named=model.named_expected_rows();self.assertEqual(len(named),4)
        for row in named:
            name=row['id'].removeprefix('complete_task_named_')
            self.assertEqual(entries['complete_task_regression_'+name]['expected']['stdout_utf8'],'')
            self.assertFalse(entries['complete_task_regression_'+name]['separate_named_body_obligation']['executed_by_run'])
            self.assertEqual(row['expected']['required_test_names'],[model._DECLARATION_ONLY[name]])
            self.assertTrue(all(row['required_original_assertion_results']))
        facts=model.named_requirements()
        self.assertEqual(facts['1122_coro_naming']['stdout_utf8'],'10\n10\n20\n30\n40\n50\ndone\n')
        self.assertEqual([len(facts[name]['assertion_results']) for name in ['1125_coro_local','1130_linked_scope','1131_linked_go_expr']],[9,4,4])

    def test_every_complete_source_and_authority_is_pinned(self):
        saved=Path.read_bytes
        for name in model._INPUTS:
            with self.subTest(input=name):
                def changed(path):return saved(path)+b'\nchanged\n' if path==model.ROOT/name else saved(path)
                with mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):model.expected_rows()

    def test_exact_complete_program_set_cannot_be_swapped_or_dropped(self):
        swapped=dict(model._PROGRAMS);names=sorted(swapped);swapped[names[0]],swapped[names[1]]=swapped[names[1]],swapped[names[0]]
        for attribute,value in [('_PROGRAMS',swapped),('_PROGRAMS',{}),('_INPUTS',{})]:
            with self.subTest(attribute=attribute),mock.patch.object(model,attribute,value),self.assertRaises(ValueError):model.expected_rows()


if __name__=='__main__':unittest.main()
