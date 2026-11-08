/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_inventory.inc.c - Actual callable basis and distinct environment identities
 *
 * KEY CONCEPT:
 *   The complete current Input defines the site basis and public root spaces.
 *   Owned key storage is framed before comparison; duplicate keys never add an environment.
 */
#include "xxir_effect_final_input_select.inc.c"

#include "xxir_effect_capture_global.inc.c"
#include "xxir_effect_invocation_final_view.inc.c"
