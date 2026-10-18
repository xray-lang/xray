/* Full historical wire controls from both fixed merge parents. */
#include "retired_merge_packets/family_0.h"
#include "retired_merge_packets/family_1.h"
#include "retired_merge_packets/family_2.h"
#include "retired_merge_packets/family_3.h"
#include "retired_merge_packets/family_4.h"
#include "retired_merge_packets/family_5.h"
#include "retired_merge_packets/family_6.h"
static const struct { const uint8_t *bytes; size_t length; } retired_merge_packets[] = {
    {fixed_sizing74_golden,sizeof(fixed_sizing74_golden)},
    {fixed_sizing74_embedded_golden,sizeof(fixed_sizing74_embedded_golden)},
    {fixed_sizing74_empty_golden,sizeof(fixed_sizing74_empty_golden)},
    {p16_sizing74_golden,sizeof(p16_sizing74_golden)},
    {p16_sizing74_embedded_golden,sizeof(p16_sizing74_embedded_golden)},
    {p16_sizing74_empty_golden,sizeof(p16_sizing74_empty_golden)},
    {fixed_assert_panics74_golden,sizeof(fixed_assert_panics74_golden)},
    {p16_assert_panics74_golden,sizeof(p16_assert_panics74_golden)},
    {fixed_checked_range74_golden,sizeof(fixed_checked_range74_golden)},
    {p16_checked_range74_golden,sizeof(p16_checked_range74_golden)},
    {fixed_checked_scalar74_golden,sizeof(fixed_checked_scalar74_golden)},
    {p16_checked_scalar74_golden,sizeof(p16_checked_scalar74_golden)},
    {fixed_error_marker74_golden,sizeof(fixed_error_marker74_golden)},
    {fixed_error_marker73_previous,sizeof(fixed_error_marker73_previous)},
    {p16_error_marker74_golden,sizeof(p16_error_marker74_golden)},
    {p16_error_marker73_previous,sizeof(p16_error_marker73_previous)},
    {fixed_generic_method74_golden,sizeof(fixed_generic_method74_golden)},
    {fixed_generic_method74_invalid_flags0,sizeof(fixed_generic_method74_invalid_flags0)},
    {fixed_generic_method74_invalid_flags1,sizeof(fixed_generic_method74_invalid_flags1)},
    {p16_generic_method74_golden,sizeof(p16_generic_method74_golden)},
    {p16_generic_method74_invalid_flags0,sizeof(p16_generic_method74_invalid_flags0)},
    {p16_generic_method74_invalid_flags1,sizeof(p16_generic_method74_invalid_flags1)},
    {fixed_rune74_golden,sizeof(fixed_rune74_golden)},
    {p16_rune74_golden,sizeof(p16_rune74_golden)},
};
