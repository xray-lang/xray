/* Independent verifier for explicit source-level ownership consumes. */
#ifndef XI_SOURCE_MOVE_VERIFY_H
#define XI_SOURCE_MOVE_VERIFY_H

#include "xi.h"

typedef enum XiSourceMoveVerifyStatus {
    XI_SOURCE_MOVE_PASS = 0,
    XI_SOURCE_MOVE_VIOLATION,
    XI_SOURCE_MOVE_INTERNAL_ERROR,
} XiSourceMoveVerifyStatus;

typedef enum XiSourceMoveContract {
    XI_SOURCE_MOVE_CONTRACT_NONE = 0,
    XI_SOURCE_MOVE_C1_EVIDENCE,
    XI_SOURCE_MOVE_C2_DOMINANCE,
    XI_SOURCE_MOVE_C3_USE_AFTER_CONSUME,
    XI_SOURCE_MOVE_C4_TYPE,
    XI_SOURCE_MOVE_C5_RESOURCE,
} XiSourceMoveContract;

typedef struct XiSourceMoveVerifyReport {
    XiSourceMoveVerifyStatus status;
    XiSourceMoveContract contract;
    const XiFunc *func;
    const XiValue *move;
    const XiValue *use;
    char message[256];
} XiSourceMoveVerifyReport;

XR_FUNC XiSourceMoveVerifyStatus xi_source_move_verify(XiFunc *func,
                                                       XiSourceMoveVerifyReport *report);
XR_FUNC XiSourceMoveVerifyStatus xi_source_move_verify_tree(XiFunc *func,
                                                            XiSourceMoveVerifyReport *report);

#endif /* XI_SOURCE_MOVE_VERIFY_H */
