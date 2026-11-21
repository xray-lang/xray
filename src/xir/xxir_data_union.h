/* Finite closed data unions. Error unions retain their independent contract. */
#ifndef XXIR_DATA_UNION_H
#define XXIR_DATA_UNION_H
#include "xxir.h"
#include "../base/xcompile_request.h"
/* Members are canonical: strictly increasing by type identity, duplicate-free,
 * with at least two entries. A one-member spelling normalizes to that member.
 * The vector is owned by the compile context's resources; the caller frees it
 * with xr_compile_resources_free after interning the result. */
typedef struct XrXirDataUnion {
    XrXirType *members;
    uint32_t count;
} XrXirDataUnion;
/* Bounded first carrier: scalar/string atoms, closed and nonrecursive. */
XR_FUNC bool xr_xir_data_union_atom(XrXirType);
XR_FUNC bool xr_xir_data_union_shape(const XrXirTypes *,XrXirType);
XR_FUNC bool xr_xir_data_union_contains(const XrXirTypes *,XrXirType,XrXirType);
/* Ordinal of an exact member, i.e. the synthesized enum's variant index. */
XR_FUNC bool xr_xir_data_union_member_index(const XrXirTypes *,XrXirType,XrXirType,uint32_t *);
/* True when every member is one Class identity, so the value is one nominal
 * handle and the runtime class identity is its discriminant (no stored tag). */
XR_FUNC bool xr_xir_data_union_all_classes(const XrXirTypes *,XrXirType);
/* All facts are returned by value. Failure leaves output unchanged. */
XR_FUNC XrXirStatus xr_xir_compile_data_union(const XrXirCompileContext *,
    const XrXirTypes *,const XrXirType *,uint32_t,XrCompileRequestControl *,XrXirDataUnion *);
/* Auto-wrap: the value's static type must be exactly one member. There is no
 * implicit numeric conversion and no subclass upcast. Literal member selection
 * happens before this typed-expression relation, in the literal checker. */
XR_FUNC XrXirStatus xr_xir_data_union_select(const XrXirTypes *,XrXirType,XrXirType,XrXirType *);
#endif
