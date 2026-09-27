"""Reject provider-specific enums and ambiguous hosted runtime bridge includes."""
import re

from native_source_compile import compile_cases


def check_generated(name, code):
    if name != "coroutine_prelude_namespace":
        return
    # Inspect the source worker, excluding unrelated runtime helper definitions.
    definitions = list(re.finditer(
        r"\b[A-Za-z_]\w*_probe_\d+_aot_resume"
        r"\(void \*raw_frame, const XrAotContext \*ctx\) \{", code))
    if len(definitions) != 1:
        raise RuntimeError("coroutine namespace fixture lacks one exact worker resume body")
    start = definitions[0].start()
    end = code.find("\nstatic ", definitions[0].end())
    if end < 0:
        raise RuntimeError("coroutine namespace fixture lacks a bounded worker definition")
    body = code[start:end]
    for fragment in ("case 1:", "xr_aot_get_builtin(ctx, 25)"):
        if fragment not in body:
            raise RuntimeError("coroutine worker did not emit " + fragment)
    if "xrt_map_new(" in body:
        raise RuntimeError("coroutine prelude lookup became an allocating local namespace")


if __name__ == "__main__":
    raise SystemExit(compile_cases({
        "prelude_typed_catch": """export fn probe(bytes: Array<u8>) -> string? {
    var result: string? = null
    try { result = string.fromUtf8(bytes[:]) }
    catch (e: Utf8Error) { result = null }
    return result
}""",
        "atomic_i64_bridge": """export fn probe(value: Atomic<i64>) -> i64 {
    value.store(7, Ordering.Release)
    return value.fetchAdd(2, Ordering.AcquireRelease) + value.load(Ordering.Acquire)
}""",
        "coroutine_prelude_namespace": """export fn probe(flag: bool) -> bool {
    Coro.yield()
    var result = flag ? SendResult.Sent : SendResult.Closed
    return result == SendResult.Sent
}""",
    }, runtime_headers_first=True, reject_statement_expressions=True,
       generated_check=check_generated))
