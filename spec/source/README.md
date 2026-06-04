# 结构化语言规范源

本目录是语言参考手册和 MCP knowledge 投影文件的唯一手写源。

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
docs/spec/source/
  sections/*.md            # 中英文语言参考手册章节源
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
- 需要在 MCP knowledge 中出现的 Xray 示例，应优先写在 `sections/*.md` 的 `xray @id=...` 代码块中，再由 card 的 `fences` 引用。
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
