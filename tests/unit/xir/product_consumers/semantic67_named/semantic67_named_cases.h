/* Current vectors and two authentic historical versions retain separate roles. */
#ifndef SEMANTIC67_NAMED_CASES_H
#define SEMANTIC67_NAMED_CASES_H
#include "semantic67_core.inc.c"
#include "semantic65_core.inc.c"
#include "semantic64_core.inc.c"
#include "semantic67_defaults.inc.c"
#include "semantic65_defaults.inc.c"
#include "semantic64_defaults.inc.c"
#include "semantic67_strings_ordering.inc.c"
#include "semantic65_strings_ordering.inc.c"
#include "semantic64_strings_ordering.inc.c"
typedef struct Semantic67NamedVector {
    const char *name; size_t index; const uint8_t *current; size_t current_size;
    const uint8_t *previous65; size_t previous65_size;
    const uint8_t *previous64; size_t previous64_size; bool reader_positive;
} Semantic67NamedVector;
static const Semantic67NamedVector semantic67_named_vectors[] = {
    {"sizing67_golden",0,named_packet_67_0,sizeof(named_packet_67_0),named_packet_65_0,sizeof(named_packet_65_0),named_packet_64_0,sizeof(named_packet_64_0),true},
    {"sizing67_embedded_golden",1,named_packet_67_1,sizeof(named_packet_67_1),named_packet_65_1,sizeof(named_packet_65_1),named_packet_64_1,sizeof(named_packet_64_1),false},
    {"sizing67_empty_golden",2,named_packet_67_2,sizeof(named_packet_67_2),named_packet_65_2,sizeof(named_packet_65_2),named_packet_64_2,sizeof(named_packet_64_2),false},
    {"rune67_golden",3,named_packet_67_3,sizeof(named_packet_67_3),named_packet_65_3,sizeof(named_packet_65_3),named_packet_64_3,sizeof(named_packet_64_3),true},
    {"assert_condition67_golden",4,named_packet_67_4,sizeof(named_packet_67_4),named_packet_65_4,sizeof(named_packet_65_4),named_packet_64_4,sizeof(named_packet_64_4),true},
    {"assert_equal67_golden",5,named_packet_67_5,sizeof(named_packet_67_5),named_packet_65_5,sizeof(named_packet_65_5),named_packet_64_5,sizeof(named_packet_64_5),true},
    {"assert_panics67_golden",6,named_packet_67_6,sizeof(named_packet_67_6),named_packet_65_6,sizeof(named_packet_65_6),named_packet_64_6,sizeof(named_packet_64_6),true},
    {"atomic_types67_golden",7,named_packet_67_7,sizeof(named_packet_67_7),named_packet_65_7,sizeof(named_packet_65_7),named_packet_64_7,sizeof(named_packet_64_7),true},
    {"checked_scalar67_golden",8,named_packet_67_8,sizeof(named_packet_67_8),named_packet_65_8,sizeof(named_packet_65_8),named_packet_64_8,sizeof(named_packet_64_8),true},
    {"defaults67_golden_7",9,named_packet_67_9,sizeof(named_packet_67_9),named_packet_65_9,sizeof(named_packet_65_9),named_packet_64_9,sizeof(named_packet_64_9),true},
    {"defaults67_golden_8",10,named_packet_67_10,sizeof(named_packet_67_10),named_packet_65_10,sizeof(named_packet_65_10),named_packet_64_10,sizeof(named_packet_64_10),true},
    {"defaults67_golden_9",11,named_packet_67_11,sizeof(named_packet_67_11),named_packet_65_11,sizeof(named_packet_65_11),named_packet_64_11,sizeof(named_packet_64_11),true},
    {"defaults67_golden_10",12,named_packet_67_12,sizeof(named_packet_67_12),named_packet_65_12,sizeof(named_packet_65_12),named_packet_64_12,sizeof(named_packet_64_12),true},
    {"defaults67_invoke_golden",13,named_packet_67_13,sizeof(named_packet_67_13),named_packet_65_13,sizeof(named_packet_65_13),named_packet_64_13,sizeof(named_packet_64_13),true},
    {"generic_method67_golden",14,named_packet_67_14,sizeof(named_packet_67_14),named_packet_65_14,sizeof(named_packet_65_14),named_packet_64_14,sizeof(named_packet_64_14),true},
    {"library_string67_alpha",15,named_packet_67_15,sizeof(named_packet_67_15),named_packet_65_15,sizeof(named_packet_65_15),named_packet_64_15,sizeof(named_packet_64_15),true},
    {"library_string67_beta",16,named_packet_67_16,sizeof(named_packet_67_16),named_packet_65_16,sizeof(named_packet_65_16),named_packet_64_16,sizeof(named_packet_64_16),true},
    {"library_string67_nul",17,named_packet_67_17,sizeof(named_packet_67_17),named_packet_65_17,sizeof(named_packet_65_17),named_packet_64_17,sizeof(named_packet_64_17),true},
    {"library_string67_empty",18,named_packet_67_18,sizeof(named_packet_67_18),named_packet_65_18,sizeof(named_packet_65_18),named_packet_64_18,sizeof(named_packet_64_18),true},
    {"library_string67_long",19,named_packet_67_19,sizeof(named_packet_67_19),named_packet_65_19,sizeof(named_packet_65_19),named_packet_64_19,sizeof(named_packet_64_19),true},
    {"library_string67_equal",20,named_packet_67_20,sizeof(named_packet_67_20),named_packet_65_20,sizeof(named_packet_65_20),named_packet_64_20,sizeof(named_packet_64_20),true},
    {"library_string67_unused",21,named_packet_67_21,sizeof(named_packet_67_21),named_packet_65_21,sizeof(named_packet_65_21),named_packet_64_21,sizeof(named_packet_64_21),true},
    {"ordering67_golden",22,named_packet_67_22,sizeof(named_packet_67_22),named_packet_65_22,sizeof(named_packet_65_22),named_packet_64_22,sizeof(named_packet_64_22),true},
};
#endif
