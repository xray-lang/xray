/* Independent complete fields with the original semantic acceptance and rejection roles. */
#ifndef P09_HASH_CURRENT83_PACKET_TABLE_H
#define P09_HASH_CURRENT83_PACKET_TABLE_H
#define sizing74_retired_golden sizing74_retained_by83_golden
#define sizing74_retired_embedded_golden sizing74_retained_by83_embedded_golden
#define sizing74_retired_empty_golden sizing74_retained_by83_empty_golden
#include "compile_owner/checked_sizing83_golden.h"
#undef sizing74_retired_golden
#undef sizing74_retired_embedded_golden
#undef sizing74_retired_empty_golden
#include "hash_protocol28_inputs/root_construction83_goldens.h"
#include "hash_protocol28_inputs/xir_assert_panics83_golden.h"
#include "hash_protocol28_inputs/xir_checked_range83_golden.h"
#include "hash_protocol28_inputs/xir_checked_scalar83_golden.h"
#include "hash_protocol28_inputs/xir_error_marker83_golden.h"
#include "hash_protocol28_inputs/xir_generic_method83_golden.h"
#include "hash_protocol28_inputs/xir_hash83_golden.h"
#include "hash_protocol28_inputs/xir_hash_members83_golden.h"
#include "hash_protocol28_inputs/xir_hash_position83_golden.h"
#include "hash_protocol28_inputs/xir_hash_projection83_golden.h"
#include "hash_protocol28_inputs/xir_map_index83_golden.h"
#include "hash_protocol28_inputs/xir_map_membership83_golden.h"
#include "hash_protocol28_inputs/xir_nested_nullable83_golden.h"
#include "hash_protocol28_inputs/xir_nullable83_golden.h"
#include "hash_protocol28_inputs/xir_rune83_golden.h"
#include "hash_protocol28_inputs/xir_set_algebra83_golden.h"
typedef struct CurrentHashPacket { const char *name; const uint8_t *bytes; size_t length; XrXirStatus expected; } CurrentHashPacket;
static const CurrentHashPacket current_hash83_packets[]={
    {"sizing74_retired_golden",sizing74_retained_by83_golden,sizeof(sizing74_retained_by83_golden),XR_XIR_BAD_STRUCTURE},
    {"sizing83_golden",sizing83_golden,sizeof(sizing83_golden),XR_XIR_OK},
    {"sizing74_retired_embedded_golden",sizing74_retained_by83_embedded_golden,sizeof(sizing74_retained_by83_embedded_golden),XR_XIR_BAD_STRUCTURE},
    {"sizing83_embedded_golden",sizing83_embedded_golden,sizeof(sizing83_embedded_golden),XR_XIR_BAD_STRUCTURE},
    {"sizing74_retired_empty_golden",sizing74_retained_by83_empty_golden,sizeof(sizing74_retained_by83_empty_golden),XR_XIR_BAD_STRUCTURE},
    {"sizing83_empty_golden",sizing83_empty_golden,sizeof(sizing83_empty_golden),XR_XIR_BAD_STRUCTURE},
    {"root_construction83_checked",root_construction83_checked,sizeof(root_construction83_checked),XR_XIR_OK},
    {"root_construction83_instance",root_construction83_instance,sizeof(root_construction83_instance),XR_XIR_OK},
    {"assert_panics83_golden",assert_panics83_golden,sizeof(assert_panics83_golden),XR_XIR_OK},
    {"checked_range83_golden",checked_range83_golden,sizeof(checked_range83_golden),XR_XIR_OK},
    {"checked_scalar83_golden",checked_scalar83_golden,sizeof(checked_scalar83_golden),XR_XIR_OK},
    {"error_marker83_golden",error_marker83_golden,sizeof(error_marker83_golden),XR_XIR_OK},
    {"generic_method83_golden",generic_method83_golden,sizeof(generic_method83_golden),XR_XIR_OK},
    {"generic_method83_invalid_flags0",generic_method83_invalid_flags0,sizeof(generic_method83_invalid_flags0),XR_XIR_BAD_TYPE},
    {"generic_method83_invalid_flags1",generic_method83_invalid_flags1,sizeof(generic_method83_invalid_flags1),XR_XIR_BAD_TYPE},
    {"hash83_golden",hash83_golden,sizeof(hash83_golden),XR_XIR_OK},
    {"hash_members83_golden",hash_members83_golden,sizeof(hash_members83_golden),XR_XIR_OK},
    {"hash_position83_map_key",hash_position83_map_key,sizeof(hash_position83_map_key),XR_XIR_OK},
    {"hash_position83_map_value",hash_position83_map_value,sizeof(hash_position83_map_value),XR_XIR_OK},
    {"hash_position83_set_key",hash_position83_set_key,sizeof(hash_position83_set_key),XR_XIR_OK},
    {"hash_position83_bad_key_receiver",hash_position83_bad_key_receiver,sizeof(hash_position83_bad_key_receiver),XR_XIR_BAD_TYPE},
    {"hash_position83_bad_value_receiver",hash_position83_bad_value_receiver,sizeof(hash_position83_bad_value_receiver),XR_XIR_BAD_TYPE},
    {"hash_position83_bad_key_result",hash_position83_bad_key_result,sizeof(hash_position83_bad_key_result),XR_XIR_BAD_TYPE},
    {"hash_position83_bad_value_result",hash_position83_bad_value_result,sizeof(hash_position83_bad_value_result),XR_XIR_BAD_TYPE},
    {"hash_position83_bad_index",hash_position83_bad_index,sizeof(hash_position83_bad_index),XR_XIR_BAD_TYPE},
    {"hash_position83_bad_key_proof",hash_position83_bad_key_proof,sizeof(hash_position83_bad_key_proof),XR_XIR_BAD_TYPE},
    {"hash_projection83_keys",hash_projection83_keys,sizeof(hash_projection83_keys),XR_XIR_OK},
    {"hash_projection83_values",hash_projection83_values,sizeof(hash_projection83_values),XR_XIR_OK},
    {"hash_projection83_entries",hash_projection83_entries,sizeof(hash_projection83_entries),XR_XIR_OK},
    {"hash_projection83_set_values",hash_projection83_set_values,sizeof(hash_projection83_set_values),XR_XIR_OK},
    {"hash_projection83_bad_keys",hash_projection83_bad_keys,sizeof(hash_projection83_bad_keys),XR_XIR_BAD_TYPE},
    {"hash_projection83_bad_values",hash_projection83_bad_values,sizeof(hash_projection83_bad_values),XR_XIR_BAD_TYPE},
    {"hash_projection83_bad_set_keys",hash_projection83_bad_set_keys,sizeof(hash_projection83_bad_set_keys),XR_XIR_BAD_TYPE},
    {"hash_projection83_bad_entries",hash_projection83_bad_entries,sizeof(hash_projection83_bad_entries),XR_XIR_BAD_TYPE},
    {"hash_projection83_bad_entry_fields",hash_projection83_bad_entry_fields,sizeof(hash_projection83_bad_entry_fields),XR_XIR_BAD_TYPE},
    {"map_index83_golden",map_index83_golden,sizeof(map_index83_golden),XR_XIR_OK},
    {"map_index83_bad_result",map_index83_bad_result,sizeof(map_index83_bad_result),XR_XIR_BAD_TYPE},
    {"map_index83_bad_key",map_index83_bad_key,sizeof(map_index83_bad_key),XR_XIR_BAD_TYPE},
    {"map_membership83_valid",map_membership83_valid,sizeof(map_membership83_valid),XR_XIR_OK},
    {"map_membership83_bad_receiver",map_membership83_bad_receiver,sizeof(map_membership83_bad_receiver),XR_XIR_BAD_TYPE},
    {"map_membership83_bad_value_width",map_membership83_bad_value_width,sizeof(map_membership83_bad_value_width),XR_XIR_BAD_TYPE},
    {"map_membership83_bad_result",map_membership83_bad_result,sizeof(map_membership83_bad_result),XR_XIR_BAD_TYPE},
    {"map_membership83_bad_equal",map_membership83_bad_equal,sizeof(map_membership83_bad_equal),XR_XIR_BAD_TYPE},
    {"map_membership83_bad_key_proof",map_membership83_bad_key_proof,sizeof(map_membership83_bad_key_proof),XR_XIR_BAD_TYPE},
    {"nested_nullable83_golden",nested_nullable83_golden,sizeof(nested_nullable83_golden),XR_XIR_OK},
    {"nullable_none83_golden",nullable_none83_golden,sizeof(nullable_none83_golden),XR_XIR_OK},
    {"nullable_some83_golden",nullable_some83_golden,sizeof(nullable_some83_golden),XR_XIR_OK},
    {"rune83_golden",rune83_golden,sizeof(rune83_golden),XR_XIR_OK},
    {"set_algebra83_union",set_algebra83_union,sizeof(set_algebra83_union),XR_XIR_OK},
    {"set_algebra83_intersection",set_algebra83_intersection,sizeof(set_algebra83_intersection),XR_XIR_OK},
    {"set_algebra83_difference",set_algebra83_difference,sizeof(set_algebra83_difference),XR_XIR_OK},
    {"set_algebra83_symmetric_difference",set_algebra83_symmetric_difference,sizeof(set_algebra83_symmetric_difference),XR_XIR_OK},
    {"set_algebra83_is_subset",set_algebra83_is_subset,sizeof(set_algebra83_is_subset),XR_XIR_OK},
    {"set_algebra83_is_superset",set_algebra83_is_superset,sizeof(set_algebra83_is_superset),XR_XIR_OK},
    {"set_algebra83_bad_receiver",set_algebra83_bad_receiver,sizeof(set_algebra83_bad_receiver),XR_XIR_BAD_TYPE},
    {"set_algebra83_bad_other_kind",set_algebra83_bad_other_kind,sizeof(set_algebra83_bad_other_kind),XR_XIR_BAD_TYPE},
    {"set_algebra83_bad_other_element",set_algebra83_bad_other_element,sizeof(set_algebra83_bad_other_element),XR_XIR_BAD_TYPE},
    {"set_algebra83_bad_set_result",set_algebra83_bad_set_result,sizeof(set_algebra83_bad_set_result),XR_XIR_BAD_TYPE},
    {"set_algebra83_bad_relation_result",set_algebra83_bad_relation_result,sizeof(set_algebra83_bad_relation_result),XR_XIR_BAD_TYPE},
    {"set_algebra83_bad_hashable",set_algebra83_bad_hashable,sizeof(set_algebra83_bad_hashable),XR_XIR_BAD_TYPE},
};
#endif // P09_HASH_CURRENT83_PACKET_TABLE_H
