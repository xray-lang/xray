# 已接受目标规则：247 - 纯 RC 内存模型收敛：移除 ORC、原子删除弱容器、`weak` 字段与开发期环检测器

> 2026-11-16 从任务文档迁入统一规范源。此处维护已接受的目标定义；原任务只维护实施、依赖、理由与验收记录。
> 迁入不表示实现完成。下面保留的示例是目标验收材料，不能据此声称当前编译器、VM 或 AOT 已支持。历史修订和已明确被取代的规则保持历史身份；不能将旧版本重新激活。
> 实施任务：[247-rc-cycle-model-layered-convergence-and-weak-container-removal-cn](../../../../xray-docs/tasks/247-rc-cycle-model-layered-convergence-and-weak-container-removal-cn.md)。


## 已整合的权威定义

原任务的有效语义已由以下规范条文承接。本页只保留来源索引，不再重复旧定义；原任务中已被取代的条文不得恢复为现役规则。

- [引用计数、环与回收域；已接受的生命周期修订已在正文收敛](../sections/017-16-runtime-model.md)。

<!-- Preserved task anchors point to the authoritative definitions above. -->
<a id="2-目标模型"></a>
<a id="21-默认体验"></a>
<a id="22-168-升级为更强的承诺"></a>
<a id="23-层间禁止事项"></a>
<a id="24-闭包环裁决只给诊断与惯用法不给语言机制"></a>
<a id="25-不自动插入-weak否决类似自动分号插入的方案"></a>
