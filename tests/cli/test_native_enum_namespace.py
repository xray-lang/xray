"""Verify declaration namespaces and enum values keep distinct physical carriers."""
from native_source_compile import compile_cases

if __name__ == "__main__":
    cases = {
        "number_parse_error_members": 'export fn probe() -> bool { return NumberParseError.InvalidSyntax != NumberParseError.OutOfRange }',
        "send_result_members": 'export fn probe() -> bool { return SendResult.Sent != SendResult.Closed }',
        "builtin_error": 'export fn probe() -> i64 { throw CryptoError.InvalidLength }',
        "builtin_compare": 'export fn probe() -> bool { return CryptoError.InvalidLength == CryptoError.InvalidLength }',
        "source_unit": 'enum ProbeError { Bad }\nexport fn probe() -> i64 { throw ProbeError.Bad }',
        "source_ordering_members": 'enum UserOrdering { Relaxed, SeqCst }\nexport fn probe() -> bool { return UserOrdering.Relaxed != UserOrdering.SeqCst }',
    }
    raise SystemExit(compile_cases(cases))
