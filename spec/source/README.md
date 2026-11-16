# 结构化语言规范源

本目录组织 Xray 已接受语言规则的唯一权威源。编译器是规则的实现；任务文档保存提案、理由、实施计划和验收记录。实现缺口不得反过来改写已接受的规则。

中文条文是规则的裁决文本，英文是受审翻译；语法、示例程序及声明使用语言无关的共享定义。翻译不得增加、删除或改变规则。变更规则时同步更新对应翻译和受影响测试。

已从现役任务迁入的目标细则见 [accepted/README.md](accepted/README.md)。其“已接受”不表示已实现。历史修订、弃案和进度仍保持原有身份；迁移不产生新的语言决定。

## 规则与实现证据

- 新方案在 task 中讨论；接受时将最终规则写入本规范工程，task 引用条文并保留实施安排。
- 同一规则只保留一个可编辑定义。已接受目标细则整合进参考章节时，删除细则的重复正文，保留引用和决定来历。
- 现存明确的取代关系继续生效；没有说明的规范矛盾必须修复或登记为缺陷，不能以代码、文件日期或任务编号静默裁决。
- 解析、检查、VM、AOT 的支持情况分别来自实际测试报告，并带源码/产物/平台身份。没有证据即未验证；不把声明存在、生成成功或无新增失败算作功能完成。
- `@run`、`@check`、`@error`、`@fragment` 保持各自验证强度；尚未实现的目标验收材料不冒充当前可运行教程。完整 G13 是否符合与棘轮是否通过分别报告。

## 共享定义

| 内容 | 唯一可编辑源 | 投影 |
|---|---|---|
| 已接受的语言语义 | `sections` 中文正文及 `accepted` 的目标细则 | 英文翻译、手册、工具检索 |
| 文法产生式 | `grammar/language.ebnf` | `grammar/views.json` 选择正文/附录视图 |
| 示例代码、模式和输出 | `examples/*.json` | 双语章节与 MCP 示例；翻译只改注释和空白 |
| 保留关键字声明 | `src/frontend/lexer/xkeywords.def` 与 `src/shared/xr_exact_scalar_registry.def` | 文档数量、索引与 API inventory |
| 关键词解释 | `lexical/keywords.json` | 双语关键词展示；不得独立添加保留字 |
| 类型、方法、模块 API 声明 | 已有受治理的 `.xr` 和注册表 | API inventory 与 MCP API 表 |

声明文件属于本规范工程组织的输入，不在这里再复制签名。复杂语义和编译器实现保持独立验证，不要求整个编译器由规范生成，也不恢复独立 contracts/digest 注册机制。

`sections` 中 `<!-- xr-source:... -->` 包围的区域是可读的生成投影，禁止手改。正文与双语说明仍在章节维护；所有 Xray 示例和 EBNF 必须引用共享源。生成器先验证全部输入，再刷新区域及手册；`--check` 拒绝过期投影且不写文件。

生成输出：

```text
LANGUAGE_SPEC_CN.md
docs/knowledge/**
LANGUAGE_SPEC.md
```

不要直接编辑这些生成输出。需要重新生成时运行：

```sh
python3 scripts/gen_language_docs.py --root .
```

## 目录结构

```text
spec/source/
  sections/*.md            # 中英文语言参考手册章节源
  accepted/*.md            # 从现役任务迁入的已接受目标细则
  grammar/language.ebnf    # 每个产生式只定义一次
  grammar/views.json       # 章节选择产生式，不另写定义
  examples/*.json          # 单一程序/模式/输出与注释排版翻译
  lexical/keywords.json    # 词法声明的章节索引与说明
  cards/topics/*.json      # MCP topic 结构化投影源
  cards/stdlib/*.json      # stdlib 模块说明源；API 表后续自动注入
  cards/resources/*.json   # MCP resource 结构化投影源
  knowledge/README.md      # docs/knowledge/README.md 的源
```

## 章节格式

`sections/` 下每个文件都包含一个小 frontmatter，后面跟两个显式语言块：

```md
---
id: spec.type_system
order: 003
---

<!-- xr-spec:cn -->
Chinese reference text.
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
English reference text.
<!-- /xr-spec:en -->
```

规则：

- `id` 必须全局唯一。
- `order` 决定输出顺序，也必须全局唯一。
- `cn` 和 `en` 块都必须存在。
- `examples` 的 `code` 是唯一程序；`trivia.en` 按 token 前的间隔编号保存翻译，只允许空白和注释。改变字符串、标识符、运算符或使注释吞掉后续 token 都是错误。
- `grammar/views.json` 只引用 `language.ebnf` 中存在的产生式，重复产生式和未引用来源都是错误。
- Xray 示例必须继续使用 `xray` 代码块；语法高亮属于渲染层，不属于源格式。
- 需要被 MCP knowledge 复用的示例必须给代码块添加 `@id=...`，生成 SPEC 时会自动剥离该标记。
- 禁止用摘要替代详细说明；生成后的 SPEC 必须保持或提升当前文档质量。

## Knowledge cards

`cards/` 下的 JSON 文件是 MCP knowledge 的结构化投影源：

- `topics/<id>.json`：语言主题卡片，包含 title、aliases、spec anchor、短说明和分段内容。
- `resources/<id>.json`：长文本 resource，如 cheatsheet / concurrency / stdlib_list。
- `stdlib/<module>.json`：标准库模块摘要；每个 symbol 的 API 签名来自 analyzer builtin metadata，不在 JSON 中手写复制。

card 中的 `fences` 字段引用 `sections/*.md` 里带 `@id` 的代码块；这样同一个示例可同时渲染到 SPEC 和 MCP knowledge，避免复制漂移。

## sections / cards 同步规则

`sections/*.md` 和 `cards/**/*.json` 需要一起维护，但职责不同：

- `sections/*.md` 是语言语义、详细解释和权威示例的主文档源。
- `cards/**/*.json` 是 MCP knowledge 的索引与投影源，只负责检索入口、摘要组织和示例引用。
- 新增或修改语言语法 / 语义时，先更新 `sections/*.md` 的中文和英文正文，再更新相关 card 的 `title`、`aliases`、`lead`、`spec_anchor` 和分段组织。
- 需要在 MCP knowledge 中出现的 Xray 示例，先写入 `examples/*.json`；由章节的 `xr-source:example` 区域生成带 `@id` 的代码块，再由 card 的 `fences` 引用。没有章节展示的知识示例由 card 的 `source_examples` 直接引用共享源。
- card 不应复制一份独立 Xray 示例来表达同一语义，避免 `sections`、SPEC 和 MCP knowledge 漂移。
- 只调整 MCP 检索关键词或展示顺序时，可以只改 card；但不得引入 `sections` 中不存在或相互冲突的新语义。

## 质量门禁

`tests/mcp/test_knowledge_generation.py` 会检查生成后的 SPEC、docs/knowledge 和 MCP C 表是否保持最新，并确保不会低于当前质量基线：

- 非空内容行数
- 标题数量
- 顶层章节数量
- Xray 示例代码块数量
- EBNF 代码块数量
- Markdown 表格数量
- card 源数量和 JSON 可解析性
- topic `spec_anchor` 必须指向生成 SPEC 中存在的标题
- card `fences` 必须引用 `sections/*.md` 中存在的 `@id`
- `sections/*.md` 中每个 `@id` 示例都必须被至少一个 card 引用
- topic knowledge 中的 Xray 示例必须能通过 `xray check`
- stdlib symbol 必须是 analyzer builtin metadata 的子集
- MCP tool/resource 描述中的 topic 列表必须覆盖全部 topic card
- prompt smoke 示例必须能通过 `xray check`
