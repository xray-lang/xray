/* Preserved independent packets and digest fragments, all rejected by the current reader. */
#ifndef STORAGE76_PREVIOUS_VECTORS_H
#define STORAGE76_PREVIOUS_VECTORS_H
#include "xir_assert_panics75_golden.h"
#include "xir_checked_range75_golden.h"
#include "xir_checked_scalar75_golden.h"
#include "xir_error_marker75_golden.h"
#include "xir_generic_method75_golden.h"
#include "xir_rune75_golden.h"
static const struct { const uint8_t *bytes; size_t length; } storage76_previous[] = {
    {assert_panics75_golden,sizeof(assert_panics75_golden)},
    {checked_range75_golden,sizeof(checked_range75_golden)},
    {checked_scalar75_golden,sizeof(checked_scalar75_golden)},
    {checked_scalar75_digest,sizeof(checked_scalar75_digest)},
    {error_marker75_golden,sizeof(error_marker75_golden)},
    {error_marker73_previous,sizeof(error_marker73_previous)},
    {generic_method75_golden,sizeof(generic_method75_golden)},
    {generic_method75_invalid_flags0,sizeof(generic_method75_invalid_flags0)},
    {generic_method75_invalid_flags1,sizeof(generic_method75_invalid_flags1)},
    {rune75_golden,sizeof(rune75_golden)},
};
#endif // STORAGE76_PREVIOUS_VECTORS_H
