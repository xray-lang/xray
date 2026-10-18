/* Complete current fields retain every original acceptance and rejection role. */
#ifndef P09_HASH_CURRENT84_PACKET_TABLE_H
#define P09_HASH_CURRENT84_PACKET_TABLE_H
#include "compile_owner/retired_sizing_inputs.inc.c"
#include "composed_wire/packet_041.inc.c"
#include "composed_wire/packet_042.inc.c"
#include "composed_wire/packet_043.inc.c"
#include "composed_wire/packet_044.inc.c"
#include "composed_wire/packet_045.inc.c"
#include "composed_wire/packet_046.inc.c"
#include "composed_wire/packet_047.inc.c"
#include "composed_wire/packet_048.inc.c"
#include "composed_wire/packet_049.inc.c"
#include "composed_wire/packet_050.inc.c"
#include "composed_wire/packet_051.inc.c"
#include "composed_wire/packet_052.inc.c"
#include "composed_wire/packet_053.inc.c"
#include "composed_wire/packet_054.inc.c"
#include "composed_wire/packet_055.inc.c"
#include "composed_wire/packet_056.inc.c"
#include "composed_wire/packet_057.inc.c"
#include "composed_wire/packet_058.inc.c"
#include "composed_wire/packet_059.inc.c"
#include "composed_wire/packet_060.inc.c"
#include "composed_wire/packet_061.inc.c"
#include "composed_wire/packet_062.inc.c"
#include "composed_wire/packet_063.inc.c"
#include "composed_wire/packet_064.inc.c"
#include "composed_wire/packet_065.inc.c"
#include "composed_wire/packet_066.inc.c"
#include "composed_wire/packet_067.inc.c"
#include "composed_wire/packet_068.inc.c"
#include "composed_wire/packet_069.inc.c"
#include "composed_wire/packet_070.inc.c"
#include "composed_wire/packet_071.inc.c"
#include "composed_wire/packet_072.inc.c"
#include "composed_wire/packet_073.inc.c"
#include "composed_wire/packet_074.inc.c"
#include "composed_wire/packet_075.inc.c"
#include "composed_wire/packet_086.inc.c"
#include "composed_wire/packet_087.inc.c"
#include "composed_wire/packet_088.inc.c"
#include "composed_wire/packet_089.inc.c"
#include "composed_wire/packet_090.inc.c"
#include "composed_wire/packet_091.inc.c"
#include "composed_wire/packet_092.inc.c"
#include "composed_wire/packet_093.inc.c"
#include "composed_wire/packet_094.inc.c"
#include "composed_wire/packet_095.inc.c"
#include "composed_wire/packet_096.inc.c"
#include "composed_wire/packet_097.inc.c"
#include "composed_wire/packet_098.inc.c"
#include "composed_wire/packet_099.inc.c"
#include "composed_wire/packet_100.inc.c"
#include "composed_wire/packet_101.inc.c"
#include "composed_wire/packet_102.inc.c"
#include "composed_wire/packet_103.inc.c"
#include "composed_wire/packet_104.inc.c"
#include "composed_wire/packet_105.inc.c"
#include "composed_wire/packet_106.inc.c"
#include "composed_wire/packet_107.inc.c"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 34u && XR_XIR_CHECKED_CONTRACT == 90u,
    "complete current Hash packet field identity");
typedef struct CurrentHashPacket { const char *name; const uint8_t *bytes; size_t length; XrXirStatus expected; } CurrentHashPacket;
static const CurrentHashPacket current_hash84_packets[]={
    {"sizing74_retired_golden",historical84_retired_sizing74_golden,sizeof(historical84_retired_sizing74_golden),XR_XIR_BAD_STRUCTURE},
    {"sizing84_golden",packet_041_current,sizeof(packet_041_current),XR_XIR_OK},
    {"sizing74_retired_embedded_golden",historical84_retired_sizing74_embedded_golden,sizeof(historical84_retired_sizing74_embedded_golden),XR_XIR_BAD_STRUCTURE},
    {"sizing84_embedded_golden",packet_042_current,sizeof(packet_042_current),XR_XIR_BAD_STRUCTURE},
    {"sizing74_retired_empty_golden",historical84_retired_sizing74_empty_golden,sizeof(historical84_retired_sizing74_empty_golden),XR_XIR_BAD_STRUCTURE},
    {"sizing84_empty_golden",packet_043_current,sizeof(packet_043_current),XR_XIR_BAD_STRUCTURE},
    {"root_construction84_checked",packet_044_current,sizeof(packet_044_current),XR_XIR_OK},
    {"root_construction84_instance",packet_045_current,sizeof(packet_045_current),XR_XIR_OK},
    {"assert_panics84_golden",packet_046_current,sizeof(packet_046_current),XR_XIR_OK},
    {"checked_range84_golden",packet_047_current,sizeof(packet_047_current),XR_XIR_OK},
    {"checked_scalar84_golden",packet_048_current,sizeof(packet_048_current),XR_XIR_OK},
    {"error_marker84_golden",packet_049_current,sizeof(packet_049_current),XR_XIR_OK},
    {"generic_method84_golden",packet_050_current,sizeof(packet_050_current),XR_XIR_OK},
    {"generic_method84_invalid_flags0",packet_051_current,sizeof(packet_051_current),XR_XIR_BAD_TYPE},
    {"generic_method84_invalid_flags1",packet_052_current,sizeof(packet_052_current),XR_XIR_BAD_TYPE},
    {"hash84_golden",packet_053_current,sizeof(packet_053_current),XR_XIR_OK},
    {"hash_members84_golden",packet_054_current,sizeof(packet_054_current),XR_XIR_OK},
    {"hash_position84_map_key",packet_055_current,sizeof(packet_055_current),XR_XIR_OK},
    {"hash_position84_map_value",packet_056_current,sizeof(packet_056_current),XR_XIR_OK},
    {"hash_position84_set_key",packet_057_current,sizeof(packet_057_current),XR_XIR_OK},
    {"hash_position84_bad_key_receiver",packet_058_current,sizeof(packet_058_current),XR_XIR_BAD_TYPE},
    {"hash_position84_bad_value_receiver",packet_059_current,sizeof(packet_059_current),XR_XIR_BAD_TYPE},
    {"hash_position84_bad_key_result",packet_060_current,sizeof(packet_060_current),XR_XIR_BAD_TYPE},
    {"hash_position84_bad_value_result",packet_061_current,sizeof(packet_061_current),XR_XIR_BAD_TYPE},
    {"hash_position84_bad_index",packet_062_current,sizeof(packet_062_current),XR_XIR_BAD_TYPE},
    {"hash_position84_bad_key_proof",packet_063_current,sizeof(packet_063_current),XR_XIR_BAD_TYPE},
    {"hash_projection84_keys",packet_064_current,sizeof(packet_064_current),XR_XIR_OK},
    {"hash_projection84_values",packet_065_current,sizeof(packet_065_current),XR_XIR_OK},
    {"hash_projection84_entries",packet_066_current,sizeof(packet_066_current),XR_XIR_OK},
    {"hash_projection84_set_values",packet_067_current,sizeof(packet_067_current),XR_XIR_OK},
    {"hash_projection84_bad_keys",packet_068_current,sizeof(packet_068_current),XR_XIR_BAD_TYPE},
    {"hash_projection84_bad_values",packet_069_current,sizeof(packet_069_current),XR_XIR_BAD_TYPE},
    {"hash_projection84_bad_set_keys",packet_070_current,sizeof(packet_070_current),XR_XIR_BAD_TYPE},
    {"hash_projection84_bad_entries",packet_071_current,sizeof(packet_071_current),XR_XIR_BAD_TYPE},
    {"hash_projection84_bad_entry_fields",packet_072_current,sizeof(packet_072_current),XR_XIR_BAD_TYPE},
    {"map_index84_golden",packet_073_current,sizeof(packet_073_current),XR_XIR_OK},
    {"map_index84_bad_result",packet_074_current,sizeof(packet_074_current),XR_XIR_BAD_TYPE},
    {"map_index84_bad_key",packet_075_current,sizeof(packet_075_current),XR_XIR_BAD_TYPE},
    {"map_membership84_valid",packet_086_current,sizeof(packet_086_current),XR_XIR_OK},
    {"map_membership84_bad_receiver",packet_087_current,sizeof(packet_087_current),XR_XIR_BAD_TYPE},
    {"map_membership84_bad_value_width",packet_088_current,sizeof(packet_088_current),XR_XIR_BAD_TYPE},
    {"map_membership84_bad_result",packet_089_current,sizeof(packet_089_current),XR_XIR_BAD_TYPE},
    {"map_membership84_bad_equal",packet_090_current,sizeof(packet_090_current),XR_XIR_BAD_TYPE},
    {"map_membership84_bad_key_proof",packet_091_current,sizeof(packet_091_current),XR_XIR_BAD_TYPE},
    {"nested_nullable84_golden",packet_092_current,sizeof(packet_092_current),XR_XIR_OK},
    {"nullable_none84_golden",packet_093_current,sizeof(packet_093_current),XR_XIR_OK},
    {"nullable_some84_golden",packet_094_current,sizeof(packet_094_current),XR_XIR_OK},
    {"rune84_golden",packet_095_current,sizeof(packet_095_current),XR_XIR_OK},
    {"set_algebra84_union",packet_096_current,sizeof(packet_096_current),XR_XIR_OK},
    {"set_algebra84_intersection",packet_097_current,sizeof(packet_097_current),XR_XIR_OK},
    {"set_algebra84_difference",packet_098_current,sizeof(packet_098_current),XR_XIR_OK},
    {"set_algebra84_symmetric_difference",packet_099_current,sizeof(packet_099_current),XR_XIR_OK},
    {"set_algebra84_is_subset",packet_100_current,sizeof(packet_100_current),XR_XIR_OK},
    {"set_algebra84_is_superset",packet_101_current,sizeof(packet_101_current),XR_XIR_OK},
    {"set_algebra84_bad_receiver",packet_102_current,sizeof(packet_102_current),XR_XIR_BAD_TYPE},
    {"set_algebra84_bad_other_kind",packet_103_current,sizeof(packet_103_current),XR_XIR_BAD_TYPE},
    {"set_algebra84_bad_other_element",packet_104_current,sizeof(packet_104_current),XR_XIR_BAD_TYPE},
    {"set_algebra84_bad_set_result",packet_105_current,sizeof(packet_105_current),XR_XIR_BAD_TYPE},
    {"set_algebra84_bad_relation_result",packet_106_current,sizeof(packet_106_current),XR_XIR_BAD_TYPE},
    {"set_algebra84_bad_hashable",packet_107_current,sizeof(packet_107_current),XR_XIR_BAD_TYPE},
};
#endif // P09_HASH_CURRENT84_PACKET_TABLE_H
