

set(XR_DECIMAL_PARSE_WORK "${CMAKE_CURRENT_BINARY_DIR}/decimal-parse-work")
file(MAKE_DIRECTORY "${XR_DECIMAL_PARSE_WORK}")

# Every work prefix remains covered; interleaving distributes the quadratic
# retry cost across bounded sanitizer tests without changing runtime budgets.
set(XR_DECIMAL_GENERATED_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_decimal_parse.c")
set(XR_DECIMAL_CHECKED "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_decimal_parse.xrc")
