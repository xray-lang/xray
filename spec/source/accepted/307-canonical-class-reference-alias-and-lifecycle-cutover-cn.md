# 已接受目标规则：307 - Canonical class 引用、别名与生命周期原子切换

> 2026-11-16 从任务文档迁入统一规范源。此处维护已接受的目标定义；原任务只维护实施、依赖、理由与验收记录。
> 迁入不表示实现完成。下面保留的示例是目标验收材料，不能据此声称当前编译器、VM 或 AOT 已支持。历史修订和已明确被取代的规则保持历史身份；不能将旧版本重新激活。
> 实施任务：[307-canonical-class-reference-alias-and-lifecycle-cutover-cn](../../../../xray-docs/tasks/307-canonical-class-reference-alias-and-lifecycle-cutover-cn.md)。


## 已整合的权威定义

原任务的有效语义已由以下规范条文承接。本页只保留来源索引，不再重复旧定义；原任务中已被取代的条文不得恢复为现役规则。

- [class 身份、别名和值语义](../sections/003-2-type-system.md)。
- [class 声明与初始化](../sections/006-5-declarations.md)。
- [所有权与生命周期细则](313-value-ownership-callable-and-module-contracts-cn.md)。

<!-- Preserved task anchors point to the authoritative definitions above. -->
<a id="2-canonical-类型合同"></a>
<a id="21-class_reference"></a>
<a id="22-递归类型与-key"></a>
<a id="23-copydrop-plan"></a>
