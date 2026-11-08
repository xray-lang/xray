/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * Same-operation complete correspondence precedes shared immutable ownership.
 */


/* Only the immutable backbone is shared. Both existing equation creation
 * and publication replace declared bounds, so every generation owns a fresh
 * complete overlay before either writer can run. No child field is writable
 * through this continuation except the holder's own bookkeeping and bounds. */

/* A positive difference is handled by the original unique generation writer.
 * Malformed storage and OOM/BUDGET are errors, never reasons to rebuild. The
 * complete current ordinary prior has already run in generation_input. */
