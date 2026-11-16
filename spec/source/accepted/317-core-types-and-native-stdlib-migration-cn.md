# 已接受目标规则：317 - 核心类型、写时复制与 XIR 标准库迁移

> 2026-11-16 从任务文档迁入统一规范源。此处维护已接受的目标定义；原任务只维护实施、依赖、理由与验收记录。
> 迁入不表示实现完成。下面保留的示例是目标验收材料，不能据此声称当前编译器、VM 或 AOT 已支持。历史修订和已明确被取代的规则保持历史身份；不能将旧版本重新激活。
> 实施任务：[317-core-types-and-native-stdlib-migration-cn](../../../../xray-docs/tasks/317-core-types-and-native-stdlib-migration-cn.md)。


## 已整合的权威定义

原任务的有效语义已由以下规范条文承接。本页只保留来源索引，不再重复旧定义；原任务中已被取代的条文不得恢复为现役规则。

- [核心类型和值语义](../sections/003-2-type-system.md)。
- [内建类型 API](../sections/015-14-built-in-type-methods.md)。
- [Map、名义类型与结构对象删除](342-remove-structural-objects-map-braces-named-arguments-cn.md)。

<!-- Preserved task anchors point to the authoritative definitions above. -->
<a id="3-核心类型合同"></a>
<a id="31-当前浮点族实施边界"></a>
<a id="311-runtime-f64-与-constructed-atomic-的下一闭包目标实施pending"></a>
<a id="32-用户-struct-的实施入口与复用审计"></a>
<a id="321-主入口类型与权限表示复核2026-09-27"></a>
<a id="322-名义元数据实施入口2026-09-27"></a>
<a id="4-cow-与嵌套修改"></a>
