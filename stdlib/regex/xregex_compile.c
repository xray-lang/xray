/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xregex_compile.c - Regular expression compiler
 *
 * KEY CONCEPT:
 *   Compiles AST (Abstract Syntax Tree) into executable instruction sequence (Prog).
 *   Uses Thompson construction algorithm to generate NFA instructions.
 */

#include "xregex_internal.h"

/* ========================================================================
 * Compiler State
 * ======================================================================== */

typedef struct {
    XrProg *prog;
    XrRegexFlags flags;
    int capture_count;
} XrCompiler;

/* ========================================================================
 * Instruction Allocation
 * ======================================================================== */

// Allocate a new instruction, return instruction index
static int alloc_inst(XrCompiler *c) {
    XrProg *prog = c->prog;

    if (prog->inst_count >= prog->inst_cap) {
        int new_cap = prog->inst_cap ? prog->inst_cap * 2 : 64;
        if (new_cap > XR_RE_MAX_PROG_INST) {
            return -1;  // too large
        }
        XR_REALLOC_OR_ABORT(prog->inst, (size_t) new_cap * sizeof(XrInst), "regex prog inst grow");
        prog->inst_cap = new_cap;
    }

    int id = prog->inst_count++;
    memset(&prog->inst[id], 0, sizeof(XrInst));
    return id;
}

// Set instruction
static void set_inst(XrCompiler *c, int id, XrOpcode op, int out) {
    XR_INST_SET(&c->prog->inst[id], op, out, 0);
}

// Create NOP instruction (for placeholder, fill later)
static int emit_nop(XrCompiler *c) {
    int id = alloc_inst(c);
    if (id < 0)
        return -1;
    set_inst(c, id, XR_OP_NOP, 0);
    return id;
}

// Create MATCH instruction
static int emit_match(XrCompiler *c) {
    int id = alloc_inst(c);
    if (id < 0)
        return -1;
    set_inst(c, id, XR_OP_MATCH, 0);
    return id;
}

// Create BYTE instruction
static int emit_byte(XrCompiler *c, uint8_t byte, int out) {
    int id = alloc_inst(c);
    if (id < 0)
        return -1;
    set_inst(c, id, XR_OP_BYTE, out);
    c->prog->inst[id].byte = byte;
    return id;
}

// Create BYTE_RANGE instruction
static int emit_byte_range(XrCompiler *c, uint8_t lo, uint8_t hi, int out) {
    int id = alloc_inst(c);
    if (id < 0)
        return -1;
    set_inst(c, id, XR_OP_BYTE_RANGE, out);
    c->prog->inst[id].lo = lo;
    c->prog->inst[id].hi = hi;
    return id;
}

// Create ANY_BYTE instruction
static int emit_any_byte(XrCompiler *c, bool dotall, int out) {
    int id = alloc_inst(c);
    if (id < 0)
        return -1;
    set_inst(c, id, dotall ? XR_OP_ANY_BYTE_NL : XR_OP_ANY_BYTE, out);
    return id;
}

// Create ALT instruction
static int emit_alt(XrCompiler *c, int out, int out1) {
    int id = alloc_inst(c);
    if (id < 0)
        return -1;
    set_inst(c, id, XR_OP_ALT, out);
    c->prog->inst[id].out1 = out1;
    return id;
}

// Create CAPTURE instruction
static int emit_capture(XrCompiler *c, int cap, int out) {
    int id = alloc_inst(c);
    if (id < 0)
        return -1;
    set_inst(c, id, XR_OP_CAPTURE, out);
    c->prog->inst[id].cap = cap;
    return id;
}

// Create EMPTY_WIDTH instruction
static int emit_empty_width(XrCompiler *c, uint32_t flags, int out) {
    int id = alloc_inst(c);
    if (id < 0)
        return -1;
    set_inst(c, id, XR_OP_EMPTY_WIDTH, out);
    c->prog->inst[id].empty = flags;
    return id;
}

// Allocate Unicode property slot
static int alloc_unicode_prop(XrCompiler *c, uint32_t prop_id, bool negated) {
    XrProg *prog = c->prog;

    if (prog->unicode_range_count >= prog->unicode_range_cap) {
        int new_cap = prog->unicode_range_cap ? prog->unicode_range_cap * 2 : 16;
        XR_REALLOC_OR_ABORT(prog->unicode_ranges, (size_t) new_cap * sizeof(XrProgUnicodeRange),
                            "regex unicode_ranges grow");
        prog->unicode_range_cap = new_cap;
    }

    int idx = prog->unicode_range_count++;
    prog->unicode_ranges[idx].prop_id = prop_id;
    prog->unicode_ranges[idx].negated = negated;
    return idx;
}

// Create UNICODE_RANGE instruction
static int emit_unicode_prop(XrCompiler *c, uint32_t prop_id, bool negated, int out) {
    int prop_idx = alloc_unicode_prop(c, prop_id, negated);
    if (prop_idx < 0)
        return -1;

    int id = alloc_inst(c);
    if (id < 0)
        return -1;
    set_inst(c, id, XR_OP_UNICODE_RANGE, out);
    c->prog->inst[id].unicode_idx = prop_idx;
    return id;
}

/* ========================================================================
 * Compile Fragment
 *
 * A fragment is a continuous sequence of instructions with one entry and
 * several exits (to be filled). Uses Thompson construction to combine fragments.
 * ======================================================================== */

typedef struct {
    int start;   // entry instruction index
    int *patch;  // list of out pointers to be filled
    int patch_count;
    int patch_cap;
} XrFragment;

static void frag_init(XrFragment *f, int start) {
    f->start = start;
    f->patch = NULL;
    f->patch_count = 0;
    f->patch_cap = 0;
}

static void frag_free(XrFragment *f) {
    xr_re_free(f->patch);
    f->patch = NULL;
    f->patch_count = 0;
}

/*
 * Add a patch location
 * @param inst_id instruction index
 * @param is_out1 whether to fill out1 (for ALT)
 */
static void frag_add_patch(XrFragment *f, int inst_id, bool is_out1) {
    if (f->patch_count >= f->patch_cap) {
        int new_cap = f->patch_cap ? f->patch_cap * 2 : 4;
        XR_REALLOC_OR_ABORT(f->patch, (size_t) new_cap * sizeof(int), "regex fragment patch grow");
        f->patch_cap = new_cap;
    }
    // Encoding: positive for out, negative for out1 (offset by 1 to avoid 0 ambiguity)
    f->patch[f->patch_count++] = is_out1 ? -(inst_id + 1) : inst_id;
}

// Connect all patch locations to target
static void frag_patch_to(XrFragment *f, XrCompiler *c, int target) {
    for (int i = 0; i < f->patch_count; i++) {
        int p = f->patch[i];
        if (p >= 0) {
            // Fill out
            int id = p;
            int op = XR_INST_OP(&c->prog->inst[id]);
            XR_INST_SET(&c->prog->inst[id], op, target, 0);
        } else {
            // Fill out1
            int id = -(p + 1);
            c->prog->inst[id].out1 = target;
        }
    }
}

// Merge patch lists of two fragments
static void frag_merge(XrFragment *dst, XrFragment *src) {
    for (int i = 0; i < src->patch_count; i++) {
        if (dst->patch_count >= dst->patch_cap) {
            int new_cap = dst->patch_cap ? dst->patch_cap * 2 : 4;
            XR_REALLOC_OR_ABORT(dst->patch, (size_t) new_cap * sizeof(int),
                                "regex fragment patch merge grow");
            dst->patch_cap = new_cap;
        }
        dst->patch[dst->patch_count++] = src->patch[i];
    }
}

/* ========================================================================
 * AST Compilation
 * ======================================================================== */

static bool compile_node(XrCompiler *c, XrAstNode *node, XrFragment *frag);

/*
 * Emit byte instruction (with ignorecase support)
 * For letters: return ALT node ID, return two BYTE node IDs via lo_out/hi_out
 * For non-letters: return BYTE node ID, lo_out/hi_out set to -1
 */
static int emit_byte_ignorecase_ex(XrCompiler *c, uint8_t byte, int out, int *lo_out, int *hi_out) {
    // Check if it's a letter
    if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z')) {
        // Convert to case-insensitive character class
        uint8_t lower = (byte >= 'A' && byte <= 'Z') ? (byte + 32) : byte;
        uint8_t upper = (byte >= 'a' && byte <= 'z') ? (byte - 32) : byte;

        // Create two branches: match uppercase or lowercase
        int lo_id = emit_byte(c, lower, out);
        int hi_id = emit_byte(c, upper, out);
        if (lo_id < 0 || hi_id < 0)
            return -1;

        if (lo_out)
            *lo_out = lo_id;
        if (hi_out)
            *hi_out = hi_id;

        // Connect with ALT
        return emit_alt(c, lo_id, hi_id);
    }

    // Non-letter: return BYTE node directly
    int id = emit_byte(c, byte, out);
    if (lo_out)
        *lo_out = -1;
    if (hi_out)
        *hi_out = -1;
    return id;
}

/*
 * Compile literal
 * For ignorecase mode, need to correctly link all BYTE nodes under ALT
 */
static bool compile_literal(XrCompiler *c, XrAstNode *node, XrFragment *frag) {
    const char *str = node->literal.str;
    int len = node->literal.len;
    bool ignorecase = (c->flags & XR_RE_IGNORECASE) != 0;

    if (len == 0) {
        // Empty literal
        int id = emit_nop(c);
        frag_init(frag, id);
        frag_add_patch(frag, id, false);
        return id >= 0;
    }

    // Store previous character's BYTE nodes (may have two, for ignorecase)
    int prev_bytes[2] = {-1, -1};
    int prev_byte_count = 0;
    int first = -1;

    for (int i = 0; i < len; i++) {
        int id;
        int lo_id = -1, hi_id = -1;

        if (ignorecase) {
            id = emit_byte_ignorecase_ex(c, (uint8_t) str[i], 0, &lo_id, &hi_id);
        } else {
            id = emit_byte(c, (uint8_t) str[i], 0);
        }
        if (id < 0) {
            frag_free(frag);
            return false;
        }

        if (i == 0) {
            first = id;
            frag_init(frag, first);
        } else {
            // Connect all BYTE nodes of previous character to current node
            for (int j = 0; j < prev_byte_count; j++) {
                int prev_id = prev_bytes[j];
                int op = XR_INST_OP(&c->prog->inst[prev_id]);
                XR_INST_SET(&c->prog->inst[prev_id], op, id, 0);
            }
        }

        // Update prev_bytes
        if (lo_id >= 0 && hi_id >= 0) {
            // ignorecase letter: has two BYTE nodes
            prev_bytes[0] = lo_id;
            prev_bytes[1] = hi_id;
            prev_byte_count = 2;
        } else {
            // Non-letter or non-ignorecase: only one node
            prev_bytes[0] = id;
            prev_byte_count = 1;
        }
    }

    // Add all BYTE nodes of last character as patch points
    for (int j = 0; j < prev_byte_count; j++) {
        frag_add_patch(frag, prev_bytes[j], false);
    }

    return true;
}

/*
 * Helper function: expand character ranges for ignorecase support
 * Expand ranges containing letters to include both cases
 */
static int expand_ranges_ignorecase(XrCharRange *ranges, int count, XrCharRange *out, int max_out) {
    int out_count = 0;

    for (int i = 0; i < count && out_count < max_out; i++) {
        uint8_t lo = ranges[i].lo;
        uint8_t hi = ranges[i].hi;

        // Add original range
        out[out_count].lo = lo;
        out[out_count].hi = hi;
        out_count++;

        // Check if contains lowercase letters, add corresponding uppercase range
        if (lo >= 'a' && hi <= 'z' && out_count < max_out) {
            out[out_count].lo = lo - 32;  // 'a' - 'A' = 32
            out[out_count].hi = hi - 32;
            out_count++;
        }
        // Check if contains uppercase letters, add corresponding lowercase range
        else if (lo >= 'A' && hi <= 'Z' && out_count < max_out) {
            out[out_count].lo = lo + 32;
            out[out_count].hi = hi + 32;
            out_count++;
        }
        // Handle cases that cross case boundaries
        else if (lo <= 'z' && hi >= 'a' && out_count < max_out) {
            // Range contains some lowercase letters
            uint8_t lower_start = (lo > 'a') ? lo : 'a';
            uint8_t lower_end = (hi < 'z') ? hi : 'z';
            if (lower_start <= lower_end) {
                out[out_count].lo = lower_start - 32;
                out[out_count].hi = lower_end - 32;
                out_count++;
            }
        } else if (lo <= 'Z' && hi >= 'A' && out_count < max_out) {
            // Range contains some uppercase letters
            uint8_t upper_start = (lo > 'A') ? lo : 'A';
            uint8_t upper_end = (hi < 'Z') ? hi : 'Z';
            if (upper_start <= upper_end) {
                out[out_count].lo = upper_start + 32;
                out[out_count].hi = upper_end + 32;
                out_count++;
            }
        }
    }

    return out_count;
}

// Compile character class
static bool compile_char_class(XrCompiler *c, XrAstNode *node, XrFragment *frag) {
    XrCharRange *ranges = node->cc.ranges;
    int count = node->cc.count;
    bool has_unicode = node->cc.has_unicode_prop;
    uint32_t unicode_prop = node->cc.unicode_prop;
    bool negated = node->cc.negated;
    bool ignorecase = (c->flags & XR_RE_IGNORECASE) != 0;

    // If ignorecase mode, expand ranges
    XrCharRange expanded_ranges[128];
    if (ignorecase && count > 0 && !has_unicode) {
        count = expand_ranges_ignorecase(ranges, count, expanded_ranges, 128);
        ranges = expanded_ranges;
    }

    if (count == 0 && !has_unicode) {
        // Empty character class: if negated match any, otherwise match nothing
        if (negated) {
            int id = emit_any_byte(c, false, 0);
            frag_init(frag, id);
            frag_add_patch(frag, id, false);
            return id >= 0;
        } else {
            // Empty character class [] never matches
            int id = alloc_inst(c);
            if (id < 0)
                return false;
            set_inst(c, id, XR_OP_FAIL, 0);
            frag_init(frag, id);
            return true;
        }
    }

    // Handle Unicode property character class
    if (has_unicode) {
        int id = emit_unicode_prop(c, unicode_prop, negated, 0);
        if (id < 0)
            return false;
        frag_init(frag, id);
        frag_add_patch(frag, id, false);
        return true;
    }

    if (!negated) {
        // Non-negated: connect all ranges with ALT
        if (count == 1 && ranges[0].lo == ranges[0].hi) {
            // Single character optimization
            int id = emit_byte(c, ranges[0].lo, 0);
            frag_init(frag, id);
            frag_add_patch(frag, id, false);
            return id >= 0;
        }

        // Create first range
        int first = emit_byte_range(c, ranges[0].lo, ranges[0].hi, 0);
        if (first < 0)
            return false;

        frag_init(frag, first);
        frag_add_patch(frag, first, false);

        // Connect other ranges with ALT
        for (int i = 1; i < count; i++) {
            int range_id = emit_byte_range(c, ranges[i].lo, ranges[i].hi, 0);
            if (range_id < 0) {
                frag_free(frag);
                return false;
            }
            int alt_id = emit_alt(c, frag->start, range_id);
            if (alt_id < 0) {
                frag_free(frag);
                return false;
            }
            frag->start = alt_id;
            frag_add_patch(frag, range_id, false);
        }

        return true;
    } else {
        // Negated character class: sort-merge then compute complement ranges
        // Sort by lo (insertion sort — count is small)
        for (int si = 1; si < count; si++) {
            XrCharRange tmp = ranges[si];
            int sj = si - 1;
            while (sj >= 0 && ranges[sj].lo > tmp.lo) {
                ranges[sj + 1] = ranges[sj];
                sj--;
            }
            ranges[sj + 1] = tmp;
        }
        // Merge overlapping/adjacent ranges in-place
        int merged = 0;
        for (int mi = 0; mi < count; mi++) {
            if (merged > 0 && ranges[mi].lo <= ranges[merged - 1].hi + 1) {
                if (ranges[mi].hi > ranges[merged - 1].hi)
                    ranges[merged - 1].hi = ranges[mi].hi;
            } else {
                ranges[merged++] = ranges[mi];
            }
        }
        // Build complement from gaps between merged ranges
        XrFragment result;
        frag_init(&result, -1);
        int prev = 0;
        for (int gi = 0; gi <= merged; gi++) {
            int gap_lo = prev;
            int gap_hi = (gi < merged) ? ranges[gi].lo - 1 : 255;
            if (gi < merged)
                prev = ranges[gi].hi + 1;
            if (gap_lo > gap_hi || gap_lo > 255)
                continue;
            int rid = emit_byte_range(c, (uint8_t) gap_lo, (uint8_t) gap_hi, 0);
            if (rid < 0) {
                frag_free(&result);
                return false;
            }
            if (result.start < 0) {
                result.start = rid;
                frag_add_patch(&result, rid, false);
            } else {
                int aid = emit_alt(c, result.start, rid);
                if (aid < 0) {
                    frag_free(&result);
                    return false;
                }
                result.start = aid;
                frag_add_patch(&result, rid, false);
            }
        }

        if (result.start < 0) {
            // Complement is empty (original is full set), match fails
            int id = alloc_inst(c);
            if (id < 0)
                return false;
            set_inst(c, id, XR_OP_FAIL, 0);
            frag_init(frag, id);
            return true;
        }

        *frag = result;
        return true;
    }
}

// Compile any character .
static bool compile_any(XrCompiler *c, XrAstNode *node, XrFragment *frag) {
    int id = emit_any_byte(c, node->any_dotall, 0);
    if (id < 0)
        return false;
    frag_init(frag, id);
    frag_add_patch(frag, id, false);
    return true;
}

// Compile zero-width assertion
static bool compile_empty_match(XrCompiler *c, XrAstNode *node, XrFragment *frag) {
    int id = emit_empty_width(c, node->empty_flags, 0);
    if (id < 0)
        return false;
    frag_init(frag, id);
    frag_add_patch(frag, id, false);
    return true;
}

// Compile concatenation
static bool compile_concat(XrCompiler *c, XrAstNode *node, XrFragment *frag) {
    int count = node->list.count;

    if (count == 0) {
        int id = emit_nop(c);
        frag_init(frag, id);
        frag_add_patch(frag, id, false);
        return id >= 0;
    }

    // Compile first child node
    if (!compile_node(c, node->list.children[0], frag)) {
        return false;
    }

    // Save first child node's entry
    int first_start = frag->start;

    // Connect subsequent child nodes
    for (int i = 1; i < count; i++) {
        XrFragment next;
        if (!compile_node(c, node->list.children[i], &next)) {
            frag_free(frag);
            return false;
        }

        // Connect all exits of frag to next's entry
        frag_patch_to(frag, c, next.start);

        // Update frag's exits to next's exits, but keep entry
        frag_free(frag);
        frag->patch = next.patch;
        frag->patch_count = next.patch_count;
        frag->patch_cap = next.patch_cap;
    }

    // Restore first child node's entry
    frag->start = first_start;

    return true;
}

// Compile alternation a|b
static bool compile_alt(XrCompiler *c, XrAstNode *node, XrFragment *frag) {
    int count = node->list.count;

    if (count == 0) {
        int id = emit_nop(c);
        frag_init(frag, id);
        frag_add_patch(frag, id, false);
        return id >= 0;
    }

    // Compile first branch
    if (!compile_node(c, node->list.children[0], frag)) {
        return false;
    }

    // Add other branches
    for (int i = 1; i < count; i++) {
        XrFragment branch;
        if (!compile_node(c, node->list.children[i], &branch)) {
            frag_free(frag);
            return false;
        }

        // Create ALT instruction
        int alt_id = emit_alt(c, frag->start, branch.start);
        if (alt_id < 0) {
            frag_free(frag);
            frag_free(&branch);
            return false;
        }

        // Merge exit lists
        frag_merge(frag, &branch);
        frag_free(&branch);

        // New entry is ALT
        frag->start = alt_id;
    }

    return true;
}

// Compile repeat a{n,m}
static bool compile_repeat(XrCompiler *c, XrAstNode *node, XrFragment *frag) {
    XrAstNode *child = node->repeat.child;
    int min = node->repeat.min;
    int max = node->repeat.max;
    bool greedy = node->repeat.greedy;

    // If has non-greedy quantifier, set flag
    if (!greedy) {
        c->prog->flags |= XR_RE_UNGREEDY;
    }

    // Special cases
    if (max == 0) {
        // a{0} equals empty
        int id = emit_nop(c);
        frag_init(frag, id);
        frag_add_patch(frag, id, false);
        return id >= 0;
    }

    frag_init(frag, -1);
    XrFragment prev = {0};
    bool has_prev = false;
    int first_start = -1;  // Save first child node's entry

    // Expand min required matches
    for (int i = 0; i < min; i++) {
        XrFragment copy;
        if (!compile_node(c, child, &copy)) {
            if (has_prev)
                frag_free(&prev);
            return false;
        }

        if (!has_prev) {
            prev = copy;
            has_prev = true;
            first_start = copy.start;  // Record first entry
        } else {
            frag_patch_to(&prev, c, copy.start);
            frag_free(&prev);
            prev = copy;
        }
    }

    if (max < 0) {
        // a{n,} unlimited repeat
        XrFragment loop;
        if (!compile_node(c, child, &loop)) {
            if (has_prev)
                frag_free(&prev);
            return false;
        }

        // Create loop: ALT(loop, out)
        int nop = emit_nop(c);
        int alt = emit_alt(c, greedy ? loop.start : nop, greedy ? nop : loop.start);
        if (alt < 0 || nop < 0) {
            frag_free(&loop);
            if (has_prev)
                frag_free(&prev);
            return false;
        }

        // loop's exit back to ALT
        frag_patch_to(&loop, c, alt);
        frag_free(&loop);

        if (has_prev) {
            frag_patch_to(&prev, c, alt);
            frag_free(&prev);
            frag->start = first_start;
        } else {
            frag->start = alt;
        }

        frag_add_patch(frag, nop, false);

    } else if (max > min) {
        // a{n,m} expand optional part
        bool need_set_start = (first_start < 0);  // If min=0, start not set yet

        for (int i = min; i < max; i++) {
            XrFragment opt;
            if (!compile_node(c, child, &opt)) {
                if (has_prev)
                    frag_free(&prev);
                frag_free(frag);
                return false;
            }

            int nop = emit_nop(c);
            int alt = emit_alt(c, greedy ? opt.start : nop, greedy ? nop : opt.start);
            if (alt < 0 || nop < 0) {
                frag_free(&opt);
                if (has_prev)
                    frag_free(&prev);
                frag_free(frag);
                return false;
            }

            if (has_prev) {
                frag_patch_to(&prev, c, alt);
                frag_free(&prev);
            }

            if (need_set_start) {
                frag->start = alt;
                need_set_start = false;
            }

            frag_add_patch(frag, nop, false);
            prev = opt;
            has_prev = true;
        }

        // Set final start (use first_start if min > 0)
        if (first_start >= 0) {
            frag->start = first_start;
        }

        // Last exit
        if (has_prev) {
            frag_merge(frag, &prev);
            frag_free(&prev);
        }

    } else {
        // a{n} exact match
        if (has_prev) {
            // Use prev's exits, but use first_start as entry
            frag->patch = prev.patch;
            frag->patch_count = prev.patch_count;
            frag->patch_cap = prev.patch_cap;
            frag->start = first_start;
        } else {
            int id = emit_nop(c);
            frag_init(frag, id);
            frag_add_patch(frag, id, false);
        }
    }

    return true;
}

/*
 * Compile capture group
 *
 * caps array layout:
 *   caps[0], caps[1] = full match (set by NFA)
 *   caps[2], caps[3] = first capture group
 *   caps[4], caps[5] = second capture group
 *   ...
 */
static bool compile_capture(XrCompiler *c, XrAstNode *node, XrFragment *frag) {
    int index = node->capture.index;

    // Start capture (+1 because caps[0,1] is full match)
    int cap_start_idx = (index + 1) * 2;
    int start_cap = emit_capture(c, cap_start_idx, 0);
    if (start_cap < 0)
        return false;

    // Compile child node
    XrFragment child_frag;
    if (!compile_node(c, node->capture.child, &child_frag)) {
        return false;
    }

    // End capture
    int cap_end_idx = (index + 1) * 2 + 1;
    int end_cap = emit_capture(c, cap_end_idx, 0);
    if (end_cap < 0) {
        frag_free(&child_frag);
        return false;
    }

    // Connect: start_cap -> child -> end_cap
    set_inst(c, start_cap, XR_OP_CAPTURE, child_frag.start);
    frag_patch_to(&child_frag, c, end_cap);
    frag_free(&child_frag);

    frag_init(frag, start_cap);
    frag_add_patch(frag, end_cap, false);

    if (index >= c->capture_count) {
        c->capture_count = index + 1;
    }

    return true;
}

// Compile empty node
static bool compile_empty(XrCompiler *c, XrFragment *frag) {
    int id = emit_nop(c);
    if (id < 0)
        return false;
    frag_init(frag, id);
    frag_add_patch(frag, id, false);
    return true;
}

// Compile any AST node
static bool compile_node(XrCompiler *c, XrAstNode *node, XrFragment *frag) {
    switch (node->type) {
        case XR_AST_EMPTY:
            return compile_empty(c, frag);
        case XR_AST_LITERAL:
            return compile_literal(c, node, frag);
        case XR_AST_CHAR_CLASS:
            return compile_char_class(c, node, frag);
        case XR_AST_ANY:
            return compile_any(c, node, frag);
        case XR_AST_CONCAT:
            return compile_concat(c, node, frag);
        case XR_AST_ALT:
            return compile_alt(c, node, frag);
        case XR_AST_REPEAT:
            return compile_repeat(c, node, frag);
        case XR_AST_CAPTURE:
            return compile_capture(c, node, frag);
        case XR_AST_EMPTY_MATCH:
            return compile_empty_match(c, node, frag);
        default:
            return false;
    }
}

/* ========================================================================
 * Public Interface
 * ======================================================================== */

// Forward declaration
static void xr_prog_analyze(XrProg *prog, XrAstNode *ast);

XR_FUNC XrProg *xr_regex_compile_prog(XrAstNode *ast, XrRegexFlags flags) {
    XrProg *prog = (XrProg *) xr_re_alloc(sizeof(XrProg));
    memset(prog, 0, sizeof(XrProg));
    prog->flags = flags;

    XrCompiler c;
    c.prog = prog;
    c.flags = flags;
    c.capture_count = 0;

    XrFragment frag;
    if (!compile_node(&c, ast, &frag)) {
        xr_prog_free(prog);
        return NULL;
    }

    // Add MATCH instruction
    int match_id = emit_match(&c);
    if (match_id < 0) {
        frag_free(&frag);
        xr_prog_free(prog);
        return NULL;
    }

    // Connect to MATCH
    frag_patch_to(&frag, &c, match_id);

    prog->start = frag.start;

    // Build .* prefix for unanchored matching:
    //   ALT(frag.start, any_byte) where any_byte loops back to ALT
    {
        int any_id = emit_any_byte(&c, false, 0);  // out patched below
        int alt_id = emit_alt(&c, frag.start, any_id);
        if (any_id >= 0 && alt_id >= 0) {
            // any_byte loops back to ALT
            set_inst(&c, any_id, XR_OP_ANY_BYTE, alt_id);
            prog->start_unanchored = alt_id;
        } else {
            prog->start_unanchored = frag.start;  // fallback
        }
    }
    prog->capture_count = c.capture_count;

    frag_free(&frag);

    // Compute ByteMap
    xr_prog_compute_bytemap(prog);

    // Analyze optimization info (needs AST, call before freeing)
    xr_prog_analyze(prog, ast);

    return prog;
}

XR_FUNC void xr_prog_free(XrProg *prog) {
    if (!prog)
        return;

    xr_re_free(prog->inst);
    xr_re_free(prog->prefix);
    xr_re_free(prog->literal);
    xr_re_free(prog->unicode_ranges);

    if (prog->capture_names) {
        for (int i = 0; i < prog->capture_count; i++) {
            xr_re_free(prog->capture_names[i]);
        }
        xr_re_free(prog->capture_names);
    }

    // Free DFA cache
    if (prog->dfa_cache) {
        for (int i = 0; i < prog->dfa_cache_count; i++) {
            xr_re_free(prog->dfa_cache[i].next_pcs);
        }
        xr_re_free(prog->dfa_cache);
    }

    xr_re_free(prog);
}

/* ========================================================================
 * ByteMap Computation
 * ======================================================================== */

XR_FUNC void xr_prog_compute_bytemap(XrProg *prog) {
    // Initial: all bytes map to class 0
    memset(prog->bytemap, 0, sizeof(prog->bytemap));

    /*
     * Mark byte class boundaries
     *
     * For BYTE instruction, need to mark start and end of that byte
     * For example for 'a' (0x61), mark 0x61 and 0x62, so:
     * - class 0: 0x00-0x60
     * - class 1: 0x61 (only 'a')
     * - class 2: 0x62-0xff
     */
    uint8_t marked[257] = {0};  // 257 to mark after 256

    for (int i = 0; i < prog->inst_count; i++) {
        XrInst *ip = &prog->inst[i];
        XrOpcode op = XR_INST_OP(ip);

        switch (op) {
            case XR_OP_BYTE:
                // Single byte: mark start and end boundaries
                marked[ip->byte] = 1;
                marked[ip->byte + 1] = 1;
                break;

            case XR_OP_BYTE_RANGE:
                // Range: mark start and end boundaries
                marked[ip->lo] = 1;
                marked[ip->hi + 1] = 1;
                break;

            case XR_OP_ANY_BYTE:
                // Need to distinguish newline
                marked['\n'] = 1;
                marked['\n' + 1] = 1;
                break;

            default:
                break;
        }
    }

    // Allocate byte classes
    int class_id = 0;
    for (int i = 0; i < 256; i++) {
        if (marked[i]) {
            class_id++;
        }
        prog->bytemap[i] = class_id;
    }

    prog->bytemap_range = class_id + 1;
}

/* ========================================================================
 * Optimization Analysis
 * ======================================================================== */

// Check if AST starts with anchor ^
static bool ast_is_anchored(XrAstNode *node) {
    if (!node)
        return false;

    switch (node->type) {
        case XR_AST_EMPTY_MATCH:
            return (node->empty_flags & XR_EMPTY_BEGIN_TEXT) != 0;

        case XR_AST_CONCAT:
            // Concatenation: check first child node
            if (node->list.count > 0) {
                return ast_is_anchored(node->list.children[0]);
            }
            return false;

        case XR_AST_CAPTURE:
            // Capture group: check inner
            return ast_is_anchored(node->capture.child);

        case XR_AST_ALT:
            // Alternation: all branches must be anchored
            for (int i = 0; i < node->list.count; i++) {
                if (!ast_is_anchored(node->list.children[i])) {
                    return false;
                }
            }
            return node->list.count > 0;

        default:
            return false;
    }
}

/*
 * Extract fixed prefix from AST
 * Return number of bytes extracted
 */
static int ast_extract_prefix(XrAstNode *node, char *buf, int max_len) {
    if (!node || max_len <= 0)
        return 0;

    switch (node->type) {
        case XR_AST_LITERAL:
            // Literal: copy directly
            {
                int len = node->literal.len;
                if (len > max_len)
                    len = max_len;
                memcpy(buf, node->literal.str, len);
                return len;
            }

        case XR_AST_CONCAT:
            // Concatenation: extract from start until non-fixed node
            {
                int total = 0;
                for (int i = 0; i < node->list.count && total < max_len; i++) {
                    XrAstNode *child = node->list.children[i];
                    // Skip zero-width assertions (^, $, \b etc.)
                    if (child->type == XR_AST_EMPTY_MATCH)
                        continue;
                    // Only literals and literals inside capture groups can be extracted
                    if (child->type != XR_AST_LITERAL)
                        break;
                    int n = ast_extract_prefix(child, buf + total, max_len - total);
                    if (n == 0)
                        break;
                    total += n;
                }
                return total;
            }

        case XR_AST_CAPTURE:
            // Capture group: extract inner
            return ast_extract_prefix(node->capture.child, buf, max_len);

        default:
            return 0;
    }
}

/*
 * Check if it's OnePass mode (no ambiguous branches)
 * In OnePass mode, each byte has only one possible next step
 */
static bool ast_is_onepass(XrAstNode *node) {
    if (!node)
        return true;

    switch (node->type) {
        case XR_AST_LITERAL:
        case XR_AST_ANY:
        case XR_AST_EMPTY:
        case XR_AST_EMPTY_MATCH:
            return true;

        case XR_AST_CHAR_CLASS:
            // Multiple ranges or negated class generates ALT instructions, not OnePass
            if (node->cc.negated || node->cc.count > 1) {
                return false;
            }
            return true;

        case XR_AST_CONCAT:
            for (int i = 0; i < node->list.count; i++) {
                if (!ast_is_onepass(node->list.children[i])) {
                    return false;
                }
            }
            return true;

        case XR_AST_CAPTURE:
            return ast_is_onepass(node->capture.child);

        case XR_AST_REPEAT:
            // Repeat mode may have ambiguity: a* matching "aa" can be (a)(a) or (aa)
            // Simplified: only {n} with fixed count is OnePass
            if (node->repeat.min == node->repeat.max && node->repeat.max >= 0) {
                return ast_is_onepass(node->repeat.child);
            }
            return false;

        case XR_AST_ALT:
            // Alternation: if first char sets don't overlap then OnePass, but detection is complex,
            // return false for now
            return false;

        default:
            return false;
    }
}

// Check if it's pure literal mode (only BYTE instruction sequence)
static bool prog_is_literal(XrProg *prog, char **out_literal, int *out_len) {
    // Traverse instructions, check if all are BYTE -> BYTE -> ... -> MATCH
    char buf[256];
    int len = 0;
    int pc = prog->start;

    while (pc >= 0 && pc < prog->inst_count && len < 255) {
        XrInst *ip = &prog->inst[pc];
        XrOpcode op = XR_INST_OP(ip);

        if (op == XR_OP_BYTE) {
            buf[len++] = (char) ip->byte;
            pc = XR_INST_OUT(ip);
        } else if (op == XR_OP_MATCH) {
            // Success: pure literal
            if (len > 0) {
                *out_literal = (char *) xr_re_alloc(len + 1);
                memcpy(*out_literal, buf, len);
                (*out_literal)[len] = '\0';
                *out_len = len;
                return true;
            }
            return false;
        } else if (op == XR_OP_CAPTURE) {
            // Capture instruction skipped (just marks position)
            pc = XR_INST_OUT(ip);
        } else {
            // Other instructions: not pure literal
            return false;
        }
    }

    return false;
}

// Analyze AST and fill optimization info
static void xr_prog_analyze(XrProg *prog, XrAstNode *ast) {
    // 0. Scan for EMPTY_WIDTH instructions (DFA cannot cache these correctly)
    prog->has_empty_width = false;
    for (int i = 0; i < prog->inst_count; i++) {
        if (XR_INST_OP(&prog->inst[i]) == XR_OP_EMPTY_WIDTH) {
            prog->has_empty_width = true;
            break;
        }
    }

    // 1. Anchor detection
    prog->is_anchored = ast_is_anchored(ast);

    // 2. Literal detection (highest priority optimization)
    if (!(prog->flags & XR_RE_IGNORECASE)) {
        prog->is_literal = prog_is_literal(prog, &prog->literal, &prog->literal_len);
    }

    // 3. Prefix extraction (max 32 bytes)
    // Note: IGNORECASE mode cannot use prefix optimization, as prefix is lowercase but input may be
    // uppercase
    if (!prog->is_literal && !(prog->flags & XR_RE_IGNORECASE)) {
        char prefix_buf[33];
        int prefix_len = ast_extract_prefix(ast, prefix_buf, 32);
        if (prefix_len > 0) {
            prog->prefix = (char *) xr_re_alloc(prefix_len + 1);
            memcpy(prog->prefix, prefix_buf, prefix_len);
            prog->prefix[prefix_len] = '\0';
            prog->prefix_len = prefix_len;
        }
    }

    // 4. OnePass detection
    // Note: IGNORECASE mode generates ALT instructions at compile time, so cannot use OnePass
    if (prog->flags & XR_RE_IGNORECASE) {
        prog->is_onepass = false;
    } else {
        prog->is_onepass = ast_is_onepass(ast);
    }
}

/* ========================================================================
 * Debug Output
 * ======================================================================== */

static const char *opcode_name(XrOpcode op) {
    switch (op) {
        case XR_OP_NOP:
            return "NOP";
        case XR_OP_MATCH:
            return "MATCH";
        case XR_OP_FAIL:
            return "FAIL";
        case XR_OP_BYTE:
            return "BYTE";
        case XR_OP_BYTE_RANGE:
            return "RANGE";
        case XR_OP_ANY_BYTE:
            return "ANY";
        case XR_OP_ANY_BYTE_NL:
            return "ANY_NL";
        case XR_OP_ALT:
            return "ALT";
        case XR_OP_CAPTURE:
            return "CAP";
        case XR_OP_EMPTY_WIDTH:
            return "EMPTY";
        case XR_OP_UNICODE_RANGE:
            return "URANGE";
        default:
            return "?";
    }
}

XR_FUNC void xr_prog_dump(XrProg *prog) {
    printf("=== Prog (%d instructions, %d captures) ===\n", prog->inst_count, prog->capture_count);
    printf("start: %d, bytemap_range: %d\n", prog->start, prog->bytemap_range);

    for (int i = 0; i < prog->inst_count; i++) {
        XrInst *ip = &prog->inst[i];
        XrOpcode op = XR_INST_OP(ip);
        int out = XR_INST_OUT(ip);

        printf("%3d: %-6s ", i, opcode_name(op));

        switch (op) {
            case XR_OP_BYTE:
                printf("'%c' (0x%02x) -> %d", ip->byte >= 32 && ip->byte < 127 ? ip->byte : '.',
                       ip->byte, out);
                break;
            case XR_OP_BYTE_RANGE:
                printf("[%d-%d] -> %d", ip->lo, ip->hi, out);
                break;
            case XR_OP_ALT:
                printf("-> %d | %d", out, ip->out1);
                break;
            case XR_OP_CAPTURE:
                printf("%d (%s) -> %d", ip->cap, ip->cap % 2 == 0 ? "start" : "end", out);
                break;
            case XR_OP_EMPTY_WIDTH:
                printf("0x%x -> %d", ip->empty, out);
                break;
            case XR_OP_MATCH:
                printf("!");
                break;
            default:
                printf("-> %d", out);
                break;
        }
        printf("\n");
    }
}
