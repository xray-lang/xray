"""Compile Buffer storage reads and imported class lifecycle callbacks."""
from native_source_compile import compile_cases

if __name__ == "__main__":
    raise SystemExit(compile_cases({
        "buffer_construction": """import { Buffer } from mem
export fn probe() -> i64 {
 var plain = Buffer(2)
 var zeroed = Buffer(3, true)
 var aligned = Buffer(5, false, 8)
 return len(plain) + len(zeroed) + len(aligned)
}
""",
        "buffer_pointer": """import { Buffer } from mem
export fn probe() -> i64 {
 var buffer = Buffer(2)
 var pointer = buffer.borrowPtr()
 return len(buffer)
}
""",
    }))
