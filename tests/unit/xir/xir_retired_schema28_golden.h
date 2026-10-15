/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_retired_schema28_golden.h - Independent complete construction packet fields
 *
 * KEY CONCEPT:
 *   Literal bytes are derived from fixed field models without a compiler writer.
 */
#ifndef XIR_RETIRED_SCHEMA28_GOLDEN_H
#define XIR_RETIRED_SCHEMA28_GOLDEN_H
#include <stdint.h>
#include "xir_retired_schema28_golden_part1.h"
#include "xir_retired_schema28_golden_part2.h"
#include <stddef.h>
typedef struct RetiredSchema28Vector { const uint8_t *bytes; size_t length; } RetiredSchema28Vector;
static const RetiredSchema28Vector retired_schema28_vectors[] = {
    {retired_schema28_sizing75_golden, sizeof(retired_schema28_sizing75_golden)},
    {retired_schema28_sizing75_embedded_golden, sizeof(retired_schema28_sizing75_embedded_golden)},
    {retired_schema28_sizing75_empty_golden, sizeof(retired_schema28_sizing75_empty_golden)},
    {retired_schema28_root_construction75_checked, sizeof(retired_schema28_root_construction75_checked)},
    {retired_schema28_root_construction75_instance, sizeof(retired_schema28_root_construction75_instance)},
    {retired_schema28_assert_panics75_golden, sizeof(retired_schema28_assert_panics75_golden)},
    {retired_schema28_checked_range75_golden, sizeof(retired_schema28_checked_range75_golden)},
    {retired_schema28_checked_scalar75_golden, sizeof(retired_schema28_checked_scalar75_golden)},
    {retired_schema28_error_marker75_golden, sizeof(retired_schema28_error_marker75_golden)},
    {retired_schema28_generic_method75_golden, sizeof(retired_schema28_generic_method75_golden)},
    {retired_schema28_generic_method75_invalid_flags0, sizeof(retired_schema28_generic_method75_invalid_flags0)},
    {retired_schema28_generic_method75_invalid_flags1, sizeof(retired_schema28_generic_method75_invalid_flags1)},
    {retired_schema28_hash75_golden, sizeof(retired_schema28_hash75_golden)},
    {retired_schema28_nested_nullable75_golden, sizeof(retired_schema28_nested_nullable75_golden)},
    {retired_schema28_nullable_none75_golden, sizeof(retired_schema28_nullable_none75_golden)},
    {retired_schema28_nullable_some75_golden, sizeof(retired_schema28_nullable_some75_golden)},
    {retired_schema28_rune75_golden, sizeof(retired_schema28_rune75_golden)},
};
#endif // XIR_RETIRED_SCHEMA28_GOLDEN_H
