"""Compile static declaration bindings and their ordinary argument contracts."""
from native_source_compile import compile_cases


if __name__ == "__main__":
    cases = {
        "utf8_lossy_tail": 'export fn probe(data: Array<u8>) -> string { return string.fromUtf8Lossy(data[:]) }',
        "utf8_checked_tail": 'export fn probe(data: Array<u8>) -> string { return string.fromUtf8(data[:]) }',
        "utf8_lossy_non_tail": 'export fn probe(data: Array<u8>) -> i64 { var text = string.fromUtf8Lossy(data[:]); return len(text) }',
        "nullable_factory": """class Token {
 value: string
 constructor(value: string) { this.value = value }
 static fromToken(value: string) -> Token? { return Token(value) }
}
export fn probe() -> bool { return Token.fromToken("GET") != null }
""",
        "scalar_arguments": """class Math {
 static combine(a: i64, b: i64) -> i64 { return a + b }
}
export fn probe() -> i64 { return Math.combine(19, 23) }
""",
        "distinct_classes": """class First {
 static value() -> i64 { return 19 }
}
class Second {
 static value() -> i64 { return 23 }
}
export fn probe() -> i64 { return First.value() + Second.value() }
""",
    }
    dependency = """export class ImportedToken {
 value: string
 constructor(value: string) { this.value = value }
 static make() -> ImportedToken { return ImportedToken("GET") }
 static fromToken(value: string) -> ImportedToken? { return ImportedToken(value) }
 static combine(a: i64, b: i64) -> i64 { return a + b }
}
"""
    cases["imported_factory"] = 'import { ImportedToken } from "./token"\nexport fn probe() -> i64 { var t = ImportedToken.make(); return len(t.value) }'
    cases["imported_nullable_factory"] = 'import { ImportedToken } from "./token"\nexport fn probe() -> bool { return ImportedToken.fromToken("GET") != null }'
    cases["imported_scalar_arguments"] = 'import { ImportedToken } from "./token"\nexport fn probe() -> i64 { return ImportedToken.combine(19, 23) }'
    peer = """export class ImportedPeer {
 value: string
 constructor(value: string) { this.value = value }
 static make() -> ImportedPeer { return ImportedPeer("peer") }
 static combine(a: i64, b: i64) -> i64 { return a - b }
}
"""
    cases["distinct_imported_owners"] = """import { ImportedToken } from "./token"
import { ImportedPeer } from "./peer"
export fn probe() -> i64 {
 var first = ImportedToken.make()
 var second = ImportedPeer.make()
 return ImportedToken.combine(len(first.value), 19) + ImportedPeer.combine(23, len(second.value))
}
"""
    cases["reversed_imported_owners"] = """import { ImportedPeer } from "./peer"
import { ImportedToken } from "./token"
export fn probe() -> i64 {
 var first = ImportedPeer.make()
 var second = ImportedToken.make()
 return ImportedPeer.combine(23, len(first.value)) + ImportedToken.combine(len(second.value), 19)
}
"""
    raise SystemExit(compile_cases(cases, {"token.xr": dependency, "peer.xr": peer}))
