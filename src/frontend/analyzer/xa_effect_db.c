/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_effect_db.c - Canonical analyzer-owned error effect summaries
 */

#include "xa_effect_db.h"
#include "../../base/xhash.h"
#include "../../base/xmalloc.h"
#include "../../runtime/value/xtype.h"
#include "../../shared/xr_hash_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct XaErrorVariantInfo {
    uint64_t stable_key;
    char *name;
} XaErrorVariantInfo;

typedef struct XaErrorTypeInfo {
    XaErrorTypeId id;
    uint64_t stable_key;
    XrType *type_handle;
    char *qualified_name;
    XaErrorVariantInfo *variants;
    uint32_t variant_count;
    uint32_t variant_capacity;
} XaErrorTypeInfo;

struct XaEffectDatabase {
    XaErrorTypeInfo *error_types;
    uint32_t error_type_count;
    uint32_t error_type_capacity;
    XaEffectSummary *summaries;
    uint32_t summary_count;
    uint32_t summary_capacity;
    uint32_t next_revision;
};

static bool grow_array(void **ptr, uint32_t *capacity, size_t elem_size, uint32_t min_capacity) {
    if (*capacity >= min_capacity)
        return true;
    uint32_t new_capacity = *capacity ? *capacity * 2u : 4u;
    while (new_capacity < min_capacity)
        new_capacity *= 2u;
    void *next = xr_realloc(*ptr, (size_t) new_capacity * elem_size);
    if (!next)
        return false;
    *ptr = next;
    *capacity = new_capacity;
    return true;
}

static void bitset_free(XaBitSet *set) {
    if (!set)
        return;
    if (set->words)
        xr_free(set->words);
    set->words = NULL;
    set->word_count = 0;
}

static bool bitset_ensure_word(XaBitSet *set, uint32_t word_index) {
    if (!set)
        return false;
    uint32_t needed = word_index + 1u;
    if (set->word_count >= needed)
        return true;
    uint64_t *next = (uint64_t *) xr_realloc(set->words, (size_t) needed * sizeof(uint64_t));
    if (!next)
        return false;
    for (uint32_t i = set->word_count; i < needed; i++)
        next[i] = 0;
    set->words = next;
    set->word_count = needed;
    return true;
}

static bool bitset_set(XaBitSet *set, uint32_t bit) {
    uint32_t word_index = bit / 64u;
    uint32_t bit_index = bit % 64u;
    if (!bitset_ensure_word(set, word_index))
        return false;
    set->words[word_index] |= (UINT64_C(1) << bit_index);
    return true;
}

static bool bitset_clear_bit(XaBitSet *set, uint32_t bit) {
    if (!set)
        return false;
    uint32_t word_index = bit / 64u;
    uint32_t bit_index = bit % 64u;
    if (word_index >= set->word_count)
        return false;
    set->words[word_index] &= ~(UINT64_C(1) << bit_index);
    return true;
}

static bool bitset_has_any(const XaBitSet *set) {
    if (!set)
        return false;
    for (uint32_t i = 0; i < set->word_count; i++) {
        if (set->words[i] != 0)
            return true;
    }
    return false;
}

bool xa_bitset_test(const XaBitSet *set, uint32_t bit) {
    if (!set)
        return false;
    uint32_t word_index = bit / 64u;
    uint32_t bit_index = bit % 64u;
    if (word_index >= set->word_count)
        return false;
    return (set->words[word_index] & (UINT64_C(1) << bit_index)) != 0;
}

uint32_t xa_bitset_word_count(const XaBitSet *set) {
    return set ? set->word_count : 0;
}

static bool bitset_copy(XaBitSet *dst, const XaBitSet *src) {
    if (!dst || !src)
        return false;
    dst->words = NULL;
    dst->word_count = 0;
    if (src->word_count == 0)
        return true;
    dst->words = (uint64_t *) xr_malloc((size_t) src->word_count * sizeof(uint64_t));
    if (!dst->words)
        return false;
    memcpy(dst->words, src->words, (size_t) src->word_count * sizeof(uint64_t));
    dst->word_count = src->word_count;
    return true;
}

static bool bitset_equals(const XaBitSet *a, const XaBitSet *b) {
    uint32_t aw = a ? a->word_count : 0;
    uint32_t bw = b ? b->word_count : 0;
    uint32_t max = aw > bw ? aw : bw;
    for (uint32_t i = 0; i < max; i++) {
        uint64_t av = (a && i < aw) ? a->words[i] : 0;
        uint64_t bv = (b && i < bw) ? b->words[i] : 0;
        if (av != bv)
            return false;
    }
    return true;
}

static const XaErrorTypeInfo *db_type_info(const XaEffectDatabase *db, XaErrorTypeId type_id) {
    if (!db || type_id == XA_ERROR_TYPE_NONE)
        return NULL;
    uint32_t index = type_id - 1u;
    if (index >= db->error_type_count)
        return NULL;
    return &db->error_types[index];
}

static XaErrorTypeInfo *db_type_info_mut(XaEffectDatabase *db, XaErrorTypeId type_id) {
    return (XaErrorTypeInfo *) db_type_info(db, type_id);
}

XaEffectDatabase *xa_effect_db_new(void) {
    return (XaEffectDatabase *) xr_calloc(1, sizeof(XaEffectDatabase));
}

void xa_effect_db_clear(XaEffectDatabase *db) {
    if (!db)
        return;
    for (uint32_t i = 0; i < db->error_type_count; i++) {
        XaErrorTypeInfo *info = &db->error_types[i];
        for (uint32_t v = 0; v < info->variant_count; v++) {
            if (info->variants[v].name)
                xr_free(info->variants[v].name);
        }
        if (info->variants)
            xr_free(info->variants);
        if (info->qualified_name)
            xr_free(info->qualified_name);
    }
    xr_free(db->error_types);
    db->error_types = NULL;
    db->error_type_count = 0;
    db->error_type_capacity = 0;

    for (uint32_t i = 0; i < db->summary_count; i++)
        xa_effect_summary_clear(&db->summaries[i]);
    xr_free(db->summaries);
    db->summaries = NULL;
    db->summary_count = 0;
    db->summary_capacity = 0;
    db->next_revision = 0;
}

void xa_effect_db_free(XaEffectDatabase *db) {
    if (!db)
        return;
    xa_effect_db_clear(db);
    xr_free(db);
}

static uint64_t stable_key_text2(const char *prefix, const char *name) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s:%s", prefix ? prefix : "", name ? name : "<anonymous>");
    uint64_t key = xr_hash_bytes64(buf, strlen(buf));
    return key ? key : 1u;
}

static uint64_t stable_key_text3(const char *prefix, const char *type_name,
                                 const char *variant_name) {
    char buf[768];
    snprintf(buf, sizeof(buf), "%s:%s.%s", prefix ? prefix : "", type_name ? type_name : "?",
             variant_name ? variant_name : "?");
    uint64_t key = xr_hash_bytes64(buf, strlen(buf));
    return key ? key : 1u;
}

static char *dup_cstr(const char *s) {
    if (!s)
        return NULL;
    size_t len = strlen(s);
    char *copy = (char *) xr_malloc(len + 1u);
    if (!copy)
        return NULL;
    memcpy(copy, s, len + 1u);
    return copy;
}

XaErrorTypeId xa_effect_db_register_error_type(XaEffectDatabase *db, uint64_t stable_type_key,
                                               XrType *type_handle) {
    if (!db || stable_type_key == 0)
        return XA_ERROR_TYPE_NONE;
    for (uint32_t i = 0; i < db->error_type_count; i++) {
        if (db->error_types[i].stable_key == stable_type_key) {
            if (type_handle && !db->error_types[i].type_handle)
                db->error_types[i].type_handle = type_handle;
            return db->error_types[i].id;
        }
    }
    if (!grow_array((void **) &db->error_types, &db->error_type_capacity, sizeof(XaErrorTypeInfo),
                    db->error_type_count + 1u))
        return XA_ERROR_TYPE_NONE;
    XaErrorTypeInfo *info = &db->error_types[db->error_type_count];
    memset(info, 0, sizeof(*info));
    info->id = db->error_type_count + 1u;
    info->stable_key = stable_type_key;
    info->type_handle = type_handle;
    db->error_type_count++;
    return info->id;
}

XaErrorTypeId xa_effect_db_register_error_enum(XaEffectDatabase *db, XrType *enum_type) {
    if (!db || !enum_type || !XR_TYPE_IS_ENUM(enum_type))
        return XA_ERROR_TYPE_NONE;
    const char *name = enum_type->enum_type.enum_name;
    XaErrorTypeId type_id =
        xa_effect_db_register_error_type(db, stable_key_text2("enum", name), enum_type);
    xa_effect_db_set_error_type_name(db, type_id, name);
    const XrEnumLayout *layout = enum_type->enum_type.layout;
    if (type_id != XA_ERROR_TYPE_NONE && layout) {
        for (uint32_t i = 0; i < layout->variant_count; i++) {
            const char *variant_name = layout->variants[i].name;
            XaErrorVariantId variant_id = xa_effect_db_register_error_variant(
                db, type_id, stable_key_text3("variant", name, variant_name));
            xa_effect_db_set_error_variant_name(db, type_id, variant_id, variant_name);
        }
    }
    return type_id;
}

XaErrorVariantId xa_effect_db_register_error_variant(XaEffectDatabase *db, XaErrorTypeId type_id,
                                                     uint64_t stable_variant_key) {
    XaErrorTypeInfo *info = db_type_info_mut(db, type_id);
    if (!info || stable_variant_key == 0)
        return XA_ERROR_VARIANT_INVALID;
    for (uint32_t i = 0; i < info->variant_count; i++) {
        if (info->variants[i].stable_key == stable_variant_key)
            return i;
    }
    if (!grow_array((void **) &info->variants, &info->variant_capacity, sizeof(XaErrorVariantInfo),
                    info->variant_count + 1u))
        return XA_ERROR_VARIANT_INVALID;
    XaErrorVariantId id = info->variant_count;
    info->variants[id].stable_key = stable_variant_key;
    info->variants[id].name = NULL;
    info->variant_count++;
    return id;
}

void xa_effect_db_set_error_type_name(XaEffectDatabase *db, XaErrorTypeId type_id,
                                      const char *name) {
    XaErrorTypeInfo *info = db_type_info_mut(db, type_id);
    if (!info || !name || info->qualified_name)
        return;
    info->qualified_name = dup_cstr(name);
}

void xa_effect_db_set_error_variant_name(XaEffectDatabase *db, XaErrorTypeId type_id,
                                         XaErrorVariantId variant_id, const char *name) {
    XaErrorTypeInfo *info = db_type_info_mut(db, type_id);
    if (!info || variant_id >= info->variant_count || !name || info->variants[variant_id].name)
        return;
    info->variants[variant_id].name = dup_cstr(name);
}

const char *xa_effect_db_error_type_name(const XaEffectDatabase *db, XaErrorTypeId type_id) {
    const XaErrorTypeInfo *info = db_type_info(db, type_id);
    return info ? info->qualified_name : NULL;
}

const char *xa_effect_db_error_variant_name(const XaEffectDatabase *db, XaErrorTypeId type_id,
                                            XaErrorVariantId variant_id) {
    const XaErrorTypeInfo *info = db_type_info(db, type_id);
    if (!info || variant_id >= info->variant_count)
        return NULL;
    return info->variants[variant_id].name;
}

uint64_t xa_effect_db_error_type_key(const XaEffectDatabase *db, XaErrorTypeId type_id) {
    const XaErrorTypeInfo *info = db_type_info(db, type_id);
    return info ? info->stable_key : 0;
}

XrType *xa_effect_db_error_type_handle(const XaEffectDatabase *db, XaErrorTypeId type_id) {
    const XaErrorTypeInfo *info = db_type_info(db, type_id);
    return info ? info->type_handle : NULL;
}

uint64_t xa_effect_db_error_variant_key(const XaEffectDatabase *db, XaErrorTypeId type_id,
                                        XaErrorVariantId variant_id) {
    const XaErrorTypeInfo *info = db_type_info(db, type_id);
    if (!info || variant_id >= info->variant_count)
        return 0;
    return info->variants[variant_id].stable_key;
}

void xa_effect_summary_init(XaEffectSummary *summary) {
    if (!summary)
        return;
    memset(summary, 0, sizeof(*summary));
    summary->error_set_completeness = XA_EFFECT_COMPLETE;
    summary->completeness = XA_EFFECT_COMPLETE;
}

void xa_effect_summary_clear(XaEffectSummary *summary) {
    if (!summary)
        return;
    for (uint32_t i = 0; i < summary->escaping.count; i++)
        bitset_free(&summary->escaping.types[i].variants);
    xr_free(summary->escaping.types);
    xr_free(summary->roots);
    xa_effect_summary_init(summary);
}

bool xa_effect_summary_add_root(XaEffectSummary *summary, XaEffectEdgeId root_id) {
    if (!summary || root_id == XA_EFFECT_EDGE_NONE)
        return false;
    uint32_t pos = 0;
    while (pos < summary->root_count && summary->roots[pos] < root_id)
        pos++;
    if (pos < summary->root_count && summary->roots[pos] == root_id)
        return true;
    if (!grow_array((void **) &summary->roots, &summary->root_capacity, sizeof(XaEffectEdgeId),
                    summary->root_count + 1u))
        return false;
    if (pos < summary->root_count) {
        memmove(&summary->roots[pos + 1u], &summary->roots[pos],
                (size_t) (summary->root_count - pos) * sizeof(XaEffectEdgeId));
    }
    summary->roots[pos] = root_id;
    summary->root_count++;
    return true;
}

static bool summary_add_roots(XaEffectSummary *summary, const XaEffectSummary *src, bool *changed) {
    if (changed)
        *changed = false;
    if (src->root_count == 0)
        return true;
    if (!grow_array((void **) &summary->roots, &summary->root_capacity, sizeof(XaEffectEdgeId),
                    summary->root_count + src->root_count))
        return false;
    for (uint32_t i = 0; i < src->root_count; i++) {
        uint32_t old_count = summary->root_count;
        if (!xa_effect_summary_add_root(summary, src->roots[i]))
            return false;
        if (changed && summary->root_count != old_count)
            *changed = true;
    }
    return true;
}

static int summary_find_type(const XaEffectSummary *summary, XaErrorTypeId type_id,
                             uint64_t stable_type_key) {
    if (!summary || type_id == XA_ERROR_TYPE_NONE)
        return -1;
    for (uint32_t i = 0; i < summary->escaping.count; i++) {
        if (summary->escaping.types[i].type_id == type_id &&
            summary->escaping.types[i].stable_type_key == stable_type_key)
            return (int) i;
    }
    return -1;
}

static bool summary_insert_type(XaEffectSummary *summary, XaErrorTypeId type_id,
                                uint64_t stable_type_key, XaErrorTypeSet **out_set) {
    if (!summary || type_id == XA_ERROR_TYPE_NONE || stable_type_key == 0)
        return false;
    int existing = summary_find_type(summary, type_id, stable_type_key);
    if (existing >= 0) {
        if (out_set)
            *out_set = &summary->escaping.types[existing];
        return true;
    }
    if (!grow_array((void **) &summary->escaping.types, &summary->escaping.capacity,
                    sizeof(XaErrorTypeSet), summary->escaping.count + 1u))
        return false;
    uint32_t pos = summary->escaping.count;
    for (uint32_t i = 0; i < summary->escaping.count; i++) {
        XaErrorTypeSet *cur = &summary->escaping.types[i];
        if (stable_type_key < cur->stable_type_key ||
            (stable_type_key == cur->stable_type_key && type_id < cur->type_id)) {
            pos = i;
            break;
        }
    }
    if (pos < summary->escaping.count) {
        memmove(&summary->escaping.types[pos + 1u], &summary->escaping.types[pos],
                (size_t) (summary->escaping.count - pos) * sizeof(XaErrorTypeSet));
    }
    XaErrorTypeSet *set = &summary->escaping.types[pos];
    memset(set, 0, sizeof(*set));
    set->type_id = type_id;
    set->stable_type_key = stable_type_key;
    summary->escaping.count++;
    if (out_set)
        *out_set = set;
    return true;
}

bool xa_effect_summary_add_variant(XaEffectDatabase *db, XaEffectSummary *summary,
                                   XaErrorTypeId type_id, XaErrorVariantId variant_id) {
    if (!db || !summary || variant_id == XA_ERROR_VARIANT_INVALID)
        return false;
    uint64_t type_key = xa_effect_db_error_type_key(db, type_id);
    if (type_key == 0)
        return false;
    XaErrorTypeSet *set = NULL;
    if (!summary_insert_type(summary, type_id, type_key, &set))
        return false;
    if (set->all_variants)
        return true;
    return bitset_set(&set->variants, variant_id);
}

bool xa_effect_summary_add_all_variants(XaEffectDatabase *db, XaEffectSummary *summary,
                                        XaErrorTypeId type_id) {
    if (!db || !summary)
        return false;
    uint64_t type_key = xa_effect_db_error_type_key(db, type_id);
    if (type_key == 0)
        return false;
    XaErrorTypeSet *set = NULL;
    if (!summary_insert_type(summary, type_id, type_key, &set))
        return false;
    set->all_variants = true;
    bitset_free(&set->variants);
    return true;
}

bool xa_effect_summary_add_summary(XaEffectDatabase *db, XaEffectSummary *summary,
                                   const XaEffectSummary *src) {
    if (!db || !summary || !src)
        return false;
    bool ok = true;
    if (src->completeness == XA_EFFECT_INCOMPLETE) {
        summary->completeness = XA_EFFECT_INCOMPLETE;
        summary->unknown_reasons |= src->unknown_reasons;
    }
    summary->semantic_effects |= src->semantic_effects;
    summary->unknown_semantic_effects |= src->unknown_semantic_effects;
    if (src->error_set_completeness == XA_EFFECT_INCOMPLETE) {
        summary->error_set_completeness = XA_EFFECT_INCOMPLETE;
        summary->error_unknown_reasons |= src->error_unknown_reasons;
    }
    summary->contains_unsafe_op |= src->contains_unsafe_op;
    summary->requires_unsafe_at_call |= src->requires_unsafe_at_call;
    for (uint32_t i = 0; i < src->escaping.count; i++) {
        const XaErrorTypeSet *type_set = &src->escaping.types[i];
        if (type_set->all_variants) {
            ok = xa_effect_summary_add_all_variants(db, summary, type_set->type_id) && ok;
            continue;
        }
        for (uint32_t word = 0; word < type_set->variants.word_count; word++) {
            uint64_t bits = type_set->variants.words[word];
            while (bits) {
                uint32_t bit = (uint32_t) __builtin_ctzll(bits);
                XaErrorVariantId variant_id = word * 64u + bit;
                ok =
                    xa_effect_summary_add_variant(db, summary, type_set->type_id, variant_id) && ok;
                bits &= bits - 1u;
            }
        }
    }
    ok = summary_add_roots(summary, src, NULL) && ok;
    return ok;
}

void xa_effect_summary_add_semantic_effects(XaEffectSummary *summary, XaSemanticEffectSet effects) {
    if (!summary)
        return;
    summary->semantic_effects |= effects;
    summary->fingerprint = 0;
}

bool xa_effect_summary_has_semantic_effect(const XaEffectSummary *summary,
                                           XaSemanticEffect effect) {
    return summary && effect != XA_SEM_EFFECT_NONE &&
           (summary->semantic_effects & (XaSemanticEffectSet) effect) != 0;
}

void xa_effect_summary_mark_contains_unsafe(XaEffectSummary *summary) {
    if (!summary)
        return;
    summary->contains_unsafe_op = true;
    summary->fingerprint = 0;
}

void xa_effect_summary_mark_requires_unsafe(XaEffectSummary *summary) {
    if (!summary)
        return;
    summary->requires_unsafe_at_call = true;
    summary->fingerprint = 0;
}

bool xa_effect_summary_add_type_from_summary(XaEffectDatabase *db, XaEffectSummary *summary,
                                             const XaEffectSummary *src, XaErrorTypeId type_id) {
    if (!db || !summary || !src || type_id == XA_ERROR_TYPE_NONE)
        return false;
    for (uint32_t i = 0; i < src->escaping.count; i++) {
        const XaErrorTypeSet *type_set = &src->escaping.types[i];
        if (type_set->type_id != type_id)
            continue;
        if (type_set->all_variants)
            return xa_effect_summary_add_all_variants(db, summary, type_id);
        bool ok = true;
        for (uint32_t word = 0; word < type_set->variants.word_count; word++) {
            uint64_t bits = type_set->variants.words[word];
            while (bits) {
                uint32_t bit = (uint32_t) __builtin_ctzll(bits);
                XaErrorVariantId variant_id = word * 64u + bit;
                ok = xa_effect_summary_add_variant(db, summary, type_id, variant_id) && ok;
                bits &= bits - 1u;
            }
        }
        return ok;
    }
    return true;
}

bool xa_effect_summary_add_variant_from_summary(XaEffectDatabase *db, XaEffectSummary *summary,
                                                const XaEffectSummary *src, XaErrorTypeId type_id,
                                                XaErrorVariantId variant_id) {
    if (!db || !summary || !src || type_id == XA_ERROR_TYPE_NONE ||
        variant_id == XA_ERROR_VARIANT_INVALID)
        return false;
    for (uint32_t i = 0; i < src->escaping.count; i++) {
        const XaErrorTypeSet *type_set = &src->escaping.types[i];
        if (type_set->type_id != type_id)
            continue;
        if (type_set->all_variants || xa_bitset_test(&type_set->variants, variant_id))
            return xa_effect_summary_add_variant(db, summary, type_id, variant_id);
        return true;
    }
    return true;
}

void xa_effect_summary_clear_escaping(XaEffectSummary *summary) {
    if (!summary)
        return;
    for (uint32_t i = 0; i < summary->escaping.count; i++)
        bitset_free(&summary->escaping.types[i].variants);
    xr_free(summary->escaping.types);
    summary->escaping.types = NULL;
    summary->escaping.count = 0;
    summary->escaping.capacity = 0;
    summary->fingerprint = 0;
}

bool xa_effect_summary_subtract_type(XaEffectSummary *summary, XaErrorTypeId type_id) {
    if (!summary || type_id == XA_ERROR_TYPE_NONE)
        return false;
    for (uint32_t i = 0; i < summary->escaping.count; i++) {
        if (summary->escaping.types[i].type_id != type_id)
            continue;
        bitset_free(&summary->escaping.types[i].variants);
        if (i + 1u < summary->escaping.count) {
            memmove(&summary->escaping.types[i], &summary->escaping.types[i + 1u],
                    (size_t) (summary->escaping.count - i - 1u) * sizeof(XaErrorTypeSet));
        }
        summary->escaping.count--;
        if (summary->escaping.count < summary->escaping.capacity)
            memset(&summary->escaping.types[summary->escaping.count], 0, sizeof(XaErrorTypeSet));
        summary->fingerprint = 0;
        return true;
    }
    return false;
}

bool xa_effect_summary_subtract_variant(XaEffectDatabase *db, XaEffectSummary *summary,
                                        XaErrorTypeId type_id, XaErrorVariantId variant_id) {
    if (!db || !summary || type_id == XA_ERROR_TYPE_NONE || variant_id == XA_ERROR_VARIANT_INVALID)
        return false;
    XaErrorTypeInfo *info = db_type_info_mut(db, type_id);
    if (!info || variant_id >= info->variant_count)
        return false;
    for (uint32_t i = 0; i < summary->escaping.count; i++) {
        XaErrorTypeSet *set = &summary->escaping.types[i];
        if (set->type_id != type_id)
            continue;
        if (set->all_variants) {
            set->all_variants = false;
            bitset_free(&set->variants);
            for (uint32_t vid = 0; vid < info->variant_count; vid++) {
                if (vid != variant_id && !bitset_set(&set->variants, vid))
                    return false;
            }
        } else {
            bitset_clear_bit(&set->variants, variant_id);
        }
        if (!bitset_has_any(&set->variants)) {
            bitset_free(&set->variants);
            if (i + 1u < summary->escaping.count) {
                memmove(&summary->escaping.types[i], &summary->escaping.types[i + 1u],
                        (size_t) (summary->escaping.count - i - 1u) * sizeof(XaErrorTypeSet));
            }
            summary->escaping.count--;
            if (summary->escaping.count < summary->escaping.capacity)
                memset(&summary->escaping.types[summary->escaping.count], 0,
                       sizeof(XaErrorTypeSet));
        }
        summary->fingerprint = 0;
        return true;
    }
    return true;
}

void xa_effect_summary_mark_incomplete(XaEffectSummary *summary, XaUnknownReason reason) {
    if (!summary || reason == XA_UNKNOWN_NONE)
        return;
    summary->completeness = XA_EFFECT_INCOMPLETE;
    summary->error_set_completeness = XA_EFFECT_INCOMPLETE;
    summary->unknown_reasons |= (XaUnknownReasonSet) reason;
    summary->error_unknown_reasons |= (XaUnknownReasonSet) reason;
}

void xa_effect_summary_mark_semantic_incomplete(XaEffectSummary *summary, XaSemanticEffect effect,
                                                XaUnknownReason reason) {
    if (!summary || effect == XA_SEM_EFFECT_NONE || reason == XA_UNKNOWN_NONE)
        return;
    summary->completeness = XA_EFFECT_INCOMPLETE;
    summary->unknown_semantic_effects |= (XaSemanticEffectSet) effect;
    summary->unknown_reasons |= (XaUnknownReasonSet) reason;
    summary->fingerprint = 0;
}

bool xa_effect_summary_is_complete(const XaEffectSummary *summary) {
    return summary && summary->completeness == XA_EFFECT_COMPLETE &&
           summary->error_set_completeness == XA_EFFECT_COMPLETE &&
           summary->unknown_semantic_effects == XA_SEM_EFFECT_NONE &&
           summary->unknown_reasons == XA_UNKNOWN_NONE;
}

bool xa_effect_summary_has_resource_failure(const XaEffectSummary *summary) {
    return summary && (summary->unknown_reasons & XA_UNKNOWN_ANALYSIS_RESOURCE_FAILURE) != 0;
}

bool xa_effect_summary_is_nothrow(const XaEffectSummary *summary) {
    return summary && summary->error_set_completeness == XA_EFFECT_COMPLETE &&
           !xa_effect_summary_has_resource_failure(summary) && summary->escaping.count == 0;
}

static uint64_t hash_u64(uint64_t h, uint64_t value) {
    for (uint32_t i = 0; i < 8; i++) {
        h ^= (uint8_t) (value >> (i * 8u));
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t av = *(const uint64_t *) a;
    uint64_t bv = *(const uint64_t *) b;
    return (av > bv) - (av < bv);
}

static uint64_t fallback_variant_key(uint64_t type_key, uint32_t variant_id) {
    uint64_t h = UINT64_C(14695981039346656037);
    h = hash_u64(h, UINT64_C(0x78616566665f7661)); /* "xaeff_va" */
    h = hash_u64(h, type_key);
    h = hash_u64(h, variant_id);
    return xr_hash_core_mix_u64(h);
}

static uint64_t hash_variant_keys(const XaEffectDatabase *db, uint64_t h,
                                  const XaErrorTypeSet *set) {
    const XaErrorTypeInfo *info = db_type_info(db, set->type_id);
    uint32_t key_count = 0;
    uint64_t *keys = NULL;
    if (set->all_variants) {
        key_count = info ? info->variant_count : 0;
        if (key_count > 0) {
            keys = (uint64_t *) xr_malloc((size_t) key_count * sizeof(uint64_t));
            if (keys) {
                for (uint32_t i = 0; i < key_count; i++)
                    keys[i] = info->variants[i].stable_key;
            } else {
                key_count = 0;
            }
        }
    } else {
        for (uint32_t word = 0; word < set->variants.word_count; word++) {
            uint64_t bits = set->variants.words[word];
            while (bits) {
                key_count++;
                bits &= bits - 1u;
            }
        }
        if (key_count > 0) {
            keys = (uint64_t *) xr_malloc((size_t) key_count * sizeof(uint64_t));
            if (keys) {
                uint32_t out = 0;
                for (uint32_t word = 0; word < set->variants.word_count; word++) {
                    uint64_t bits = set->variants.words[word];
                    for (uint32_t bit = 0; bit < 64u; bit++) {
                        if ((bits & (UINT64_C(1) << bit)) == 0)
                            continue;
                        uint32_t variant_id = word * 64u + bit;
                        uint64_t key = xa_effect_db_error_variant_key(db, set->type_id, variant_id);
                        keys[out++] =
                            key ? key : fallback_variant_key(set->stable_type_key, variant_id);
                    }
                }
            } else {
                key_count = 0;
            }
        }
    }
    h = hash_u64(h, key_count);
    if (keys && key_count > 1)
        qsort(keys, key_count, sizeof(uint64_t), cmp_u64);
    for (uint32_t i = 0; i < key_count; i++)
        h = hash_u64(h, keys[i]);
    xr_free(keys);
    return h;
}

uint64_t xa_effect_summary_fingerprint(const XaEffectDatabase *db, const XaEffectSummary *summary) {
    if (!summary)
        return 0;
    uint64_t h = UINT64_C(14695981039346656037);
    h = hash_u64(h, UINT64_C(0x7861656666646233)); /* "xaeffdb3" */
    h = hash_u64(h, (uint64_t) summary->semantic_effects);
    h = hash_u64(h, (uint64_t) summary->unknown_semantic_effects);
    h = hash_u64(h, (uint64_t) summary->error_set_completeness);
    h = hash_u64(h, (uint64_t) summary->error_unknown_reasons);
    h = hash_u64(h, (uint64_t) summary->completeness);
    h = hash_u64(h, (uint64_t) summary->unknown_reasons);
    h = hash_u64(h, summary->contains_unsafe_op ? 1u : 0u);
    h = hash_u64(h, summary->requires_unsafe_at_call ? 1u : 0u);
    h = hash_u64(h, (uint64_t) summary->escaping.count);
    for (uint32_t i = 0; i < summary->escaping.count; i++) {
        const XaErrorTypeSet *set = &summary->escaping.types[i];
        h = hash_u64(h, set->stable_type_key);
        h = hash_u64(h, set->all_variants ? 1u : 0u);
        h = hash_variant_keys(db, h, set);
    }
    h = xr_hash_core_mix_u64(h);
    return h ? h : 1u;
}

/* Collects the sorted, de-duplicated stable variant keys escaping for one type
 * set.  `all_variants` sets expand to the currently registered variant universe
 * of the error type; specific sets expand to the escaping bits only. */
static uint64_t *collect_variant_keys(const XaEffectDatabase *db, const XaErrorTypeSet *set,
                                      uint32_t *out_count) {
    if (out_count)
        *out_count = 0;
    if (!set)
        return NULL;
    const XaErrorTypeInfo *info = db_type_info(db, set->type_id);
    uint32_t count = 0;
    if (set->all_variants) {
        count = info ? info->variant_count : 0;
    } else {
        for (uint32_t word = 0; word < set->variants.word_count; word++) {
            uint64_t bits = set->variants.words[word];
            while (bits) {
                count++;
                bits &= bits - 1u;
            }
        }
    }
    if (count == 0)
        return NULL;
    uint64_t *keys = (uint64_t *) xr_malloc((size_t) count * sizeof(uint64_t));
    if (!keys)
        return NULL;
    uint32_t out = 0;
    if (set->all_variants) {
        for (uint32_t i = 0; i < count; i++)
            keys[out++] = info->variants[i].stable_key;
    } else {
        for (uint32_t word = 0; word < set->variants.word_count; word++) {
            uint64_t bits = set->variants.words[word];
            for (uint32_t bit = 0; bit < 64u; bit++) {
                if ((bits & (UINT64_C(1) << bit)) == 0)
                    continue;
                uint32_t variant_id = word * 64u + bit;
                uint64_t key = xa_effect_db_error_variant_key(db, set->type_id, variant_id);
                keys[out++] = key ? key : fallback_variant_key(set->stable_type_key, variant_id);
            }
        }
    }
    if (out > 1)
        qsort(keys, out, sizeof(uint64_t), cmp_u64);
    if (out_count)
        *out_count = out;
    return keys;
}

static void diff_specific_variant_sets(const XaEffectDatabase *db, const XaErrorTypeSet *before,
                                       const XaErrorTypeSet *after, XaEffectDiff *diff) {
    uint32_t bc = 0;
    uint32_t ac = 0;
    uint64_t *bk = collect_variant_keys(db, before, &bc);
    uint64_t *ak = collect_variant_keys(db, after, &ac);
    uint32_t x = 0;
    uint32_t y = 0;
    while (x < bc || y < ac) {
        if (x < bc && (y >= ac || bk[x] < ak[y])) {
            diff->removed_escaping = true;
            x++;
        } else if (y < ac && (x >= bc || ak[y] < bk[x])) {
            diff->added_escaping = true;
            y++;
        } else {
            x++;
            y++;
        }
    }
    xr_free(bk);
    xr_free(ak);
}

XaEffectDiffKind xa_effect_summary_diff(const XaEffectDatabase *db, const XaEffectSummary *before,
                                        const XaEffectSummary *after, XaEffectDiff *out_diff) {
    XaEffectDiff diff;
    memset(&diff, 0, sizeof(diff));

    XaEffectSummary empty;
    xa_effect_summary_init(&empty);
    if (!before)
        before = &empty;
    if (!after)
        after = &empty;

    /* Both escaping sets are kept sorted by stable_type_key, so a merge join
     * detects newly escaping / no-longer escaping typed errors by stable key. */
    uint32_t bi = 0;
    uint32_t ai = 0;
    while (bi < before->escaping.count || ai < after->escaping.count) {
        const XaErrorTypeSet *bs = bi < before->escaping.count ? &before->escaping.types[bi] : NULL;
        const XaErrorTypeSet *as = ai < after->escaping.count ? &after->escaping.types[ai] : NULL;
        if (bs && (!as || bs->stable_type_key < as->stable_type_key)) {
            diff.removed_escaping = true;
            bi++;
            continue;
        }
        if (as && (!bs || as->stable_type_key < bs->stable_type_key)) {
            diff.added_escaping = true;
            ai++;
            continue;
        }
        if (bs->all_variants && as->all_variants) {
            /* identical full coverage */
        } else if (!bs->all_variants && as->all_variants) {
            diff.added_escaping = true;
        } else if (bs->all_variants && !as->all_variants) {
            diff.removed_escaping = true;
        } else {
            diff_specific_variant_sets(db, bs, as, &diff);
        }
        bi++;
        ai++;
    }

    if (before->completeness == XA_EFFECT_COMPLETE && after->completeness == XA_EFFECT_INCOMPLETE)
        diff.became_incomplete = true;
    if (before->completeness == XA_EFFECT_INCOMPLETE && after->completeness == XA_EFFECT_COMPLETE)
        diff.became_complete = true;
    if ((after->unknown_reasons & ~before->unknown_reasons) != 0)
        diff.widened_unknown = true;
    if ((before->unknown_reasons & ~after->unknown_reasons) != 0)
        diff.narrowed_unknown = true;

    diff.added_semantic_effects = after->semantic_effects & ~before->semantic_effects;
    diff.removed_semantic_effects = before->semantic_effects & ~after->semantic_effects;
    diff.added_unsafe_operation = !before->contains_unsafe_op && after->contains_unsafe_op;
    diff.removed_unsafe_operation = before->contains_unsafe_op && !after->contains_unsafe_op;
    diff.added_unsafe_call_requirement =
        !before->requires_unsafe_at_call && after->requires_unsafe_at_call;
    diff.removed_unsafe_call_requirement =
        before->requires_unsafe_at_call && !after->requires_unsafe_at_call;

    if (diff.added_escaping || diff.became_incomplete || diff.widened_unknown ||
        diff.added_semantic_effects != XA_SEM_EFFECT_NONE || diff.added_unsafe_operation ||
        diff.added_unsafe_call_requirement)
        diff.kind = XA_EFFECT_DIFF_BREAKING;
    else if (diff.removed_escaping || diff.became_complete || diff.narrowed_unknown ||
             diff.removed_semantic_effects != XA_SEM_EFFECT_NONE || diff.removed_unsafe_operation ||
             diff.removed_unsafe_call_requirement)
        diff.kind = XA_EFFECT_DIFF_IMPROVEMENT;
    else
        diff.kind = XA_EFFECT_DIFF_COMPATIBLE;

    xa_effect_summary_clear(&empty);
    if (out_diff)
        *out_diff = diff;
    return diff.kind;
}

typedef struct XaJsonBuf {
    char *data;
    size_t len;
    size_t cap;
    bool ok;
} XaJsonBuf;

static void xa_json_reserve(XaJsonBuf *b, size_t extra) {
    if (!b->ok)
        return;
    size_t need = b->len + extra + 1u;
    if (need <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 128u;
    while (cap < need)
        cap *= 2u;
    char *next = (char *) xr_realloc(b->data, cap);
    if (!next) {
        b->ok = false;
        return;
    }
    b->data = next;
    b->cap = cap;
}

static void xa_json_raw(XaJsonBuf *b, const char *s) {
    if (!b->ok || !s)
        return;
    size_t n = strlen(s);
    xa_json_reserve(b, n);
    if (!b->ok)
        return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void xa_json_string(XaJsonBuf *b, const char *s) {
    xa_json_raw(b, "\"");
    for (const char *p = s; b->ok && p && *p; p++) {
        unsigned char c = (unsigned char) *p;
        if (c == '"' || c == '\\') {
            char esc[3] = {'\\', (char) c, '\0'};
            xa_json_raw(b, esc);
        } else if (c < 0x20u) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", (unsigned) c);
            xa_json_raw(b, buf);
        } else {
            char one[2] = {(char) c, '\0'};
            xa_json_raw(b, one);
        }
    }
    xa_json_raw(b, "\"");
}

static void xa_json_hex_u64(XaJsonBuf *b, uint64_t value) {
    char buf[24];
    snprintf(buf, sizeof(buf), "\"0x%016llx\"", (unsigned long long) value);
    xa_json_raw(b, buf);
}

typedef struct XaVariantKeyId {
    uint64_t key;
    XaErrorVariantId variant_id;
} XaVariantKeyId;

static int cmp_variant_key_id(const void *a, const void *b) {
    const XaVariantKeyId *x = (const XaVariantKeyId *) a;
    const XaVariantKeyId *y = (const XaVariantKeyId *) b;
    if (x->key != y->key)
        return (x->key > y->key) - (x->key < y->key);
    return (x->variant_id > y->variant_id) - (x->variant_id < y->variant_id);
}

static void xa_json_specific_variants(XaJsonBuf *b, const XaEffectDatabase *db,
                                      const XaErrorTypeSet *set) {
    uint32_t count = 0;
    for (uint32_t word = 0; word < set->variants.word_count; word++) {
        uint64_t bits = set->variants.words[word];
        while (bits) {
            count++;
            bits &= bits - 1u;
        }
    }
    if (count == 0)
        return;
    XaVariantKeyId *entries = (XaVariantKeyId *) xr_malloc((size_t) count * sizeof(XaVariantKeyId));
    if (!entries) {
        b->ok = false;
        return;
    }
    uint32_t out = 0;
    for (uint32_t word = 0; word < set->variants.word_count; word++) {
        uint64_t bits = set->variants.words[word];
        for (uint32_t bit = 0; bit < 64u; bit++) {
            if ((bits & (UINT64_C(1) << bit)) == 0)
                continue;
            XaErrorVariantId variant_id = word * 64u + bit;
            uint64_t key = xa_effect_db_error_variant_key(db, set->type_id, variant_id);
            entries[out].key = key ? key : fallback_variant_key(set->stable_type_key, variant_id);
            entries[out].variant_id = variant_id;
            out++;
        }
    }
    if (out > 1)
        qsort(entries, out, sizeof(XaVariantKeyId), cmp_variant_key_id);
    const char *type_name = xa_effect_db_error_type_name(db, set->type_id);
    for (uint32_t i = 0; i < out; i++) {
        if (i > 0)
            xa_json_raw(b, ",");
        xa_json_raw(b, "{");
        if (type_name) {
            xa_json_raw(b, "\"type\":");
            xa_json_string(b, type_name);
            xa_json_raw(b, ",");
        }
        xa_json_raw(b, "\"typeKey\":");
        xa_json_hex_u64(b, set->stable_type_key);
        const char *variant_name =
            xa_effect_db_error_variant_name(db, set->type_id, entries[i].variant_id);
        if (variant_name) {
            xa_json_raw(b, ",\"variant\":");
            xa_json_string(b, variant_name);
        }
        xa_json_raw(b, ",\"variantKey\":");
        xa_json_hex_u64(b, entries[i].key);
        xa_json_raw(b, "}");
    }
    xr_free(entries);
}

char *xa_effect_summary_to_json(const XaEffectDatabase *db, const XaEffectSummary *summary,
                                const char *symbol_qualified_name) {
    if (!summary)
        return NULL;
    static const struct {
        XaUnknownReason bit;
        const char *name;
    } unknown_reason_names[] = {
        {XA_UNKNOWN_UNRESOLVED_CALLEE, "unresolvedCallee"},
        {XA_UNKNOWN_MISSING_IMPORTED_EFFECT, "missingImportedEffect"},
        {XA_UNKNOWN_OPEN_VIRTUAL_DISPATCH, "openVirtualDispatch"},
        {XA_UNKNOWN_DYNAMIC_CALL_TARGET, "dynamicCallTarget"},
        {XA_UNKNOWN_NATIVE_CONTRACT_MISSING, "nativeContractMissing"},
        {XA_UNKNOWN_ANALYSIS_LIMIT, "analysisLimit"},
        {XA_UNKNOWN_INVALID_PROGRAM, "invalidProgram"},
        {XA_UNKNOWN_VIEW_INVALIDATION, "viewInvalidationUnknown"},
        {XA_UNKNOWN_ANALYSIS_RESOURCE_FAILURE, "analysisResourceFailure"},
    };

    XaJsonBuf b;
    b.data = NULL;
    b.len = 0;
    b.cap = 0;
    b.ok = true;

    xa_json_raw(&b, "{\"schema\":\"xray.effect-summary.v3\"");
    if (symbol_qualified_name) {
        xa_json_raw(&b, ",\"symbol\":");
        xa_json_string(&b, symbol_qualified_name);
    }
    xa_json_raw(&b, ",\"complete\":");
    xa_json_raw(&b, xa_effect_summary_is_complete(summary) ? "true" : "false");
    xa_json_raw(&b, ",\"errorSetComplete\":");
    xa_json_raw(&b, summary->error_set_completeness == XA_EFFECT_COMPLETE ? "true" : "false");

    xa_json_raw(&b, ",\"semanticEffects\":[");
    static const struct {
        XaSemanticEffect bit;
        const char *name;
    } semantic_effect_names[] = {
        {XA_SEM_EFFECT_ALLOC, "semanticAlloc"},
        {XA_SEM_EFFECT_SUSPEND, "suspend"},
        {XA_SEM_EFFECT_MAY_BLOCK, "mayBlock"},
        {XA_SEM_EFFECT_THREAD_BLOCK, "threadBlock"},
        {XA_SEM_EFFECT_PANIC, "panic"},
        {XA_SEM_EFFECT_ABORT, "abort"},
        {XA_SEM_EFFECT_IO, "io"},
        {XA_SEM_EFFECT_FOREIGN, "foreign"},
        {XA_SEM_EFFECT_SYNC, "sync"},
    };
    bool first_semantic = true;
    for (size_t i = 0; i < sizeof(semantic_effect_names) / sizeof(semantic_effect_names[0]); i++) {
        if (!xa_effect_summary_has_semantic_effect(summary, semantic_effect_names[i].bit))
            continue;
        if (!first_semantic)
            xa_json_raw(&b, ",");
        first_semantic = false;
        xa_json_string(&b, semantic_effect_names[i].name);
    }
    xa_json_raw(&b, "]");
    xa_json_raw(&b, ",\"unknownSemanticEffects\":[");
    bool first_unknown_semantic = true;
    for (size_t i = 0; i < sizeof(semantic_effect_names) / sizeof(semantic_effect_names[0]); i++) {
        if ((summary->unknown_semantic_effects &
             (XaSemanticEffectSet) semantic_effect_names[i].bit) == 0)
            continue;
        if (!first_unknown_semantic)
            xa_json_raw(&b, ",");
        first_unknown_semantic = false;
        xa_json_string(&b, semantic_effect_names[i].name);
    }
    xa_json_raw(&b, "]");
    xa_json_raw(&b, ",\"containsUnsafeOp\":");
    xa_json_raw(&b, summary->contains_unsafe_op ? "true" : "false");
    xa_json_raw(&b, ",\"requiresUnsafeAtCall\":");
    xa_json_raw(&b, summary->requires_unsafe_at_call ? "true" : "false");

    xa_json_raw(&b, ",\"errors\":[");
    bool first_error = true;
    for (uint32_t i = 0; i < summary->escaping.count; i++) {
        const XaErrorTypeSet *set = &summary->escaping.types[i];
        const char *type_name = xa_effect_db_error_type_name(db, set->type_id);
        if (set->all_variants) {
            if (!first_error)
                xa_json_raw(&b, ",");
            first_error = false;
            xa_json_raw(&b, "{");
            if (type_name) {
                xa_json_raw(&b, "\"type\":");
                xa_json_string(&b, type_name);
                xa_json_raw(&b, ",");
            }
            xa_json_raw(&b, "\"typeKey\":");
            xa_json_hex_u64(&b, set->stable_type_key);
            xa_json_raw(&b, ",\"allVariants\":true}");
        } else {
            if (!first_error)
                xa_json_raw(&b, ",");
            first_error = false;
            xa_json_specific_variants(&b, db, set);
        }
    }
    xa_json_raw(&b, "]");

    xa_json_raw(&b, ",\"errorUnknownReasons\":[");
    bool first_error_reason = true;
    for (size_t i = 0; i < sizeof(unknown_reason_names) / sizeof(unknown_reason_names[0]); i++) {
        if ((summary->error_unknown_reasons & (XaUnknownReasonSet) unknown_reason_names[i].bit) ==
            0)
            continue;
        if (!first_error_reason)
            xa_json_raw(&b, ",");
        first_error_reason = false;
        xa_json_string(&b, unknown_reason_names[i].name);
    }
    xa_json_raw(&b, "]");

    xa_json_raw(&b, ",\"unknownReasons\":[");
    bool first_reason = true;
    for (size_t i = 0; i < sizeof(unknown_reason_names) / sizeof(unknown_reason_names[0]); i++) {
        if ((summary->unknown_reasons & (XaUnknownReasonSet) unknown_reason_names[i].bit) == 0)
            continue;
        if (!first_reason)
            xa_json_raw(&b, ",");
        first_reason = false;
        xa_json_string(&b, unknown_reason_names[i].name);
    }
    xa_json_raw(&b, "]");

    xa_json_raw(&b, ",\"fingerprint\":");
    xa_json_hex_u64(&b, xa_effect_summary_fingerprint(db, summary));
    xa_json_raw(&b, "}");

    if (!b.ok) {
        xr_free(b.data);
        return NULL;
    }
    return b.data;
}

static bool summary_copy(XaEffectSummary *dst, const XaEffectSummary *src) {
    xa_effect_summary_init(dst);
    dst->semantic_effects = src->semantic_effects;
    dst->unknown_semantic_effects = src->unknown_semantic_effects;
    dst->error_set_completeness = src->error_set_completeness;
    dst->error_unknown_reasons = src->error_unknown_reasons;
    dst->completeness = src->completeness;
    dst->unknown_reasons = src->unknown_reasons;
    dst->contains_unsafe_op = src->contains_unsafe_op;
    dst->requires_unsafe_at_call = src->requires_unsafe_at_call;
    dst->fingerprint = src->fingerprint;
    dst->revision = src->revision;
    if (src->escaping.count > 0) {
        dst->escaping.types =
            (XaErrorTypeSet *) xr_calloc(src->escaping.count, sizeof(XaErrorTypeSet));
        if (!dst->escaping.types)
            return false;
        dst->escaping.count = src->escaping.count;
        dst->escaping.capacity = src->escaping.count;
        for (uint32_t i = 0; i < src->escaping.count; i++) {
            dst->escaping.types[i].type_id = src->escaping.types[i].type_id;
            dst->escaping.types[i].stable_type_key = src->escaping.types[i].stable_type_key;
            dst->escaping.types[i].all_variants = src->escaping.types[i].all_variants;
            if (!bitset_copy(&dst->escaping.types[i].variants, &src->escaping.types[i].variants)) {
                xa_effect_summary_clear(dst);
                return false;
            }
        }
    }
    if (src->root_count > 0) {
        if (!summary_add_roots(dst, src, NULL)) {
            xa_effect_summary_clear(dst);
            return false;
        }
    }
    return true;
}

static bool summary_equals(const XaEffectSummary *a, const XaEffectSummary *b) {
    if (a == b)
        return true;
    if (!a || !b)
        return false;
    if (a->semantic_effects != b->semantic_effects ||
        a->unknown_semantic_effects != b->unknown_semantic_effects ||
        a->error_set_completeness != b->error_set_completeness ||
        a->error_unknown_reasons != b->error_unknown_reasons ||
        a->completeness != b->completeness || a->unknown_reasons != b->unknown_reasons ||
        a->contains_unsafe_op != b->contains_unsafe_op ||
        a->requires_unsafe_at_call != b->requires_unsafe_at_call ||
        a->escaping.count != b->escaping.count)
        return false;
    for (uint32_t i = 0; i < a->escaping.count; i++) {
        const XaErrorTypeSet *as = &a->escaping.types[i];
        const XaErrorTypeSet *bs = &b->escaping.types[i];
        if (as->type_id != bs->type_id || as->stable_type_key != bs->stable_type_key ||
            as->all_variants != bs->all_variants)
            return false;
        if (!bitset_equals(&as->variants, &bs->variants))
            return false;
    }
    return true;
}

XaEffectId xa_effect_db_intern(XaEffectDatabase *db, const XaEffectSummary *summary) {
    if (!db || !summary || xa_effect_summary_has_resource_failure(summary))
        return XA_EFFECT_NONE;
    XaEffectSummary normalized;
    if (!summary_copy(&normalized, summary))
        return XA_EFFECT_NONE;
    normalized.fingerprint = xa_effect_summary_fingerprint(db, &normalized);
    for (uint32_t i = 0; i < db->summary_count; i++) {
        if (db->summaries[i].fingerprint == normalized.fingerprint &&
            summary_equals(&db->summaries[i], &normalized)) {
            bool roots_changed = false;
            if (!summary_add_roots(&db->summaries[i], &normalized, &roots_changed)) {
                xa_effect_summary_clear(&normalized);
                return XA_EFFECT_NONE;
            }
            if (roots_changed)
                db->summaries[i].revision = ++db->next_revision;
            xa_effect_summary_clear(&normalized);
            return i + 1u;
        }
    }
    if (!grow_array((void **) &db->summaries, &db->summary_capacity, sizeof(XaEffectSummary),
                    db->summary_count + 1u)) {
        xa_effect_summary_clear(&normalized);
        return XA_EFFECT_NONE;
    }
    normalized.revision = ++db->next_revision;
    db->summaries[db->summary_count] = normalized;
    db->summary_count++;
    return db->summary_count;
}

XaEffectId xa_effect_db_import(XaEffectDatabase *dst, const XaEffectDatabase *src,
                               XaEffectId src_id) {
    const XaEffectSummary *source = xa_effect_db_get(src, src_id);
    if (!dst || !src || !source)
        return XA_EFFECT_NONE;
    if (dst == src)
        return src_id;

    XaEffectSummary imported;
    xa_effect_summary_init(&imported);
    imported.semantic_effects = source->semantic_effects;
    imported.unknown_semantic_effects = source->unknown_semantic_effects;
    imported.error_set_completeness = source->error_set_completeness;
    imported.error_unknown_reasons = source->error_unknown_reasons;
    imported.completeness = source->completeness;
    imported.unknown_reasons = source->unknown_reasons;
    imported.contains_unsafe_op = source->contains_unsafe_op;
    imported.requires_unsafe_at_call = source->requires_unsafe_at_call;

    bool ok = true;
    for (uint32_t i = 0; ok && i < source->escaping.count; i++) {
        const XaErrorTypeSet *source_set = &source->escaping.types[i];
        const XaErrorTypeInfo *source_type = db_type_info(src, source_set->type_id);
        uint64_t type_key = source_set->stable_type_key;
        if (!source_type || !type_key) {
            ok = false;
            break;
        }
        XaErrorTypeId type_id =
            xa_effect_db_register_error_type(dst, type_key, source_type->type_handle);
        if (type_id == XA_ERROR_TYPE_NONE) {
            ok = false;
            break;
        }
        xa_effect_db_set_error_type_name(dst, type_id, source_type->qualified_name);
        for (uint32_t variant = 0; ok && variant < source_type->variant_count; variant++) {
            XaErrorVariantId imported_variant = xa_effect_db_register_error_variant(
                dst, type_id, source_type->variants[variant].stable_key);
            if (imported_variant == XA_ERROR_VARIANT_INVALID) {
                ok = false;
                break;
            }
            xa_effect_db_set_error_variant_name(dst, type_id, imported_variant,
                                                source_type->variants[variant].name);
            if (!source_set->all_variants && xa_bitset_test(&source_set->variants, variant))
                ok = xa_effect_summary_add_variant(dst, &imported, type_id, imported_variant);
        }
        if (ok && source_set->all_variants)
            ok = xa_effect_summary_add_all_variants(dst, &imported, type_id);
    }

    XaEffectId result = ok ? xa_effect_db_intern(dst, &imported) : XA_EFFECT_NONE;
    xa_effect_summary_clear(&imported);
    return result;
}

const XaEffectSummary *xa_effect_db_get(const XaEffectDatabase *db, XaEffectId id) {
    if (!db || id == XA_EFFECT_NONE)
        return NULL;
    uint32_t index = id - 1u;
    if (index >= db->summary_count)
        return NULL;
    return &db->summaries[index];
}

uint32_t xa_effect_db_summary_count(const XaEffectDatabase *db) {
    return db ? db->summary_count : 0;
}
