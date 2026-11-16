# 已接受目标规则：316 - 可恢复协程、生成器与取消清理

> 2026-11-16 从任务文档迁入统一规范源。此处维护已接受的目标定义；原任务只维护实施、依赖、理由与验收记录。
> 迁入不表示实现完成。下面保留的示例是目标验收材料，不能据此声称当前编译器、VM 或 AOT 已支持。历史修订和已明确被取代的规则保持历史身份；不能将旧版本重新激活。
> 实施任务：[316-resumable-coroutine-generator-and-cancellation-cn](../../../../xray-docs/tasks/316-resumable-coroutine-generator-and-cancellation-cn.md)。


## 已整合的权威定义

原任务的有效语义已由以下规范条文承接。本页只保留来源索引，不再重复旧定义；原任务中已被取代的条文不得恢复为现役规则。

- [协程、生成器与取消](../sections/011-10-concurrency-and-coroutines.md)。
- [声明错误与 Iterator<T,E>](329-errors-in-signatures-cn.md)。
- [已接受的调度和挂起目标细则](335-coroutine-execution-and-scheduler-cn.md)。

<!-- Preserved task anchors point to the authoritative definitions above. -->
<a id="2-可恢复调用与效应合同"></a>
<a id="21-函数与调用边界"></a>
<a id="22-no_suspend-与-no_blocking"></a>
<a id="23-泛型和符号效应"></a>
<a id="24-vmnative-高阶回调门"></a>
<a id="25-atomic-有限重试与独立参数故障合同目标实施pending"></a>
<a id="4-生成器和视图"></a>
<a id="41-独立生命周期"></a>
<a id="42-首版外部借用限制"></a>
<a id="5-取消异常与-ffi"></a>
<a id="51-取消状态机"></a>
<a id="52-c-边界"></a>
<a id="53-catch-头部与-enum-pattern-handler2026-09-29-用户已定"></a>
