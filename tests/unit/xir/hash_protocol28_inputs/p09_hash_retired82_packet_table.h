/* Complete retired field models remain independent early-rejection inputs. */
#ifndef P09_HASH_RETIRED82_PACKET_TABLE_H
#define P09_HASH_RETIRED82_PACKET_TABLE_H
#include "compile_owner/checked_sizing82_golden.h"
#include "root_construction82_goldens.h"
#include "xir_assert_panics82_golden.h"
#include "xir_checked_range82_golden.h"
#include "xir_checked_scalar82_golden.h"
#include "xir_error_marker82_golden.h"
#include "xir_generic_method82_golden.h"
#include "xir_hash82_golden.h"
#include "xir_hash_members82_golden.h"
#include "xir_hash_position82_golden.h"
#include "xir_hash_projection82_golden.h"
#include "xir_map_index82_golden.h"
#include "xir_map_membership82_golden.h"
#include "xir_nested_nullable82_golden.h"
#include "xir_nullable82_golden.h"
#include "xir_rune82_golden.h"
#include "xir_set_algebra82_golden.h"
typedef struct RetiredHashPacket { const uint8_t *bytes; size_t length; } RetiredHashPacket;
static const RetiredHashPacket retired_hash82_packets[]={
    {sizing74_retired_golden,sizeof(sizing74_retired_golden)},
    {sizing82_golden,sizeof(sizing82_golden)},
    {sizing74_retired_embedded_golden,sizeof(sizing74_retired_embedded_golden)},
    {sizing82_embedded_golden,sizeof(sizing82_embedded_golden)},
    {sizing74_retired_empty_golden,sizeof(sizing74_retired_empty_golden)},
    {sizing82_empty_golden,sizeof(sizing82_empty_golden)},
    {root_construction82_checked,sizeof(root_construction82_checked)},
    {root_construction82_instance,sizeof(root_construction82_instance)},
    {assert_panics82_golden,sizeof(assert_panics82_golden)},
    {checked_range82_golden,sizeof(checked_range82_golden)},
    {checked_scalar82_golden,sizeof(checked_scalar82_golden)},
    {error_marker82_golden,sizeof(error_marker82_golden)},
    {generic_method82_golden,sizeof(generic_method82_golden)},
    {generic_method82_invalid_flags0,sizeof(generic_method82_invalid_flags0)},
    {generic_method82_invalid_flags1,sizeof(generic_method82_invalid_flags1)},
    {hash82_golden,sizeof(hash82_golden)},
    {hash_members82_golden,sizeof(hash_members82_golden)},
    {hash_position82_map_key,sizeof(hash_position82_map_key)},
    {hash_position82_map_value,sizeof(hash_position82_map_value)},
    {hash_position82_set_key,sizeof(hash_position82_set_key)},
    {hash_position82_bad_key_receiver,sizeof(hash_position82_bad_key_receiver)},
    {hash_position82_bad_value_receiver,sizeof(hash_position82_bad_value_receiver)},
    {hash_position82_bad_key_result,sizeof(hash_position82_bad_key_result)},
    {hash_position82_bad_value_result,sizeof(hash_position82_bad_value_result)},
    {hash_position82_bad_index,sizeof(hash_position82_bad_index)},
    {hash_position82_bad_key_proof,sizeof(hash_position82_bad_key_proof)},
    {hash_projection82_keys,sizeof(hash_projection82_keys)},
    {hash_projection82_values,sizeof(hash_projection82_values)},
    {hash_projection82_entries,sizeof(hash_projection82_entries)},
    {hash_projection82_set_values,sizeof(hash_projection82_set_values)},
    {hash_projection82_bad_keys,sizeof(hash_projection82_bad_keys)},
    {hash_projection82_bad_values,sizeof(hash_projection82_bad_values)},
    {hash_projection82_bad_set_keys,sizeof(hash_projection82_bad_set_keys)},
    {hash_projection82_bad_entries,sizeof(hash_projection82_bad_entries)},
    {hash_projection82_bad_entry_fields,sizeof(hash_projection82_bad_entry_fields)},
    {map_index82_golden,sizeof(map_index82_golden)},
    {map_index82_bad_result,sizeof(map_index82_bad_result)},
    {map_index82_bad_key,sizeof(map_index82_bad_key)},
    {map_membership82_valid,sizeof(map_membership82_valid)},
    {map_membership82_bad_receiver,sizeof(map_membership82_bad_receiver)},
    {map_membership82_bad_value_width,sizeof(map_membership82_bad_value_width)},
    {map_membership82_bad_result,sizeof(map_membership82_bad_result)},
    {map_membership82_bad_equal,sizeof(map_membership82_bad_equal)},
    {map_membership82_bad_key_proof,sizeof(map_membership82_bad_key_proof)},
    {nested_nullable82_golden,sizeof(nested_nullable82_golden)},
    {nullable_none82_golden,sizeof(nullable_none82_golden)},
    {nullable_some82_golden,sizeof(nullable_some82_golden)},
    {rune82_golden,sizeof(rune82_golden)},
    {set_algebra82_union,sizeof(set_algebra82_union)},
    {set_algebra82_intersection,sizeof(set_algebra82_intersection)},
    {set_algebra82_difference,sizeof(set_algebra82_difference)},
    {set_algebra82_symmetric_difference,sizeof(set_algebra82_symmetric_difference)},
    {set_algebra82_is_subset,sizeof(set_algebra82_is_subset)},
    {set_algebra82_is_superset,sizeof(set_algebra82_is_superset)},
    {set_algebra82_bad_receiver,sizeof(set_algebra82_bad_receiver)},
    {set_algebra82_bad_other_kind,sizeof(set_algebra82_bad_other_kind)},
    {set_algebra82_bad_other_element,sizeof(set_algebra82_bad_other_element)},
    {set_algebra82_bad_set_result,sizeof(set_algebra82_bad_set_result)},
    {set_algebra82_bad_relation_result,sizeof(set_algebra82_bad_relation_result)},
    {set_algebra82_bad_hashable,sizeof(set_algebra82_bad_hashable)},
};
#endif // P09_HASH_RETIRED82_PACKET_TABLE_H
