/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef PRODUCT_FACTS_CONTROL_FIXTURE_H
#define PRODUCT_FACTS_CONTROL_FIXTURE_H
static size_t product_facts_directory_fixture(char *text,size_t capacity) {
    size_t used=0;
#define PF_APPEND(...) do {int count=snprintf(text+used,capacity-used,__VA_ARGS__);CHECK(count>=0&&(size_t)count<capacity-used);used+=(size_t)count;}while(0)
    PF_APPEND("@before_all\nfn prepare(){}\n@before_each\nfn before_case(){}\n");
    for(unsigned n=0;n<40;++n)PF_APPEND("@test\nfn case%u(){assertEqual(1+1,2)}\n",n);
    PF_APPEND("@test(skip)\nfn ");
    for(unsigned n=0;n<5000;++n)PF_APPEND("a");
    PF_APPEND("(){print(\"must not run\")}\n@test(timeout:7)\nfn timed(){}\n"
        "@after_each\nfn after_case(){}\n@after_all\nfn finish(){}\n"
        "export fn answer()->i64{return 42}\nprint(answer())\n");
#undef PF_APPEND
    CHECK(used==6746);return used;
}
#endif // PRODUCT_FACTS_CONTROL_FIXTURE_H
