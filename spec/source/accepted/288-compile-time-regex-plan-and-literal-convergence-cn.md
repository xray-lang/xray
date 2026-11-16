# 已接受目标规则：288 - 编译期 RegexPlan、正则字面量与 VM/AOT 收敛

> 2026-11-16 从任务文档迁入统一规范源。此处维护已接受的目标定义；原任务只维护实施、依赖、理由与验收记录。
> 迁入不表示实现完成。下面保留的示例是目标验收材料，不能据此声称当前编译器、VM 或 AOT 已支持。历史修订和已明确被取代的规则保持历史身份；不能将旧版本重新激活。
> 实施任务：[288-compile-time-regex-plan-and-literal-convergence-cn](../../../../xray-docs/tasks/288-compile-time-regex-plan-and-literal-convergence-cn.md)。


## 已整合的权威定义

原任务的有效语义已由以下规范条文承接。本页只保留来源索引，不再重复旧定义；原任务中已被取代的条文不得恢复为现役规则。

- [G5 正则字面量决定](331-language-surface-convergence-cn.md)。
- [正则字面量的参考条文](../sections/002-1-lexical-structure.md)。
- [Regex API 的参考条文](../sections/015-14-built-in-type-methods.md)。

<!-- Preserved task anchors point to the authoritative definitions above. -->
<a id="2-终局语言与-api-语义"></a>
<a id="21-regex-literal"></a>
<a id="22-constant-regexcompile-folding"></a>
<a id="23-dynamic-compile-与错误通道"></a>
<a id="24-matching-semantics-必须冻结"></a>
