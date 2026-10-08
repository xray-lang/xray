/* Legal parent chains stay positive; blanket inheritance rejection does not qualify negatives. */
typedef struct ParentSourceCase { const char *name; XrXirStatus expected; unsigned fields, final, control; } ParentSourceCase;
static const ParentSourceCase parent_source_cases[] = {
    {"open_root_empty",0,0,0,1},
    {"open_root_fields",0,2,0,1},
    {"final_root_fields",0,2,1,1},
    {"parent_chain_fields",0,0,0,0},
    {"parent_chain_empty",0,0,0,0},
    {"self_parent",3,0,0,0},
    {"cycle",3,0,0,0},
    {"scalar_parent",1,0,0,0},
    {"record_parent",3,0,0,0},
    {"final_parent",3,0,0,0},
    {"missing_parent",3,0,0,0},
};
