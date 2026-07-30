#include "shared/xr_numeric_conversion_core.h"

uint64_t xr_freestanding_numeric_i64_roundtrip(uint64_t bits) {
    return xr_numeric_i64_to_bits(xr_numeric_i64_from_bits(bits));
}

uint64_t xr_freestanding_numeric_f64_roundtrip(uint64_t bits) {
    return xr_numeric_double_to_bits(xr_numeric_double_from_bits(bits));
}

double xr_freestanding_numeric_f32_from_bits(uint32_t bits) {
    return (double) xr_numeric_float_from_bits(bits);
}

double xr_freestanding_numeric_f64_to_f32(double value) {
    return xr_numeric_f64_to_f32(value);
}

int64_t xr_freestanding_numeric_convert(int64_t value, uint8_t source_rep, uint8_t target_rep,
                                        uint8_t pointer_bits) {
    return xr_numeric_int_convert_i64(value, source_rep, target_rep, pointer_bits);
}
