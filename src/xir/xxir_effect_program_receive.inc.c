/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_program_receive.inc.c - Current owned Input and Final reception
 */
#include "xxir_layout_input_internal.h"
#include "xxir_effect_program_final_storage.inc.c"

/* Source templates have authentic specialization contexts; closed execution
 * inputs have no such owner. Both cases read the current Control input and
 * locally check declared errors, never a saved constructor result. */

/* Pure final projections follow every temporary CFG release. Root and
 * suspension evidence retain their independent receiving checks. */

/* ROOT correspondence observes the owned equations after receiving callbacks.
 * Independent control recomputation is confined to paranoid builds. */

/* The structural checker derives the same Cell proof through the sole owned
 * Input reader. The raw Lowered projection is matched again after callbacks. */
