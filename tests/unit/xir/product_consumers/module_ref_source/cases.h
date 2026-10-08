/* Positive module-ref source remains a positive obligation. */
typedef struct ModuleRefSourceCase { const char *name; XrXirStatus expected; } ModuleRefSourceCase;
static const ModuleRefSourceCase module_ref_source_cases[] = {
    {"local_scalar", 0},
    {"local_array", 0},
    {"root_scalar", 0},
    {"root_array", 0},
    {"imported_scalar", 0},
    {"root_const", 3},
    {"local_const", 3},
    {"local_missing_ref", 3},
    {"local_alias", 3},
};
