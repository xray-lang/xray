"""Render shared language definitions into the bilingual reference sections.

Generated regions are readable, checked-in projections. Only their referenced
inputs define their contents; changing a projection never changes the rule.
"""
from __future__ import annotations

import json
import re
from pathlib import Path

from check_spec_xray_snippets import lexical_facts

REGION = re.compile(
    r"^<!-- xr-source:(example|grammar|keywords) ([a-zA-Z0-9_.-]+) -->\n"
    r"(.*?)^<!-- /xr-source -->", re.M | re.S)
RULE = re.compile(r"^([A-Za-z][A-Za-z0-9_]*)\s*::=", re.M)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate source key: {key}")
        result[key] = value
    return result


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_object)


def lexical_parts(code):
    tokens, _, spans = lexical_facts(code, with_spans=True)
    gaps, previous = [], 0
    for start, end in spans:
        gaps.append(code[previous:start])
        previous = end
    gaps.append(code[previous:])
    return tokens, gaps


def localized_code(entry, lang):
    code = entry["code"]
    tokens, gaps = lexical_parts(code)
    for index, trivia in entry.get("trivia", {}).get(lang, {}).items():
        if not index.isdecimal() or not 0 <= int(index) <= len(tokens):
            raise ValueError(f"invalid translation position: {index}")
        if lexical_facts(trivia)[0]:
            raise ValueError("example translations may contain only whitespace and comments")
        at = int(index)
        if 0 < at < len(tokens) and (
                bool(gaps[at]) != bool(trivia) or ("\n" in gaps[at]) != ("\n" in trivia)):
            raise ValueError("example translation changes a token boundary or statement line ending")
        gaps[at] = trivia
    result = "".join(gap + token for gap, token in zip(gaps, tokens)) + gaps[-1]
    # A comment must not swallow a token, and whitespace must not join tokens.
    if lexical_facts(result)[0] != tokens:
        raise ValueError("example translation changes executable tokens")
    return result


def grammar_rules(text):
    result = {}
    matches = list(RULE.finditer(text))
    if not matches:
        raise ValueError("grammar has no productions")
    for index, match in enumerate(matches):
        name = match[1]
        if name in result:
            raise ValueError(f"duplicate grammar production: {name}")
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        result[name] = text[match.start():end].strip()
    return result


def reserved_keywords(root):
    """Use the same two declarations as the lexer, with original provenance."""
    sources = (
        ("src/frontend/lexer/xkeywords.def",
         r'XR_KW\(\s*"([^"\n]+)"\s*,\s*(\d+)\s*,\s*(TK_[A-Z_]+)\s*\)'),
        ("src/shared/xr_exact_scalar_registry.def",
         r'XR_EXACT_SCALAR\(\s*([A-Z0-9_]+),\s*\d+,\s*"([^"\n]+)"'),
    )
    rows = {}
    for relative, pattern in sources:
        text = (root / relative).read_text(encoding="utf-8")
        for match in re.finditer(pattern, text):
            if "xkeywords" in relative:
                name, length, token = match.groups()
                if len(name.encode("ascii")) != int(length):
                    raise ValueError(f"{relative}: invalid keyword length for {name}")
            else:
                kind, name = match.groups()
                token = "TK_" + kind
            if name in rows:
                raise ValueError(f"duplicate reserved keyword: {name}")
            rows[name] = dict(name=name, token=token, source=relative,
                              line=text.count("\n", 0, match.start()) + 1)
    if not rows:
        raise ValueError("empty reserved keyword source")
    return [rows[name] for name in sorted(rows)]


class Sources:
    def __init__(self, root):
        self.root = Path(root)
        self.source = self.root / "spec/source"
        self.examples = {}
        for path in sorted((self.source / "examples").glob("*.json")):
            for key, entry in read_json(path).items():
                if key in self.examples:
                    raise ValueError(f"duplicate example: {key}")
                self.examples[key] = entry
        self.rules = grammar_rules((self.source / "grammar/language.ebnf").read_text(encoding="utf-8"))
        self.views = read_json(self.source / "grammar/views.json")
        lexical = read_json(self.source / "lexical/keywords.json")
        self.keyword_notes = lexical["reserved"]
        self.contextual = lexical["contextual"]
        self.keywords = reserved_keywords(self.root)
        names = {row["name"] for row in self.keywords}
        if names != set(self.keyword_notes):
            raise ValueError(f"keyword descriptions differ from lexer declarations: {sorted(names ^ set(self.keyword_notes))}")
        if names & set(self.contextual):
            raise ValueError("a contextual word cannot also be globally reserved")
        self.used = {"example": set(), "grammar": set()}
        for path in sorted((self.source / "cards").rglob("*.json")):
            for section in read_json(path).get("sections", []):
                if section.get("code"):
                    raise ValueError(f"{path}: inline examples must reference shared sources")
                for key in section.get("source_examples", []):
                    if key not in self.examples:
                        raise ValueError(f"{path}: unknown shared example {key}")
                    self.used["example"].add(key)

    def render(self, kind, key, lang):
        if kind == "example":
            entry = self.examples[key]
            self.used[kind].add(key)
            result = "```" + entry["fence"] + "\n" + localized_code(entry, lang) + "\n```"
            if "output" in entry:
                result += "\n```output\n" + entry["output"] + "```"
            return result
        if kind == "grammar":
            self.used[kind].add(key)
            names = self.views[key]
            if not names or len(names) != len(set(names)):
                raise ValueError(f"empty or duplicate grammar view: {key}")
            return "```ebnf\n" + "\n".join(self.rules[name] for name in names) + "\n```"
        if kind == "keywords" and key == "count":
            return (f"当前词法实现保留 **{len(self.keywords)} 个关键字**。完整名单由词法声明生成，见附录 B；目标分类的变更不代表已实现。" if lang == "cn" else
                    f"The current lexer reserves **{len(self.keywords)} keywords**. Appendix B is generated from the lexical declarations; a target classification change does not imply implementation.")
        if kind == "keywords" and key == "index":
            head = ("下表由当前词法声明生成。它记录实际保留集，不证明相关功能已实现。上下文词见 §1.5.8；`out` 是普通标识符。\n\n| 关键字 | 节 |\n|--|--|" if lang == "cn" else
                    "This table is generated from the current lexical declarations. Reservation does not establish feature support. Contextual words are listed in §1.5.8; `out` is an ordinary identifier.\n\n| Keyword | Section |\n|--|--|")
            return head + "\n" + "\n".join(f"| `{row['name']}` | {self.keyword_notes[row['name']]['section']} |" for row in self.keywords)
        if kind == "keywords" and (key.startswith("group-") or key == "contextual"):
            rows = (self.contextual.items() if key == "contextual" else
                    ((name, entry) for name, entry in self.keyword_notes.items()
                     if entry["group"] == int(key.removeprefix("group-"))))
            header = "| 关键字 | 用途 |" if lang == "cn" else "| Keyword | Purpose |"
            return header + "\n|--|--|\n" + "\n".join(
                f"| `{name}` | {entry[lang]} |" for name, entry in rows)
        raise ValueError(f"unknown source projection: {kind} {key}")

    def expand(self, path):
        text = path.read_text(encoding="utf-8")
        def replace(match):
            prefix = text[:match.start()]
            cn, en = prefix.rfind("<!-- xr-spec:cn -->"), prefix.rfind("<!-- xr-spec:en -->")
            if cn == en == -1:
                raise ValueError(f"{path}: source region outside language block")
            lang = "cn" if cn > en else "en"
            if prefix.rfind(f"<!-- /xr-spec:{lang} -->") > max(cn, en):
                raise ValueError(f"{path}: source region outside language block")
            if f"<!-- /xr-spec:{lang} -->" in match[3]:
                raise ValueError(f"{path}: source region crosses language block boundary")
            try:
                body = self.render(match[1], match[2], lang)
            except KeyError as error:
                raise ValueError(f"{path}: missing {match[1]} source {error}") from error
            return f"<!-- xr-source:{match[1]} {match[2]} -->\n{body}\n<!-- /xr-source -->"
        expanded = REGION.sub(replace, text)
        remainder = REGION.sub("", expanded)
        if "<!-- xr-source:" in remainder or "<!-- /xr-source -->" in remainder:
            raise ValueError(f"{path}: malformed source projection")
        if re.search(r"^```(?:xray|ebnf)(?:\s|$)", remainder, re.M):
            raise ValueError(f"{path}: executable examples and grammar must use shared sources")
        return expanded

    def validate_usage(self):
        for kind, entries in (("example", self.examples), ("grammar", self.views)):
            unused = set(entries) - self.used[kind]
            if unused:
                raise ValueError(f"unreferenced {kind} sources: {sorted(unused)}")
        referenced = {name for names in self.views.values() for name in names}
        if set(self.rules) - referenced:
            raise ValueError(f"unreferenced grammar productions: {sorted(set(self.rules) - referenced)}")


def render_sections(root):
    sources = Sources(root)
    sections = {path: sources.expand(path) for path in sorted((sources.source / "sections").glob("*.md"))}
    sources.validate_usage()
    return sections
