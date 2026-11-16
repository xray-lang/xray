/* Finite closed data unions. Error unions retain their independent contract. */
#ifndef XXIR_DATA_UNION_H
#define XXIR_DATA_UNION_H
#include "xxir.h"
#include "../base/xcompile_request.h"
#define XR_XIR_DATA_UNION_MAX_MEMBERS 6u
typedef struct XrXirDataUnion {
    XrXirType members[XR_XIR_DATA_UNION_MAX_MEMBERS];
    uint32_t count;
} XrXirDataUnion;
/* Bounded first carrier: scalar/string atoms, closed and nonrecursive. */
XR_FUNC bool xr_xir_data_union_atom(XrXirType);
XR_FUNC bool xr_xir_data_union_shape(const XrXirTypes *,XrXirType);
XR_FUNC bool xr_xir_data_union_contains(const XrXirTypes *,XrXirType,XrXirType);
/* All facts are returned by value. Failure leaves output unchanged. */
XR_FUNC XrXirStatus xr_xir_compile_data_union(const XrXirCompileContext *,
    const XrXirTypes *,const XrXirType *,uint32_t,XrCompileRequestControl *,XrXirDataUnion *);
/* Exact member first, otherwise a unique existing implicit numeric widening.
 * Literals select their numeric context before this typed-expression relation. */
XR_FUNC XrXirStatus xr_xir_data_union_select(const XrXirTypes *,XrXirType,XrXirType,XrXirType *);
#endif
