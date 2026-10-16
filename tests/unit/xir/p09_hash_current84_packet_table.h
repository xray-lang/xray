/* Independent complete fields with the original semantic acceptance and rejection roles. */
#ifndef P09_HASH_CURRENT84_PACKET_TABLE_H
#define P09_HASH_CURRENT84_PACKET_TABLE_H
#include "compile_owner/checked_sizing84_golden.h"
#include "root_construction84_goldens.h"
#include "xir_assert_panics84_golden.h"
#include "xir_checked_range84_golden.h"
#include "xir_checked_scalar84_golden.h"
#include "xir_error_marker84_golden.h"
#include "xir_generic_method84_golden.h"
#include "xir_hash84_golden.h"
#include "xir_hash_members84_golden.h"
#include "xir_hash_position84_golden.h"
#include "xir_hash_projection84_golden.h"
#include "xir_map_index84_golden.h"
#include "xir_map_membership84_golden.h"
#include "xir_nested_nullable84_golden.h"
#include "xir_nullable84_golden.h"
#include "xir_rune84_golden.h"
#include "xir_set_algebra84_golden.h"
typedef struct CurrentHashPacket { const char *name; const uint8_t *bytes; size_t length; XrXirStatus expected; } CurrentHashPacket;
static const CurrentHashPacket current_hash84_packets[]={
    {"sizing74_retired_golden",retired_sizing74_golden,sizeof(retired_sizing74_golden),XR_XIR_BAD_STRUCTURE},
    {"sizing84_golden",sizing84_golden,sizeof(sizing84_golden),XR_XIR_OK},
    {"sizing74_retired_embedded_golden",retired_sizing74_embedded_golden,sizeof(retired_sizing74_embedded_golden),XR_XIR_BAD_STRUCTURE},
    {"sizing84_embedded_golden",sizing84_embedded_golden,sizeof(sizing84_embedded_golden),XR_XIR_BAD_STRUCTURE},
    {"sizing74_retired_empty_golden",retired_sizing74_empty_golden,sizeof(retired_sizing74_empty_golden),XR_XIR_BAD_STRUCTURE},
    {"sizing84_empty_golden",sizing84_empty_golden,sizeof(sizing84_empty_golden),XR_XIR_BAD_STRUCTURE},
    {"root_construction84_checked",root_construction84_checked,sizeof(root_construction84_checked),XR_XIR_OK},
    {"root_construction84_instance",root_construction84_instance,sizeof(root_construction84_instance),XR_XIR_OK},
    {"assert_panics84_golden",assert_panics84_golden,sizeof(assert_panics84_golden),XR_XIR_OK},
    {"checked_range84_golden",checked_range84_golden,sizeof(checked_range84_golden),XR_XIR_OK},
    {"checked_scalar84_golden",checked_scalar84_golden,sizeof(checked_scalar84_golden),XR_XIR_OK},
    {"error_marker84_golden",error_marker84_golden,sizeof(error_marker84_golden),XR_XIR_OK},
    {"generic_method84_golden",generic_method84_golden,sizeof(generic_method84_golden),XR_XIR_OK},
    {"generic_method84_invalid_flags0",generic_method84_invalid_flags0,sizeof(generic_method84_invalid_flags0),XR_XIR_BAD_TYPE},
    {"generic_method84_invalid_flags1",generic_method84_invalid_flags1,sizeof(generic_method84_invalid_flags1),XR_XIR_BAD_TYPE},
    {"hash84_golden",hash84_golden,sizeof(hash84_golden),XR_XIR_OK},
    {"hash_members84_golden",hash_members84_golden,sizeof(hash_members84_golden),XR_XIR_OK},
    {"hash_position84_map_key",hash_position84_map_key,sizeof(hash_position84_map_key),XR_XIR_OK},
    {"hash_position84_map_value",hash_position84_map_value,sizeof(hash_position84_map_value),XR_XIR_OK},
    {"hash_position84_set_key",hash_position84_set_key,sizeof(hash_position84_set_key),XR_XIR_OK},
    {"hash_position84_bad_key_receiver",hash_position84_bad_key_receiver,sizeof(hash_position84_bad_key_receiver),XR_XIR_BAD_TYPE},
    {"hash_position84_bad_value_receiver",hash_position84_bad_value_receiver,sizeof(hash_position84_bad_value_receiver),XR_XIR_BAD_TYPE},
    {"hash_position84_bad_key_result",hash_position84_bad_key_result,sizeof(hash_position84_bad_key_result),XR_XIR_BAD_TYPE},
    {"hash_position84_bad_value_result",hash_position84_bad_value_result,sizeof(hash_position84_bad_value_result),XR_XIR_BAD_TYPE},
    {"hash_position84_bad_index",hash_position84_bad_index,sizeof(hash_position84_bad_index),XR_XIR_BAD_TYPE},
    {"hash_position84_bad_key_proof",hash_position84_bad_key_proof,sizeof(hash_position84_bad_key_proof),XR_XIR_BAD_TYPE},
    {"hash_projection84_keys",hash_projection84_keys,sizeof(hash_projection84_keys),XR_XIR_OK},
    {"hash_projection84_values",hash_projection84_values,sizeof(hash_projection84_values),XR_XIR_OK},
    {"hash_projection84_entries",hash_projection84_entries,sizeof(hash_projection84_entries),XR_XIR_OK},
    {"hash_projection84_set_values",hash_projection84_set_values,sizeof(hash_projection84_set_values),XR_XIR_OK},
    {"hash_projection84_bad_keys",hash_projection84_bad_keys,sizeof(hash_projection84_bad_keys),XR_XIR_BAD_TYPE},
    {"hash_projection84_bad_values",hash_projection84_bad_values,sizeof(hash_projection84_bad_values),XR_XIR_BAD_TYPE},
    {"hash_projection84_bad_set_keys",hash_projection84_bad_set_keys,sizeof(hash_projection84_bad_set_keys),XR_XIR_BAD_TYPE},
    {"hash_projection84_bad_entries",hash_projection84_bad_entries,sizeof(hash_projection84_bad_entries),XR_XIR_BAD_TYPE},
    {"hash_projection84_bad_entry_fields",hash_projection84_bad_entry_fields,sizeof(hash_projection84_bad_entry_fields),XR_XIR_BAD_TYPE},
    {"map_index84_golden",map_index84_golden,sizeof(map_index84_golden),XR_XIR_OK},
    {"map_index84_bad_result",map_index84_bad_result,sizeof(map_index84_bad_result),XR_XIR_BAD_TYPE},
    {"map_index84_bad_key",map_index84_bad_key,sizeof(map_index84_bad_key),XR_XIR_BAD_TYPE},
    {"map_membership84_valid",map_membership84_valid,sizeof(map_membership84_valid),XR_XIR_OK},
    {"map_membership84_bad_receiver",map_membership84_bad_receiver,sizeof(map_membership84_bad_receiver),XR_XIR_BAD_TYPE},
    {"map_membership84_bad_value_width",map_membership84_bad_value_width,sizeof(map_membership84_bad_value_width),XR_XIR_BAD_TYPE},
    {"map_membership84_bad_result",map_membership84_bad_result,sizeof(map_membership84_bad_result),XR_XIR_BAD_TYPE},
    {"map_membership84_bad_equal",map_membership84_bad_equal,sizeof(map_membership84_bad_equal),XR_XIR_BAD_TYPE},
    {"map_membership84_bad_key_proof",map_membership84_bad_key_proof,sizeof(map_membership84_bad_key_proof),XR_XIR_BAD_TYPE},
    {"nested_nullable84_golden",nested_nullable84_golden,sizeof(nested_nullable84_golden),XR_XIR_OK},
    {"nullable_none84_golden",nullable_none84_golden,sizeof(nullable_none84_golden),XR_XIR_OK},
    {"nullable_some84_golden",nullable_some84_golden,sizeof(nullable_some84_golden),XR_XIR_OK},
    {"rune84_golden",rune84_golden,sizeof(rune84_golden),XR_XIR_OK},
    {"set_algebra84_union",set_algebra84_union,sizeof(set_algebra84_union),XR_XIR_OK},
    {"set_algebra84_intersection",set_algebra84_intersection,sizeof(set_algebra84_intersection),XR_XIR_OK},
    {"set_algebra84_difference",set_algebra84_difference,sizeof(set_algebra84_difference),XR_XIR_OK},
    {"set_algebra84_symmetric_difference",set_algebra84_symmetric_difference,sizeof(set_algebra84_symmetric_difference),XR_XIR_OK},
    {"set_algebra84_is_subset",set_algebra84_is_subset,sizeof(set_algebra84_is_subset),XR_XIR_OK},
    {"set_algebra84_is_superset",set_algebra84_is_superset,sizeof(set_algebra84_is_superset),XR_XIR_OK},
    {"set_algebra84_bad_receiver",set_algebra84_bad_receiver,sizeof(set_algebra84_bad_receiver),XR_XIR_BAD_TYPE},
    {"set_algebra84_bad_other_kind",set_algebra84_bad_other_kind,sizeof(set_algebra84_bad_other_kind),XR_XIR_BAD_TYPE},
    {"set_algebra84_bad_other_element",set_algebra84_bad_other_element,sizeof(set_algebra84_bad_other_element),XR_XIR_BAD_TYPE},
    {"set_algebra84_bad_set_result",set_algebra84_bad_set_result,sizeof(set_algebra84_bad_set_result),XR_XIR_BAD_TYPE},
    {"set_algebra84_bad_relation_result",set_algebra84_bad_relation_result,sizeof(set_algebra84_bad_relation_result),XR_XIR_BAD_TYPE},
    {"set_algebra84_bad_hashable",set_algebra84_bad_hashable,sizeof(set_algebra84_bad_hashable),XR_XIR_BAD_TYPE},
};
#endif // P09_HASH_CURRENT84_PACKET_TABLE_H
