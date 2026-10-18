/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_access_source_cases.h - Genuine current Class rows and active access
 */
#ifndef XIR_CLASS_ACCESS_SOURCE_CASES_H
#define XIR_CLASS_ACCESS_SOURCE_CASES_H
static void class_access_source_cases(void) {
    static const CellSourceCase cases[]={
        {"class_access_worker_return_formal.xr",CELL_SOURCE_PROJECT_VALUE,41,XR_XIR_CALL_READY,false},
        {"class_access_worker_array_cow.xr",CELL_SOURCE_PROJECT_VALUE,4240,XR_XIR_CALL_READY,false},
        {"class_access_worker_carrier_unknown.xr",CELL_SOURCE_REJECT,0,XR_XIR_CALL_READY,false},
        {"class_access_same_owner.xr",CELL_SOURCE_PROJECT_REJECT,0,XR_XIR_CALL_BAD_ARGUMENT,false},
        {"class_access_worker_module_alias.xr",CELL_SOURCE_REJECT,0,XR_XIR_CALL_READY,false},
        {"class_access_worker_ref_array.xr",CELL_SOURCE_PROJECT_VALUE,42853,XR_XIR_CALL_READY,false}};
    for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        printf("Source Class current access %u: %s\n",i,cases[i].name);
        cell_source_case(&cases[i]);
    }
}
#endif // XIR_CLASS_ACCESS_SOURCE_CASES_H
