/* Independently named Source entries and canonical scalar payloads. */
#ifndef XIR_TASK_SCALAR_ORACLES_H
#define XIR_TASK_SCALAR_ORACLES_H
typedef struct TaskScalarOracle { const char *name; XrXirType type; int64_t payload; } TaskScalarOracle;
static const TaskScalarOracle task_scalar_oracles[] = {
    {"i8", XR_XIR_I8, -128}, {"i16", XR_XIR_I16, -32768}, {"i32", XR_XIR_I32, INT64_C(-2147483648)},
    {"u8", XR_XIR_U8, 255}, {"u16", XR_XIR_U16, 65535}, {"u32", XR_XIR_U32, INT64_C(4294967295)},
    {"u64", XR_XIR_U64, -1}, {"f32", XR_XIR_F32, INT64_C(0x3fc00000)},
    {"f64", XR_XIR_F64, INT64_C(0x3ff8000000000000)}, {"rune", XR_XIR_RUNE, 120},
    {"i64", XR_XIR_I64, -42}, {"bool", XR_XIR_BOOL, 1}, {"string", XR_XIR_STRING, 0}, {"unit", XR_XIR_UNIT, 0}
};
enum { TASK_SCALAR_CASES = sizeof(task_scalar_oracles) / sizeof(task_scalar_oracles[0]) };
#endif // XIR_TASK_SCALAR_ORACLES_H
