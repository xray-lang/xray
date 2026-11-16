# 已接受目标规则：284 - 结构化仿射所有权语法与跨执行转移契约改造

> 2026-11-16 从任务文档迁入统一规范源。此处维护已接受的目标定义；原任务只维护实施、依赖、理由与验收记录。
> 迁入不表示实现完成。下面保留的示例是目标验收材料，不能据此声称当前编译器、VM 或 AOT 已支持。历史修订和已明确被取代的规则保持历史身份；不能将旧版本重新激活。
> 实施任务：[284-structured-affine-ownership-syntax-and-transfer-contract-cn](../../../../xray-docs/tasks/284-structured-affine-ownership-syntax-and-transfer-contract-cn.md)。


## 已整合的权威定义

原任务的有效语义已由以下规范条文承接。本页只保留来源索引，不再重复旧定义；原任务中已被取代的条文不得恢复为现役规则。

- [§2.14 参数、receiver、借用与所有权](../sections/003-2-type-system.md)。
- [§5.2 参数模式](../sections/006-5-declarations.md)。
- [当前所有权与 callable 细则](313-value-ownership-callable-and-module-contracts-cn.md)。

<!-- Preserved task anchors point to the authoritative definitions above. -->
<a id="4-终局语法"></a>
<a id="41-参数模式不变"></a>
<a id="42-receiver-mode-使用方法前缀"></a>
<a id="43-borrowed-return-使用签名级-from-消歧"></a>
<a id="431-from-省略规则"></a>
<a id="432-return-path-验证"></a>
<a id="433-const-返回能力与-binding-mutability-分轴"></a>
<a id="44-local-view-使用前向失效不强制词法借用块"></a>
<a id="45-并发表面不变"></a>
<a id="5-精确语义规则"></a>
<a id="51-read"></a>
<a id="510-returnawaitgochannel"></a>
<a id="52-ref"></a>
<a id="53-move"></a>
<a id="54-receiver"></a>
<a id="55-borrowed-return"></a>
<a id="56-sliceptr-的前向-validity-与其他-loan"></a>
<a id="57-external-strong-alias"></a>
<a id="58-adoption-与-internal-graph"></a>
<a id="59-closure"></a>
