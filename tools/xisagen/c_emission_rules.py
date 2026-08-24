"""Parser and independent renderers for typed C emission rules."""

from __future__ import annotations

import re
from dataclasses import dataclass


class RuleError(ValueError):
    pass


@dataclass(frozen=True)
class Atom:
    value: str
    line: int
    quoted: bool = False


@dataclass(frozen=True)
class Form:
    items: tuple[Atom | "Form", ...]
    line: int

    def keyword(self, name: str) -> Atom | Form | None:
        for index, item in enumerate(self.items):
            if isinstance(item, Atom) and item.value == name and index + 1 < len(self.items):
                return self.items[index + 1]
        return None


@dataclass(frozen=True)
class RuleDomain:
    name: str
    members: tuple[str, ...]


@dataclass(frozen=True)
class Rule:
    name: str
    stable_id: int
    domain: str
    member: str
    opcode: str
    intrinsic: str
    operand_count: int
    result_kind: str
    element_access: str
    reference_action: str
    reference_drop: str
    element_source_class: bool
    applies_element_source_class: bool
    applies_storage: str
    call_convention: str
    target_kind: str
    layout_kind: str
    storage: str
    receiver_ownership: str
    element_ownership: str
    receiver_storage: str
    element_storage: str
    caller_register_kind: str
    caller_memory_kind: str
    recipe: str
    recipe_rep: str
    recipe_symbol: str
    diagnostic: str
    coverage: str
    max_builder_lines: int
    max_verifier_lines: int


RULE_KEYWORDS = {
    ":stable-id", ":domain", ":member", ":opcode", ":intrinsic", ":operand-count",
    ":result-kind", ":element-access", ":reference-action", ":reference-drop",
    ":element-source-class", ":applies-element-source-class", ":applies-storage",
    ":call-convention", ":target-kind", ":layout-kind",
    ":storage", ":receiver-ownership", ":element-ownership", ":receiver-storage",
    ":element-storage", ":caller-register-kind", ":caller-memory-kind", ":recipe",
    ":recipe-rep", ":recipe-symbol", ":diagnostic", ":coverage",
    ":max-builder-lines", ":max-verifier-lines",
}

C_TOKEN = re.compile(r"[A-Z][A-Z0-9_]*\Z")
NAME = re.compile(r"[a-z][a-z0-9.-]*\Z")
COVERAGE = re.compile(r"[a-z][a-z0-9-]*\Z")


def _tokens(text: str, path: str) -> list[Atom]:
    tokens: list[Atom] = []
    index = 0
    line = 1
    while index < len(text):
        char = text[index]
        if char in " \t\r":
            index += 1
            continue
        if char == "\n":
            line += 1
            index += 1
            continue
        if char == ";":
            while index < len(text) and text[index] != "\n":
                index += 1
            continue
        if char in "()":
            tokens.append(Atom(char, line))
            index += 1
            continue
        if char == '"':
            start_line = line
            index += 1
            value: list[str] = []
            while index < len(text) and text[index] != '"':
                if text[index] == "\n":
                    raise RuleError(f"{path}:{start_line}: newline in string")
                if text[index] == "\\":
                    index += 1
                    if index >= len(text) or text[index] not in ('"', "\\"):
                        raise RuleError(f"{path}:{start_line}: invalid string escape")
                value.append(text[index])
                index += 1
            if index >= len(text):
                raise RuleError(f"{path}:{start_line}: unterminated string")
            tokens.append(Atom("".join(value), start_line, True))
            index += 1
            continue
        start = index
        while index < len(text) and text[index] not in "() \t\r\n;":
            index += 1
        value = text[start:index]
        if not value:
            raise RuleError(f"{path}:{line}: invalid token")
        tokens.append(Atom(value, line))
    return tokens


def _forms(text: str, path: str) -> list[Form]:
    tokens = _tokens(text, path)
    position = 0

    def parse_one() -> Atom | Form:
        nonlocal position
        if position >= len(tokens):
            raise RuleError(f"{path}: unexpected end of input")
        token = tokens[position]
        position += 1
        if token.value == ")":
            raise RuleError(f"{path}:{token.line}: unexpected ')'")
        if token.value != "(":
            return token
        items: list[Atom | Form] = []
        while position < len(tokens) and tokens[position].value != ")":
            items.append(parse_one())
        if position >= len(tokens):
            raise RuleError(f"{path}:{token.line}: unclosed '('")
        position += 1
        return Form(tuple(items), token.line)

    result: list[Form] = []
    while position < len(tokens):
        value = parse_one()
        if not isinstance(value, Form):
            raise RuleError(f"{path}:{value.line}: top-level atom is not allowed")
        result.append(value)
    return result


def _atom(form: Form, keyword: str, path: str) -> Atom:
    value = form.keyword(keyword)
    if not isinstance(value, Atom):
        raise RuleError(f"{path}:{form.line}: {keyword} requires one atom")
    return value


def _c_token(form: Form, keyword: str, path: str) -> str:
    value = _atom(form, keyword, path)
    if value.quoted or not C_TOKEN.fullmatch(value.value):
        raise RuleError(f"{path}:{value.line}: {keyword} must be a closed C enum token")
    return value.value


def _positive(form: Form, keyword: str, path: str) -> int:
    value = _atom(form, keyword, path)
    if value.quoted or not value.value.isdigit() or int(value.value) <= 0:
        raise RuleError(f"{path}:{value.line}: {keyword} must be a positive integer")
    return int(value.value)


def _boolean(form: Form, keyword: str, path: str) -> bool:
    value = _atom(form, keyword, path)
    if value.quoted or value.value not in ("yes", "no"):
        raise RuleError(f"{path}:{value.line}: {keyword} must be yes or no")
    return value.value == "yes"


def parse(text: str, path: str = "<input>") -> tuple[list[RuleDomain], list[Rule]]:
    domains: list[RuleDomain] = []
    rules: list[Rule] = []
    for form in _forms(text, path):
        if len(form.items) < 2 or not isinstance(form.items[0], Atom) or \
                not isinstance(form.items[1], Atom):
            raise RuleError(f"{path}:{form.line}: malformed rule form")
        head = form.items[0].value
        name = form.items[1]
        if name.quoted or not NAME.fullmatch(name.value):
            raise RuleError(f"{path}:{name.line}: invalid rule identity")
        if head == "define-c-emission-domain":
            members = form.keyword(":members")
            if not isinstance(members, Form) or not members.items:
                raise RuleError(f"{path}:{form.line}: domain requires non-empty :members")
            values: list[str] = []
            for member in members.items:
                if not isinstance(member, Atom) or member.quoted or \
                        not C_TOKEN.fullmatch(member.value):
                    raise RuleError(f"{path}:{getattr(member, 'line', form.line)}: invalid member")
                if member.value in values:
                    raise RuleError(f"{path}:{member.line}: duplicate domain member")
                values.append(member.value)
            domains.append(RuleDomain(name.value, tuple(values)))
            continue
        if head != "define-c-emission-rule":
            raise RuleError(f"{path}:{form.line}: unknown form {head}")
        for item in form.items[2:]:
            if isinstance(item, Atom) and item.value.startswith(":") and \
                    item.value not in RULE_KEYWORDS:
                raise RuleError(f"{path}:{item.line}: unknown rule keyword {item.value}")
        symbol = _atom(form, ":recipe-symbol", path)
        diagnostic = _atom(form, ":diagnostic", path)
        coverage = _atom(form, ":coverage", path)
        if not symbol.quoted or not symbol.value:
            raise RuleError(f"{path}:{symbol.line}: recipe symbol must be a string")
        if not diagnostic.quoted or ":" not in diagnostic.value:
            raise RuleError(f"{path}:{diagnostic.line}: diagnostic requires code and detail")
        if coverage.quoted or not COVERAGE.fullmatch(coverage.value):
            raise RuleError(f"{path}:{coverage.line}: invalid coverage key")
        domain = _atom(form, ":domain", path)
        if domain.quoted or not NAME.fullmatch(domain.value):
            raise RuleError(f"{path}:{domain.line}: invalid domain identity")
        rules.append(Rule(
            name=name.value,
            stable_id=_positive(form, ":stable-id", path),
            domain=domain.value,
            member=_c_token(form, ":member", path),
            opcode=_c_token(form, ":opcode", path),
            intrinsic=_c_token(form, ":intrinsic", path),
            operand_count=_positive(form, ":operand-count", path),
            result_kind=_c_token(form, ":result-kind", path),
            element_access=_c_token(form, ":element-access", path),
            reference_action=_c_token(form, ":reference-action", path),
            reference_drop=_c_token(form, ":reference-drop", path),
            element_source_class=_boolean(form, ":element-source-class", path),
            applies_element_source_class=_boolean(
                form, ":applies-element-source-class", path),
            applies_storage=_c_token(form, ":applies-storage", path),
            call_convention=_c_token(form, ":call-convention", path),
            target_kind=_c_token(form, ":target-kind", path),
            layout_kind=_c_token(form, ":layout-kind", path),
            storage=_c_token(form, ":storage", path),
            receiver_ownership=_c_token(form, ":receiver-ownership", path),
            element_ownership=_c_token(form, ":element-ownership", path),
            receiver_storage=_c_token(form, ":receiver-storage", path),
            element_storage=_c_token(form, ":element-storage", path),
            caller_register_kind=_c_token(form, ":caller-register-kind", path),
            caller_memory_kind=_c_token(form, ":caller-memory-kind", path),
            recipe=_c_token(form, ":recipe", path),
            recipe_rep=_c_token(form, ":recipe-rep", path),
            recipe_symbol=symbol.value,
            diagnostic=diagnostic.value,
            coverage=coverage.value,
            max_builder_lines=_positive(form, ":max-builder-lines", path),
            max_verifier_lines=_positive(form, ":max-verifier-lines", path),
        ))
    if not domains or not rules:
        raise RuleError(f"{path}: at least one domain and rule are required")
    domain_map = {domain.name: domain for domain in domains}
    if len(domain_map) != len(domains):
        raise RuleError(f"{path}: duplicate domain identity")
    stable_ids: set[int] = set()
    identities: set[str] = set()
    coverage_keys: set[str] = set()
    covered = {domain.name: set() for domain in domains}
    for rule in rules:
        domain = domain_map.get(rule.domain)
        if not domain:
            raise RuleError(f"{path}: unknown domain {rule.domain}")
        if rule.member not in domain.members:
            raise RuleError(f"{path}: member {rule.member} is outside {rule.domain}")
        if rule.member in covered[rule.domain]:
            raise RuleError(f"{path}: overlapping rule for {rule.member}")
        if rule.stable_id in stable_ids or rule.name in identities:
            raise RuleError(f"{path}: duplicate rule identity {rule.name}")
        if rule.coverage in coverage_keys:
            raise RuleError(f"{path}: duplicate coverage key {rule.coverage}")
        covered[rule.domain].add(rule.member)
        stable_ids.add(rule.stable_id)
        identities.add(rule.name)
        coverage_keys.add(rule.coverage)
    for domain in domains:
        missing = set(domain.members) - covered[domain.name]
        if missing:
            raise RuleError(f"{path}: non-exhaustive domain {domain.name}: {sorted(missing)}")
    return domains, sorted(rules, key=lambda rule: rule.stable_id)


def _ident(rule: Rule) -> str:
    return re.sub(r"[^A-Za-z0-9]+", "_", rule.name).strip("_").upper()


def _clauses(rule: Rule) -> list[str]:
    source_class = "true" if rule.element_source_class else "false"
    return [
        f"facts->opcode == {rule.opcode}",
        f"facts->intrinsic == {rule.intrinsic}",
        f"facts->operand_count == {rule.operand_count}",
        f"facts->result_kind == {rule.result_kind}",
        f"facts->element_access == {rule.element_access}",
        f"facts->reference_action == {rule.reference_action}",
        f"facts->reference_drop == {rule.reference_drop}",
        f"facts->element_source_class == {source_class}",
        f"facts->call_convention == {rule.call_convention}",
        f"facts->target_kind == {rule.target_kind}",
        f"facts->layout_kind == {rule.layout_kind}",
        f"facts->call_storage == {rule.storage}",
        f"facts->layout_storage == {rule.storage}",
        f"facts->argument_ownership[0] == {rule.receiver_ownership}",
        f"facts->argument_ownership[1] == {rule.element_ownership}",
        f"facts->argument_storage[0] == {rule.receiver_storage}",
        f"facts->argument_storage[1] == {rule.element_storage}",
        f"facts->caller_register_kind[0] == {rule.caller_register_kind}",
        f"facts->caller_register_kind[1] == {rule.caller_register_kind}",
        f"facts->caller_memory_kind[0] == {rule.caller_memory_kind}",
        f"facts->caller_memory_kind[1] == {rule.caller_memory_kind}",
        "facts->operation_result_bound",
        "facts->call_result_bound",
        "facts->arguments_structurally_exact",
    ]


def _applicability_clauses(rule: Rule) -> list[str]:
    source_class = "true" if rule.applies_element_source_class else "false"
    return [
        f"facts->element_source_class == {source_class}",
        f"facts->call_storage == {rule.applies_storage}",
        f"facts->layout_storage == {rule.applies_storage}",
        f"facts->argument_storage[0] == {rule.applies_storage}",
        f"facts->argument_storage[1] == {rule.applies_storage}",
    ]


def render_ids(rules: list[Rule]) -> str:
    lines = [
        "/* AUTO-GENERATED by xisagen - DO NOT EDIT */",
        "/* Source: xisa/aot/c_emission_rules.def */",
        "#ifndef XR_C_EMISSION_RULE_IDS_GEN_H",
        "#define XR_C_EMISSION_RULE_IDS_GEN_H",
        "",
        "typedef enum XrCEmissionRuleId {",
        "    XR_C_EMISSION_RULE_NONE = 0,",
    ]
    lines.extend(f"    XR_C_EMISSION_RULE_{_ident(rule)} = {rule.stable_id}," for rule in rules)
    lines.extend(["} XrCEmissionRuleId;", "", "#endif  // XR_C_EMISSION_RULE_IDS_GEN_H", ""])
    return "\n".join(lines)


def render_builder(rules: list[Rule]) -> str:
    lines = [
        "/* AUTO-GENERATED by xisagen - DO NOT EDIT */",
        "/* Source: xisa/aot/c_emission_rules.def */",
        "",
        "static inline XrCEmissionRuleMatch xr_c_emission_rule_build(",
        "    const XrCEmissionRuleFacts *facts, XrCEmissionRuleDecision *out) {",
        "    if (!facts || !out)",
        "        return XR_C_EMISSION_RULE_MALFORMED;",
        "    switch (facts->member) {",
    ]
    for rule in rules:
        block = [f"        case {rule.member}:", "            if (!("]
        applicability = _applicability_clauses(rule)
        for index, clause in enumerate(applicability):
            block.append(
                f"                {clause}{' ||' if index + 1 < len(applicability) else '))'}")
        block.extend([
            "                return XR_C_EMISSION_RULE_NOT_APPLICABLE;",
            "            if (!("
        ])
        clauses = _clauses(rule)
        for index, clause in enumerate(clauses):
            block.append(f"                {clause}{' &&' if index + 1 < len(clauses) else '))'}")
        block.extend([
            "                return XR_C_EMISSION_RULE_MALFORMED;",
            "            *out = (XrCEmissionRuleDecision) {",
            f"                XR_C_EMISSION_RULE_{_ident(rule)}, {rule.recipe}, {rule.recipe_rep},",
            f"                {rule.storage}, \"{rule.recipe_symbol}\"}};",
            "            return XR_C_EMISSION_RULE_EXACT;",
        ])
        if len(block) > rule.max_builder_lines:
            raise RuleError(f"builder for {rule.name} exceeds {rule.max_builder_lines} lines")
        lines.extend(block)
    lines.extend([
        "        default:",
        "            return XR_C_EMISSION_RULE_NOT_APPLICABLE;",
        "    }",
        "}",
        "",
    ])
    return "\n".join(lines)


def render_verifier(rules: list[Rule]) -> str:
    lines = [
        "/* AUTO-GENERATED by xisagen - DO NOT EDIT */",
        "/* Source: xisa/aot/c_emission_rules.def */",
        "",
        "static inline XrCEmissionRuleMatch xr_c_emission_rule_verify(",
        "    const XrCEmissionRuleFacts *facts, const XrCEmissionRuleDecision *actual,",
        "    const char **diagnostic) {",
        "    if (diagnostic)",
        "        *diagnostic = \"XR_EXEC_5003:invalid C emission rule input\";",
        "    if (!facts || !actual)",
        "        return XR_C_EMISSION_RULE_MALFORMED;",
        "    switch (facts->member) {",
    ]
    for rule in rules:
        block = [f"        case {rule.member}: {{", "            bool applicable ="]
        applicability = list(reversed(_applicability_clauses(rule)))
        for index, clause in enumerate(applicability):
            block.append(
                f"                {clause}{' ||' if index + 1 < len(applicability) else ';'}")
        block.extend([
            "            if (!applicable)",
            "                return XR_C_EMISSION_RULE_NOT_APPLICABLE;",
            "            bool clauses ="
        ])
        clauses = list(reversed(_clauses(rule)))
        for index, clause in enumerate(clauses):
            block.append(f"                {clause}{' &&' if index + 1 < len(clauses) else ';'}")
        block.extend([
            "            bool decision =",
            f"                actual->rule_id == XR_C_EMISSION_RULE_{_ident(rule)} &&",
            f"                actual->recipe == {rule.recipe} && actual->rep == {rule.recipe_rep} &&",
            f"                actual->storage == {rule.storage} && actual->symbol &&",
            f"                strcmp(actual->symbol, \"{rule.recipe_symbol}\") == 0;",
            "            if (!clauses || !decision) {",
            f"                if (diagnostic) *diagnostic = \"{rule.diagnostic}\";",
            "                return XR_C_EMISSION_RULE_MALFORMED;",
            "            }",
            "            return XR_C_EMISSION_RULE_EXACT;",
            "        }",
        ])
        if len(block) > rule.max_verifier_lines:
            raise RuleError(f"verifier for {rule.name} exceeds {rule.max_verifier_lines} lines")
        lines.extend(block)
    lines.extend([
        "        default:",
        "            return XR_C_EMISSION_RULE_NOT_APPLICABLE;",
        "    }",
        "}",
        "",
    ])
    return "\n".join(lines)


def generate(text: str, path: str = "<input>") -> dict[str, str]:
    _, rules = parse(text, path)
    builder = render_builder(rules)
    verifier = render_verifier(rules)
    if "xr_c_emission_rule_build(" in verifier or "xr_c_emission_rule_verify(" in builder:
        raise RuleError("builder and verifier renderers are not independent")
    return {
        "src/aot/emit_c/xr_c_emission_rule_ids_gen.h": render_ids(rules),
        "src/aot/emit_c/xr_c_emission_rule_build_gen.inc.c": builder,
        "src/aot/emit_c/xr_c_emission_rule_verify_gen.inc.c": verifier,
    }


def self_test() -> None:
    source = """
    (define-c-emission-domain mutation :members (XI_METHOD_SYMBOL_PUSH))
    (define-c-emission-rule c-emission.array-push.tagged.v1
      :stable-id 1 :domain mutation :member XI_METHOD_SYMBOL_PUSH
      :opcode XI_CALL_METHOD :intrinsic XR_SEM_INTRINSIC_ARRAY_MEMBER_SCALAR
      :operand-count 2 :result-kind XR_KIND_UNIT
      :element-access XR_ARRAY_MEMBER_ELEMENT_ACCESS_STORE
      :reference-action XR_ARRAY_MEMBER_REFERENCE_CONSUME_INTO_STORAGE
      :reference-drop XR_ARRAY_MEMBER_REFERENCE_DROP_RELEASE_ON_ERASE_OR_DESTROY
      :element-source-class yes
      :applies-element-source-class yes
      :applies-storage XR_TARGET_ARRAY_STORAGE_TAGGED
      :call-convention XR_TARGET_CALL_CONVENTION_ARRAY_MEMBER_SCALAR
      :target-kind XR_TARGET_CALL_TARGET_ARRAY_MEMBER_SCALAR
      :layout-kind XR_TARGET_LAYOUT_DYNAMIC :storage XR_TARGET_ARRAY_STORAGE_TAGGED
      :receiver-ownership XR_TARGET_CALL_BORROW :element-ownership XR_TARGET_CALL_CONSUME
      :receiver-storage XR_TARGET_ARRAY_STORAGE_NONE
      :element-storage XR_TARGET_ARRAY_STORAGE_TAGGED
      :caller-register-kind XR_MACHINE_REP_DYN_VALUE
      :caller-memory-kind XR_MACHINE_REP_DYN_VALUE
      :recipe XR_C_VALUE_MATERIALIZATION_ARRAY_PUSH_TAGGED
      :recipe-rep XR_C_VALUE_REP_VOID :recipe-symbol "xrt_array_push"
      :diagnostic "XR_EXEC_5003:test" :coverage source-class-array-push
      :max-builder-lines 56 :max-verifier-lines 72)
    """
    outputs = generate(source)
    assert len(outputs) == 3
    assert "XR_C_EMISSION_RULE_EXACT" in outputs[
        "src/aot/emit_c/xr_c_emission_rule_build_gen.inc.c"]
    assert "xr_c_emission_rule_build(" not in outputs[
        "src/aot/emit_c/xr_c_emission_rule_verify_gen.inc.c"]
    mutations = (
        source.replace(":members (XI_METHOD_SYMBOL_PUSH)",
                       ":members (XI_METHOD_SYMBOL_PUSH XI_METHOD_SYMBOL_POP)"),
        source + source[source.index("(define-c-emission-rule"):],
        source.replace(":element-source-class yes", ":element-source-class maybe"),
        source.replace(":applies-storage XR_TARGET_ARRAY_STORAGE_TAGGED",
                       ":applies-storage tagged"),
        source.replace(":recipe XR_C_VALUE_MATERIALIZATION_ARRAY_PUSH_TAGGED",
                       ":recipe arbitrary-c-expression"),
        source.replace(":coverage source-class-array-push", ":coverage \"not-a-key\""),
    )
    for mutation in mutations:
        try:
            parse(mutation)
        except RuleError:
            continue
        raise AssertionError("invalid C emission rule mutation was accepted")
