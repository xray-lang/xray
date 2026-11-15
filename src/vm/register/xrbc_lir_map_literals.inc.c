/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * The bytecode program owns every provenance byte before LIR can retire. */
static XrRbcCompileStatus rbc_lir_map_literals(const XrLirModule *input,XrRbcProgram *program,XrRbcCompileDiagnostic *diagnostic) {
    uint32_t count=xr_lir_map_literal_count(input);
    if(!count)return XR_RBC_COMPILE_OK;
    if(sizeof(XrtMapLiteral)>SIZE_MAX/count)return rbc_lir_error(diagnostic,XR_RBC_COMPILE_LIMIT,
        "Map provenance table exceeds address space",(XrLirSource){0});
    XrtMapLiteral *literals=xr_calloc(count,sizeof(*literals));
    if(!literals)return rbc_lir_error(diagnostic,XR_RBC_COMPILE_OUT_OF_MEMORY,
        "cannot own Map provenance table",(XrLirSource){0});
    program->module.map_literals=literals;program->module.map_literal_count=count;
    for(uint32_t n=0;n<count;++n) {
        XrLirMapLiteral source;
        if(xr_lir_map_literal_view(input,n+1,&source)!=XR_LIR_OK)return rbc_lir_error(diagnostic,XR_RBC_COMPILE_INVALID,
            "Map provenance has no readable owner",(XrLirSource){0});
        char *path=source.path_length?xr_malloc(source.path_length):NULL;
        if(source.path_length && !path)return rbc_lir_error(diagnostic,XR_RBC_COMPILE_OUT_OF_MEMORY,
            "cannot own Map source path",(XrLirSource){0});
        literals[n].path=path;literals[n].path_length=source.path_length;
        if(source.path_length)memcpy(path,source.path,source.path_length);
        if(source.count) {
            if(sizeof(XrtMapLiteralOrigin)>SIZE_MAX/source.count)return rbc_lir_error(diagnostic,XR_RBC_COMPILE_LIMIT,
                "Map provenance items exceed address space",(XrLirSource){0});
            XrtMapLiteralOrigin *items=xr_malloc((size_t)source.count*sizeof(*items));
            if(!items)return rbc_lir_error(diagnostic,XR_RBC_COMPILE_OUT_OF_MEMORY,
                "cannot own Map source positions",(XrLirSource){0});
            literals[n].items=items;literals[n].count=source.count;
            for(uint32_t p=0;p<source.count;++p)items[p]=(XrtMapLiteralOrigin){source.items[p].kind,source.items[p].line,source.items[p].column};
        }
    }
    return XR_RBC_COMPILE_OK;
}
