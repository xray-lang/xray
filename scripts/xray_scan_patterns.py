"""Regex filters with fast file selection from the same literal alternatives."""

from __future__ import annotations

from dataclasses import dataclass
import re


@dataclass(frozen=True)
class ScanPattern:
    regex: re.Pattern[str]
    literals: tuple[str, ...] = ()

    def may_match(self, text: str) -> bool:
        return not self.literals or any(literal in text for literal in self.literals)


def literal_alternatives(*branches: str | tuple[str, str, str]) -> ScanPattern:
    """An entry is a whole word, or a literal with a regex prefix and suffix."""
    literals = []
    patterns = []
    for branch in branches:
        literal, before, after = (branch, r"\b", r"\b") if isinstance(branch, str) else branch
        if not literal:
            raise ValueError("An optimized alternative needs a literal")
        literals.append(literal)
        patterns.append(before + re.escape(literal) + after)
    return ScanPattern(re.compile("|".join(patterns)), tuple(dict.fromkeys(literals)))
