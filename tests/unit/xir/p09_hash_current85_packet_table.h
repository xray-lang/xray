/* Independent complete fields with the original semantic acceptance and rejection roles. */
#ifndef P09_HASH_CURRENT85_PACKET_TABLE_H
#define P09_HASH_CURRENT85_PACKET_TABLE_H
#define sizing74_retired_golden sizing74_retained_by85_golden
#define sizing74_retired_embedded_golden sizing74_retained_by85_embedded_golden
#define sizing74_retired_empty_golden sizing74_retained_by85_empty_golden
#include "compile_owner/checked_sizing85_golden.h"
#undef sizing74_retired_golden
#undef sizing74_retired_embedded_golden
#undef sizing74_retired_empty_golden
#include "root_construction85_goldens.h"
#include "xir_assert_panics85_golden.h"
#include "xir_checked_range85_golden.h"
#include "xir_checked_scalar85_golden.h"
#include "xir_error_marker85_golden.h"
#include "xir_generic_method85_golden.h"
#include "xir_hash85_golden.h"
#include "xir_hash_members85_golden.h"
#include "xir_hash_position85_golden.h"
#include "xir_hash_projection85_golden.h"
#include "xir_map_index85_golden.h"
#include "xir_map_membership85_golden.h"
#include "xir_nested_nullable85_golden.h"
#include "xir_nullable85_golden.h"
#include "xir_rune85_golden.h"
#include "xir_set_algebra85_golden.h"
typedef struct CurrentHashPacket { const char *name; const uint8_t *bytes; size_t length; XrXirStatus expected; } CurrentHashPacket;
static const CurrentHashPacket current_hash85_packets[]={
    {"sizing74_retired_golden",sizing74_retained_by85_golden,sizeof(sizing74_retained_by85_golden),XR_XIR_BAD_STRUCTURE},
    {"sizing85_golden",sizing85_golden,sizeof(sizing85_golden),XR_XIR_OK},
    {"sizing74_retired_embedded_golden",sizing74_retained_by85_embedded_golden,sizeof(sizing74_retained_by85_embedded_golden),XR_XIR_BAD_STRUCTURE},
    {"sizing85_embedded_golden",sizing85_embedded_golden,sizeof(sizing85_embedded_golden),XR_XIR_BAD_STRUCTURE},
    {"sizing74_retired_empty_golden",sizing74_retained_by85_empty_golden,sizeof(sizing74_retained_by85_empty_golden),XR_XIR_BAD_STRUCTURE},
    {"sizing85_empty_golden",sizing85_empty_golden,sizeof(sizing85_empty_golden),XR_XIR_BAD_STRUCTURE},
    {"root_construction85_checked",root_construction85_checked,sizeof(root_construction85_checked),XR_XIR_OK},
    {"root_construction85_instance",root_construction85_instance,sizeof(root_construction85_instance),XR_XIR_OK},
    {"assert_panics85_golden",assert_panics85_golden,sizeof(assert_panics85_golden),XR_XIR_OK},
    {"checked_range85_golden",checked_range85_golden,sizeof(checked_range85_golden),XR_XIR_OK},
    {"checked_scalar85_golden",checked_scalar85_golden,sizeof(checked_scalar85_golden),XR_XIR_OK},
    {"error_marker85_golden",error_marker85_golden,sizeof(error_marker85_golden),XR_XIR_OK},
    {"generic_method85_golden",generic_method85_golden,sizeof(generic_method85_golden),XR_XIR_OK},
    {"generic_method85_invalid_flags0",generic_method85_invalid_flags0,sizeof(generic_method85_invalid_flags0),XR_XIR_BAD_TYPE},
    {"generic_method85_invalid_flags1",generic_method85_invalid_flags1,sizeof(generic_method85_invalid_flags1),XR_XIR_BAD_TYPE},
    {"hash85_golden",hash85_golden,sizeof(hash85_golden),XR_XIR_OK},
    {"hash_members85_golden",hash_members85_golden,sizeof(hash_members85_golden),XR_XIR_OK},
    {"hash_position85_map_key",hash_position85_map_key,sizeof(hash_position85_map_key),XR_XIR_OK},
    {"hash_position85_map_value",hash_position85_map_value,sizeof(hash_position85_map_value),XR_XIR_OK},
    {"hash_position85_set_key",hash_position85_set_key,sizeof(hash_position85_set_key),XR_XIR_OK},
    {"hash_position85_bad_key_receiver",hash_position85_bad_key_receiver,sizeof(hash_position85_bad_key_receiver),XR_XIR_BAD_TYPE},
    {"hash_position85_bad_value_receiver",hash_position85_bad_value_receiver,sizeof(hash_position85_bad_value_receiver),XR_XIR_BAD_TYPE},
    {"hash_position85_bad_key_result",hash_position85_bad_key_result,sizeof(hash_position85_bad_key_result),XR_XIR_BAD_TYPE},
    {"hash_position85_bad_value_result",hash_position85_bad_value_result,sizeof(hash_position85_bad_value_result),XR_XIR_BAD_TYPE},
    {"hash_position85_bad_index",hash_position85_bad_index,sizeof(hash_position85_bad_index),XR_XIR_BAD_TYPE},
    {"hash_position85_bad_key_proof",hash_position85_bad_key_proof,sizeof(hash_position85_bad_key_proof),XR_XIR_BAD_TYPE},
    {"hash_projection85_keys",hash_projection85_keys,sizeof(hash_projection85_keys),XR_XIR_OK},
    {"hash_projection85_values",hash_projection85_values,sizeof(hash_projection85_values),XR_XIR_OK},
    {"hash_projection85_entries",hash_projection85_entries,sizeof(hash_projection85_entries),XR_XIR_OK},
    {"hash_projection85_set_values",hash_projection85_set_values,sizeof(hash_projection85_set_values),XR_XIR_OK},
    {"hash_projection85_bad_keys",hash_projection85_bad_keys,sizeof(hash_projection85_bad_keys),XR_XIR_BAD_TYPE},
    {"hash_projection85_bad_values",hash_projection85_bad_values,sizeof(hash_projection85_bad_values),XR_XIR_BAD_TYPE},
    {"hash_projection85_bad_set_keys",hash_projection85_bad_set_keys,sizeof(hash_projection85_bad_set_keys),XR_XIR_BAD_TYPE},
    {"hash_projection85_bad_entries",hash_projection85_bad_entries,sizeof(hash_projection85_bad_entries),XR_XIR_BAD_TYPE},
    {"hash_projection85_bad_entry_fields",hash_projection85_bad_entry_fields,sizeof(hash_projection85_bad_entry_fields),XR_XIR_BAD_TYPE},
    {"map_index85_golden",map_index85_golden,sizeof(map_index85_golden),XR_XIR_OK},
    {"map_index85_bad_result",map_index85_bad_result,sizeof(map_index85_bad_result),XR_XIR_BAD_TYPE},
    {"map_index85_bad_key",map_index85_bad_key,sizeof(map_index85_bad_key),XR_XIR_BAD_TYPE},
    {"map_membership85_valid",map_membership85_valid,sizeof(map_membership85_valid),XR_XIR_OK},
    {"map_membership85_bad_receiver",map_membership85_bad_receiver,sizeof(map_membership85_bad_receiver),XR_XIR_BAD_TYPE},
    {"map_membership85_bad_value_width",map_membership85_bad_value_width,sizeof(map_membership85_bad_value_width),XR_XIR_BAD_TYPE},
    {"map_membership85_bad_result",map_membership85_bad_result,sizeof(map_membership85_bad_result),XR_XIR_BAD_TYPE},
    {"map_membership85_bad_equal",map_membership85_bad_equal,sizeof(map_membership85_bad_equal),XR_XIR_BAD_TYPE},
    {"map_membership85_bad_key_proof",map_membership85_bad_key_proof,sizeof(map_membership85_bad_key_proof),XR_XIR_BAD_TYPE},
    {"nested_nullable85_golden",nested_nullable85_golden,sizeof(nested_nullable85_golden),XR_XIR_OK},
    {"nullable_none85_golden",nullable_none85_golden,sizeof(nullable_none85_golden),XR_XIR_OK},
    {"nullable_some85_golden",nullable_some85_golden,sizeof(nullable_some85_golden),XR_XIR_OK},
    {"rune85_golden",rune85_golden,sizeof(rune85_golden),XR_XIR_OK},
    {"set_algebra85_union",set_algebra85_union,sizeof(set_algebra85_union),XR_XIR_OK},
    {"set_algebra85_intersection",set_algebra85_intersection,sizeof(set_algebra85_intersection),XR_XIR_OK},
    {"set_algebra85_difference",set_algebra85_difference,sizeof(set_algebra85_difference),XR_XIR_OK},
    {"set_algebra85_symmetric_difference",set_algebra85_symmetric_difference,sizeof(set_algebra85_symmetric_difference),XR_XIR_OK},
    {"set_algebra85_is_subset",set_algebra85_is_subset,sizeof(set_algebra85_is_subset),XR_XIR_OK},
    {"set_algebra85_is_superset",set_algebra85_is_superset,sizeof(set_algebra85_is_superset),XR_XIR_OK},
    {"set_algebra85_bad_receiver",set_algebra85_bad_receiver,sizeof(set_algebra85_bad_receiver),XR_XIR_BAD_TYPE},
    {"set_algebra85_bad_other_kind",set_algebra85_bad_other_kind,sizeof(set_algebra85_bad_other_kind),XR_XIR_BAD_TYPE},
    {"set_algebra85_bad_other_element",set_algebra85_bad_other_element,sizeof(set_algebra85_bad_other_element),XR_XIR_BAD_TYPE},
    {"set_algebra85_bad_set_result",set_algebra85_bad_set_result,sizeof(set_algebra85_bad_set_result),XR_XIR_BAD_TYPE},
    {"set_algebra85_bad_relation_result",set_algebra85_bad_relation_result,sizeof(set_algebra85_bad_relation_result),XR_XIR_BAD_TYPE},
    {"set_algebra85_bad_hashable",set_algebra85_bad_hashable,sizeof(set_algebra85_bad_hashable),XR_XIR_BAD_TYPE},
};
#endif // P09_HASH_CURRENT85_PACKET_TABLE_H
