/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef IMPLEMENTATION_CONTROL_FIXTURE_H
#define IMPLEMENTATION_CONTROL_FIXTURE_H
/* Preserve the complete generic implementation used by the CLI baseline. */
static size_t implementation_control_fixture(char *text,size_t capacity) {
    size_t used=0;
#define IC_APPEND(...) do {int count=snprintf(text+used,capacity-used,__VA_ARGS__);CHECK(count>=0&&(size_t)count<capacity-used);used+=(size_t)count;}while(0)
    IC_APPEND("interface Reader<T>{\n");
    for(unsigned i=0;i<192;++i)IC_APPEND("value%u()->T\n",i);
    IC_APPEND("}\nstruct Box implements Reader<i64>{\n");
    for(unsigned i=0;i<192;++i)IC_APPEND("value%u()->i64{return %u}\n",i,i==0?41u:i==191?1u:0u);
    IC_APPEND("}\nfn read<T:Reader<i64>>(value:T)->i64{return value.value0()+value.value191()}\n"
        "export fn answer()->i64{return read(Box{})}\nprint(answer())\n");
#undef IC_APPEND
    CHECK(used==7658);return used;
}
#endif // IMPLEMENTATION_CONTROL_FIXTURE_H
