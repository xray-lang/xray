/* A C++17 client owns real results after the C11 Program and Instance retire. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern "C" {
#include "xir/xxir_task_result.h"
}
#include "task_result_bridge.h"
#define CHECK(c) do {if(!(c)){std::fprintf(stderr,"C++ TaskResult %d: %s\n",__LINE__,#c);std::exit(1);}}while(0)
int main(int argc,char **argv) {
    CHECK(argc==6);XrXirValue first[4],copies[4];std::memset(first,0,sizeof(first));std::memset(copies,0,sizeof(copies));
    const char *names[4]={argv[1],argv[2],argv[3],argv[4]};const XrXirTypes *types=nullptr;TestTaskResultOwner *owner=nullptr;
    test_task_result_produce(names,argv[5],first,&types,&owner);CHECK(types && owner);
    for(unsigned i=0;i<4;++i) {
        XrXirTaskResultType shape;std::memset(&shape,0,sizeof(shape));
        CHECK(xr_xir_task_result_type_info(types,static_cast<XrXirType>(first[i].type),&shape));
        CHECK(shape.type==static_cast<XrXirType>(first[i].type) && xr_xir_value_copy(&first[i],&copies[i])==XR_XIR_VALUE_OK);
        xr_xir_value_drop(&first[i]);CHECK(xr_xir_value_valid(&copies[i]));
    }
    test_task_result_observe(copies);
    for(unsigned i=0;i<4;++i)xr_xir_value_drop(&copies[i]);test_task_result_finish(owner);
    std::puts("C++17 TaskResult: new header, type_info and detached four-state ownership PASS");return 0;
}
