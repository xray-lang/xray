/* Independent Source arithmetic and output; no measured expected values. */
#ifndef XIR_LIBRARY_VIEWS_NATIVE_ORACLES_H
#define XIR_LIBRARY_VIEWS_NATIVE_ORACLES_H
typedef struct RootParameterSourceOracle {
    const char *file, *text;
    bool root, unresolved;
    int64_t result;
    const char *output;
    size_t output_bytes;
    uint32_t modules, index;
} RootParameterSourceOracle;
static const RootParameterSourceOracle rps_oracles[] = {
    {"diamond",NULL,false,false,41,"41\n",3,5,0},
    {"leaf",NULL,false,false,41,"41\n",3,2,1},
    {"disjoint",NULL,false,false,41,"41\n",3,6,2},
    {"reverse",NULL,false,false,41,"41\n",3,6,3},
    {"chain17",NULL,false,false,41,"41\n",3,18,4}
};
#endif
