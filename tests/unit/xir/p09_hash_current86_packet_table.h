/* Independent complete fields with the original semantic acceptance and rejection roles. */
#ifndef P09_HASH_CURRENT86_PACKET_TABLE_H
#define P09_HASH_CURRENT86_PACKET_TABLE_H
#define sizing74_retired_golden sizing74_retained_by86_golden
#define sizing74_retired_embedded_golden sizing74_retained_by86_embedded_golden
#define sizing74_retired_empty_golden sizing74_retained_by86_empty_golden
#include "compile_owner/checked_sizing85_golden.h"
#undef sizing74_retired_golden
#undef sizing74_retired_embedded_golden
#undef sizing74_retired_empty_golden
#include "compile_owner/checked_sizing86_golden.h"
#include "root_construction86_goldens.h"
#include "xir_assert_panics86_golden.h"
#include "xir_checked_range86_golden.h"
#include "xir_checked_scalar86_golden.h"
#include "xir_error_marker86_golden.h"
#include "xir_generic_method86_golden.h"
#include "xir_hash86_golden.h"
#include "xir_hash_members86_golden.h"
#include "xir_hash_position86_golden.h"
#include "xir_hash_projection86_golden.h"
#include "xir_map_index86_golden.h"
#include "xir_map_membership86_golden.h"
#include "xir_nested_nullable86_golden.h"
#include "xir_nullable86_golden.h"
#include "xir_rune86_golden.h"
#include "xir_set_algebra86_golden.h"
typedef struct CurrentHashPacket { const char *name; const uint8_t *bytes; size_t length; XrXirStatus expected; } CurrentHashPacket;
static const CurrentHashPacket current_hash86_packets[]={
    {"sizing74_retired_golden",sizing74_retained_by86_golden,sizeof(sizing74_retained_by86_golden),XR_XIR_BAD_STRUCTURE},
    {"sizing86_golden",sizing86_golden,sizeof(sizing86_golden),XR_XIR_OK},
    {"sizing74_retired_embedded_golden",sizing74_retained_by86_embedded_golden,sizeof(sizing74_retained_by86_embedded_golden),XR_XIR_BAD_STRUCTURE},
    {"sizing86_embedded_golden",sizing86_embedded_golden,sizeof(sizing86_embedded_golden),XR_XIR_BAD_STRUCTURE},
    {"sizing74_retired_empty_golden",sizing74_retained_by86_empty_golden,sizeof(sizing74_retained_by86_empty_golden),XR_XIR_BAD_STRUCTURE},
    {"sizing86_empty_golden",sizing86_empty_golden,sizeof(sizing86_empty_golden),XR_XIR_BAD_STRUCTURE},
    {"root_construction86_checked",root_construction86_checked,sizeof(root_construction86_checked),XR_XIR_OK},
    {"root_construction86_instance",root_construction86_instance,sizeof(root_construction86_instance),XR_XIR_OK},
    {"assert_panics86_golden",assert_panics86_golden,sizeof(assert_panics86_golden),XR_XIR_OK},
    {"checked_range86_golden",checked_range86_golden,sizeof(checked_range86_golden),XR_XIR_OK},
    {"checked_scalar86_golden",checked_scalar86_golden,sizeof(checked_scalar86_golden),XR_XIR_OK},
    {"error_marker86_golden",error_marker86_golden,sizeof(error_marker86_golden),XR_XIR_OK},
    {"generic_method86_golden",generic_method86_golden,sizeof(generic_method86_golden),XR_XIR_OK},
    {"generic_method86_invalid_flags0",generic_method86_invalid_flags0,sizeof(generic_method86_invalid_flags0),XR_XIR_BAD_TYPE},
    {"generic_method86_invalid_flags1",generic_method86_invalid_flags1,sizeof(generic_method86_invalid_flags1),XR_XIR_BAD_TYPE},
    {"hash86_golden",hash86_golden,sizeof(hash86_golden),XR_XIR_OK},
    {"hash_members86_golden",hash_members86_golden,sizeof(hash_members86_golden),XR_XIR_OK},
    {"hash_position86_map_key",hash_position86_map_key,sizeof(hash_position86_map_key),XR_XIR_OK},
    {"hash_position86_map_value",hash_position86_map_value,sizeof(hash_position86_map_value),XR_XIR_OK},
    {"hash_position86_set_key",hash_position86_set_key,sizeof(hash_position86_set_key),XR_XIR_OK},
    {"hash_position86_bad_key_receiver",hash_position86_bad_key_receiver,sizeof(hash_position86_bad_key_receiver),XR_XIR_BAD_TYPE},
    {"hash_position86_bad_value_receiver",hash_position86_bad_value_receiver,sizeof(hash_position86_bad_value_receiver),XR_XIR_BAD_TYPE},
    {"hash_position86_bad_key_result",hash_position86_bad_key_result,sizeof(hash_position86_bad_key_result),XR_XIR_BAD_TYPE},
    {"hash_position86_bad_value_result",hash_position86_bad_value_result,sizeof(hash_position86_bad_value_result),XR_XIR_BAD_TYPE},
    {"hash_position86_bad_index",hash_position86_bad_index,sizeof(hash_position86_bad_index),XR_XIR_BAD_TYPE},
    {"hash_position86_bad_key_proof",hash_position86_bad_key_proof,sizeof(hash_position86_bad_key_proof),XR_XIR_BAD_TYPE},
    {"hash_projection86_keys",hash_projection86_keys,sizeof(hash_projection86_keys),XR_XIR_OK},
    {"hash_projection86_values",hash_projection86_values,sizeof(hash_projection86_values),XR_XIR_OK},
    {"hash_projection86_entries",hash_projection86_entries,sizeof(hash_projection86_entries),XR_XIR_OK},
    {"hash_projection86_set_values",hash_projection86_set_values,sizeof(hash_projection86_set_values),XR_XIR_OK},
    {"hash_projection86_bad_keys",hash_projection86_bad_keys,sizeof(hash_projection86_bad_keys),XR_XIR_BAD_TYPE},
    {"hash_projection86_bad_values",hash_projection86_bad_values,sizeof(hash_projection86_bad_values),XR_XIR_BAD_TYPE},
    {"hash_projection86_bad_set_keys",hash_projection86_bad_set_keys,sizeof(hash_projection86_bad_set_keys),XR_XIR_BAD_TYPE},
    {"hash_projection86_bad_entries",hash_projection86_bad_entries,sizeof(hash_projection86_bad_entries),XR_XIR_BAD_TYPE},
    {"hash_projection86_bad_entry_fields",hash_projection86_bad_entry_fields,sizeof(hash_projection86_bad_entry_fields),XR_XIR_BAD_TYPE},
    {"map_index86_golden",map_index86_golden,sizeof(map_index86_golden),XR_XIR_OK},
    {"map_index86_bad_result",map_index86_bad_result,sizeof(map_index86_bad_result),XR_XIR_BAD_TYPE},
    {"map_index86_bad_key",map_index86_bad_key,sizeof(map_index86_bad_key),XR_XIR_BAD_TYPE},
    {"map_membership86_valid",map_membership86_valid,sizeof(map_membership86_valid),XR_XIR_OK},
    {"map_membership86_bad_receiver",map_membership86_bad_receiver,sizeof(map_membership86_bad_receiver),XR_XIR_BAD_TYPE},
    {"map_membership86_bad_value_width",map_membership86_bad_value_width,sizeof(map_membership86_bad_value_width),XR_XIR_BAD_TYPE},
    {"map_membership86_bad_result",map_membership86_bad_result,sizeof(map_membership86_bad_result),XR_XIR_BAD_TYPE},
    {"map_membership86_bad_equal",map_membership86_bad_equal,sizeof(map_membership86_bad_equal),XR_XIR_BAD_TYPE},
    {"map_membership86_bad_key_proof",map_membership86_bad_key_proof,sizeof(map_membership86_bad_key_proof),XR_XIR_BAD_TYPE},
    {"nested_nullable86_golden",nested_nullable86_golden,sizeof(nested_nullable86_golden),XR_XIR_OK},
    {"nullable_none86_golden",nullable_none86_golden,sizeof(nullable_none86_golden),XR_XIR_OK},
    {"nullable_some86_golden",nullable_some86_golden,sizeof(nullable_some86_golden),XR_XIR_OK},
    {"rune86_golden",rune86_golden,sizeof(rune86_golden),XR_XIR_OK},
    {"set_algebra86_union",set_algebra86_union,sizeof(set_algebra86_union),XR_XIR_OK},
    {"set_algebra86_intersection",set_algebra86_intersection,sizeof(set_algebra86_intersection),XR_XIR_OK},
    {"set_algebra86_difference",set_algebra86_difference,sizeof(set_algebra86_difference),XR_XIR_OK},
    {"set_algebra86_symmetric_difference",set_algebra86_symmetric_difference,sizeof(set_algebra86_symmetric_difference),XR_XIR_OK},
    {"set_algebra86_is_subset",set_algebra86_is_subset,sizeof(set_algebra86_is_subset),XR_XIR_OK},
    {"set_algebra86_is_superset",set_algebra86_is_superset,sizeof(set_algebra86_is_superset),XR_XIR_OK},
    {"set_algebra86_bad_receiver",set_algebra86_bad_receiver,sizeof(set_algebra86_bad_receiver),XR_XIR_BAD_TYPE},
    {"set_algebra86_bad_other_kind",set_algebra86_bad_other_kind,sizeof(set_algebra86_bad_other_kind),XR_XIR_BAD_TYPE},
    {"set_algebra86_bad_other_element",set_algebra86_bad_other_element,sizeof(set_algebra86_bad_other_element),XR_XIR_BAD_TYPE},
    {"set_algebra86_bad_set_result",set_algebra86_bad_set_result,sizeof(set_algebra86_bad_set_result),XR_XIR_BAD_TYPE},
    {"set_algebra86_bad_relation_result",set_algebra86_bad_relation_result,sizeof(set_algebra86_bad_relation_result),XR_XIR_BAD_TYPE},
    {"set_algebra86_bad_hashable",set_algebra86_bad_hashable,sizeof(set_algebra86_bad_hashable),XR_XIR_BAD_TYPE},
};
#endif // P09_HASH_CURRENT86_PACKET_TABLE_H
