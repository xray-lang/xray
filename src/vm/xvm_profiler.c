/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_profiler.c - VM performance profiler implementation
 */

#include "xvm_profiler.h"
#include "../base/xchecks.h"

#if XR_ENABLE_VM_PROFILER

#include <stdio.h>
#include <stdlib.h>
#include "xdebug.h"

VMProfiler g_vm_profiler;

// Opcode names (must match OpCode enum in xchunk.h exactly!)
static const char* opcode_names[NUM_OPCODES] = {
    // Load/Move (0-6)
    [OP_MOVE] = "MOVE",
    [OP_LOADI] = "LOADI",
    [OP_LOADF] = "LOADF",
    [OP_LOADK] = "LOADK",
    [OP_LOADNULL] = "LOADNULL",
    [OP_LOADTRUE] = "LOADTRUE",
    [OP_LOADFALSE] = "LOADFALSE",
    
    // Arithmetic (7-21)
    [OP_ADD] = "ADD",
    [OP_ADDI] = "ADDI",
    [OP_ADDK] = "ADDK",
    [OP_SUB] = "SUB",
    [OP_SUBI] = "SUBI",
    [OP_SUBK] = "SUBK",
    [OP_MUL] = "MUL",
    [OP_MULI] = "MULI",
    [OP_MULK] = "MULK",
    [OP_DIV] = "DIV",
    [OP_DIVK] = "DIVK",
    [OP_MOD] = "MOD",
    [OP_MODK] = "MODK",
    [OP_UNM] = "UNM",
    [OP_NOT] = "NOT",
    
    // String buffer
    [OP_STRBUF_NEW] = "STRBUF_NEW",
    [OP_STRBUF_APPEND] = "STRBUF_APPEND",
    [OP_STRBUF_FINISH] = "STRBUF_FINISH",
    
    // Bitwise (26-31)
    [OP_BAND] = "BAND",
    [OP_BOR] = "BOR",
    [OP_BXOR] = "BXOR",
    [OP_BNOT] = "BNOT",
    [OP_SHL] = "SHL",
    [OP_SHR] = "SHR",
    
    // Comparison (jump-based)
    [OP_EQ] = "EQ",
    [OP_EQK] = "EQK",
    [OP_EQI] = "EQI",
    [OP_LT] = "LT",
    [OP_LTI] = "LTI",
    [OP_LE] = "LE",
    [OP_LEI] = "LEI",
    
    // Comparison expression-based (46-54)
    [OP_CMP_EQ] = "CMP_EQ",
    [OP_CMP_NE] = "CMP_NE",
    [OP_CMP_EQ_STRICT] = "CMP_EQ_STRICT",
    [OP_CMP_NE_STRICT] = "CMP_NE_STRICT",
    [OP_CMP_LT] = "CMP_LT",
    [OP_CMP_LE] = "CMP_LE",
    [OP_IS] = "IS",
    [OP_CHECKTYPE] = "CHECKTYPE",
    [OP_ISNULL] = "ISNULL",
    [OP_ISNULL_SET] = "ISNULL_SET",
    
    // Control flow
    [OP_JMP] = "JMP",
    [OP_TEST] = "TEST",
    [OP_TESTSET] = "TESTSET",
    [OP_CALL] = "CALL",
    [OP_CALL_KEEP] = "CALL_KEEP",
    [OP_CALLSELF] = "CALLSELF",
    [OP_TAILCALL] = "TAILCALL",
    [OP_RETURN] = "RETURN",
    [OP_RETURN0] = "RETURN0",
    [OP_RETURN1] = "RETURN1",
    
    // Container creation
    [OP_NEWARRAY] = "NEWARRAY",
    [OP_NEWMAP] = "NEWMAP",
    [OP_NEWSET] = "NEWSET",
    [OP_NEWSTRINGBUILDER] = "NEWSTRINGBUILDER",
    
    // Array operations
    [OP_ARRAY_GET] = "ARRAY_GET",
    [OP_ARRAY_GETC] = "ARRAY_GETC",
    [OP_ARRAY_SET] = "ARRAY_SET",
    [OP_ARRAY_SETC] = "ARRAY_SETC",
    [OP_ARRAY_PUSH] = "ARRAY_PUSH",
    [OP_ARRAY_LEN] = "ARRAY_LEN",
    [OP_ARRAY_INIT] = "ARRAY_INIT",
    
    // Map operations (77-80)
    [OP_MAP_GET] = "MAP_GET",
    [OP_MAP_GETK] = "MAP_GETK",
    [OP_MAP_SET] = "MAP_SET",
    [OP_MAP_SETK] = "MAP_SETK",
    
    // Generic index (81-82)
    [OP_INDEX_GET] = "INDEX_GET",
    [OP_INDEX_SET] = "INDEX_SET",
    
    // Slice (83)
    [OP_SLICE] = "SLICE",
    
    // Closure & Upvalues
    [OP_CLOSURE]   = "CLOSURE",
    
    // OOP - Class building
    [OP_CLASS_CREATE_FROM_DESCRIPTOR] = "CLASS_CREATE_FROM_DESCRIPTOR",
    [OP_CLINIT_CALL] = "CLINIT_CALL",
    
    // OOP - Class operations (96-101)
    [OP_INHERIT] = "INHERIT",
    [OP_GETPROP] = "GETPROP",
    [OP_SETPROP] = "SETPROP",
    [OP_GETSUPER] = "GETSUPER",
    [OP_INVOKE] = "INVOKE",
    [OP_INVOKE_TAIL] = "INVOKE_TAIL",
    [OP_SUPERINVOKE] = "SUPERINVOKE",
    
    // OOP - Optimized (102-108)
    [OP_INVOKE_DIRECT] = "INVOKE_DIRECT",
    [OP_INVOKE_BUILTIN] = "INVOKE_BUILTIN",
    
    // OOP - Runtime support
    [OP_ABSTRACT_ERROR] = "ABSTRACT_ERROR",
    [OP_SET_STORAGE_CTX] = "SET_STORAGE_CTX",
    
    // Shared conversion (112-119)
    [OP_TO_SHARED] = "TO_SHARED",
    [OP_MAP_SETKS] = "MAP_SETKS",
    
    // Instance field access
    [OP_GETFIELD] = "GETFIELD",
    [OP_SETFIELD] = "SETFIELD",
    [OP_GETFIELD_IC] = "GETFIELD_IC",
    
    // Json dynamic object (122-127)
    [OP_NEWJSON] = "NEWJSON",
    [OP_JSON_GET] = "JSON_GET",
    [OP_JSON_SET] = "JSON_SET",
    [OP_JSON_GETK] = "JSON_GETK",
    [OP_JSON_SETK] = "JSON_SETK",
    [OP_JSON_INIT] = "JSON_INIT",
    [OP_JSON_INIT_I] = "JSON_INIT_I",
    [OP_JSON_INIT_N] = "JSON_INIT_N",
    
    // Builtin globals
    [OP_GETBUILTIN] = "GETBUILTIN",
    
    // Built-in functions
    [OP_PRINT] = "PRINT",
    [OP_TYPEOF] = "TYPEOF",
    [OP_DUMP] = "DUMP",
    [OP_TOINT] = "TOINT",
    [OP_TOFLOAT] = "TOFLOAT",
    [OP_TOSTRING] = "TOSTRING",
    [OP_TOBOOL] = "TOBOOL",
    [OP_COPY] = "COPY",
    [OP_CHR] = "CHR",
    
    // Enum
    [OP_ENUM_ACCESS] = "ENUM_ACCESS",
    [OP_ENUM_CONVERT] = "ENUM_CONVERT",
    [OP_ENUM_NAME] = "ENUM_NAME",
    
    // Exception handling (147-152)
    [OP_TRY] = "TRY",
    [OP_CATCH] = "CATCH",
    [OP_FINALLY] = "FINALLY",
    [OP_END_TRY] = "END_TRY",
    [OP_THROW] = "THROW",
    
    // Register spill (153-154)
    [OP_SPILL] = "SPILL",
    [OP_RELOAD] = "RELOAD",
    
    // Box/Unbox (typed storage boundary)
    [OP_BOX_I64] = "BOX_I64",
    [OP_BOX_F64] = "BOX_F64",
    [OP_UNBOX_I64] = "UNBOX_I64",
    [OP_UNBOX_F64] = "UNBOX_F64",
    
    // Unchecked array access (167)
    [OP_ARRAY_GET_NOCHECK] = "ARRAY_GET_NOCHECK",
    
    // Map counter (168)
    [OP_MAP_INCREMENT] = "MAP_INCREMENT",
    
    // String optimization (169-170)
    [OP_SUBSTRING] = "SUBSTRING",
    [OP_STR_REPEAT] = "STR_REPEAT",
    
    // Module system (171-173)
    [OP_IMPORT] = "IMPORT",
    [OP_EXPORT] = "EXPORT",
    [OP_EXPORT_ALL] = "EXPORT_ALL",
    
    // Assertion (174-175)
    [OP_ASSERT] = "ASSERT",
    [OP_ASSERT_EQ] = "ASSERT_EQ",
    [OP_ASSERT_NE] = "ASSERT_NE",
    
    // Regex (176)
    [OP_REGEX_COMPILE] = "REGEX_COMPILE",
    
    // Coroutine (177-194)
    [OP_GO] = "GO",
    [OP_GO_INVOKE] = "GO_INVOKE",
    [OP_SPAWN_CONT] = "SPAWN_CONT",
    [OP_AWAIT] = "AWAIT",
    [OP_AWAIT_TIMEOUT] = "AWAIT_TIMEOUT",
    [OP_AWAIT_ALL] = "AWAIT_ALL",
    [OP_AWAIT_ANY] = "AWAIT_ANY",
    [OP_YIELD] = "YIELD",
    [OP_CANCELLED] = "CANCELLED",
    [OP_LOCK_THREAD] = "LOCK_THREAD",
    [OP_UNLOCK_THREAD] = "UNLOCK_THREAD",
    [OP_SET_LOCAL] = "SET_LOCAL",
    [OP_GET_LOCAL] = "GET_LOCAL",
    [OP_SET_PRIORITY] = "SET_PRIORITY",
    [OP_CORO_CTRL] = "CORO_CTRL",
    
    // Channel (195-204)
    [OP_CHAN_NEW] = "CHAN_NEW",
    [OP_CHAN_SEND] = "CHAN_SEND",
    [OP_CHAN_RECV] = "CHAN_RECV",
    [OP_CHAN_TRY_SEND] = "CHAN_TRY_SEND",
    [OP_CHAN_TRY_RECV] = "CHAN_TRY_RECV",
    [OP_CHAN_SEND_TIMEOUT] = "CHAN_SEND_TIMEOUT",
    [OP_CHAN_RECV_TIMEOUT] = "CHAN_RECV_TIMEOUT",
    [OP_CHAN_CLOSE] = "CHAN_CLOSE",
    [OP_CHAN_IS_CLOSED] = "CHAN_IS_CLOSED",
    
    // Select (205-207)
    [OP_SELECT_START] = "SELECT_START",
    [OP_SELECT_CASE] = "SELECT_CASE",
    [OP_SELECT_END] = "SELECT_END",
    
    // Defer (208)
    [OP_DEFER] = "DEFER",
    
    // Bytes (209)
    [OP_BYTES_NEW] = "BYTES_NEW",
    
    // Structured concurrency (210-211)
    [OP_SCOPE_ENTER] = "SCOPE_ENTER",
    [OP_SCOPE_EXIT] = "SCOPE_EXIT",
    
    // Coroutine-friendly (212-214)
    [OP_SLEEP] = "SLEEP",
    [OP_TIME_AFTER] = "TIME_AFTER",
    [OP_SELECT_BLOCK] = "SELECT_BLOCK",
    
    // Shared variables (215-216)
    [OP_GETSHARED] = "GETSHARED",
    [OP_SETSHARED] = "SETSHARED",
    
    // Tail recursion loop
    [OP_LOOP_BACK] = "LOOP_BACK",

    // Struct native storage
    [OP_NEW_STRUCT] = "NEW_STRUCT",
    [OP_STRUCT_GET] = "STRUCT_GET",
    [OP_STRUCT_SET] = "STRUCT_SET",
    [OP_STRUCT_COPY] = "STRUCT_COPY",

    // Placeholder
    [OP_NOP] = "NOP",
};

// Structure for sorting opcode statistics
typedef struct {
    int op;
    VMOpStats stats;
} SortedOpStats;

// Comparison function for sorting by count
static int compare_by_count(const void *a, const void *b) {
    const SortedOpStats *sa = (const SortedOpStats *)a;
    const SortedOpStats *sb = (const SortedOpStats *)b;
    
    if (sb->stats.count > sa->stats.count) return 1;
    if (sb->stats.count < sa->stats.count) return -1;
    return 0;
}

#if XR_PROFILE_TIMING
// Comparison function for sorting by time
static int compare_by_time(const void *a, const void *b) {
    const SortedOpStats *sa = (const SortedOpStats *)a;
    const SortedOpStats *sb = (const SortedOpStats *)b;
    
    if (sb->stats.total_ns > sa->stats.total_ns) return 1;
    if (sb->stats.total_ns < sa->stats.total_ns) return -1;
    return 0;
}
#endif // Get opcode name
static const char* get_opcode_name(int op) {
    if (op >= 0 && op < NUM_OPCODES && opcode_names[op] != NULL) {
        return opcode_names[op];
    }
    return "UNKNOWN";
}

// Print performance report
void vm_profiler_report(void) {
    XR_DCHECK(sizeof(opcode_names)/sizeof(opcode_names[0]) == NUM_OPCODES,
              "opcode_names table size mismatch with NUM_OPCODES");
    uint64_t end_time_ms = vm_profiler_get_ms();
    
    printf("\n");
    printf("======== VM Performance Report ========\n");
    printf("\n");
    
#if XR_PROFILE_TIMING
    double total_sec = (end_time_ms - g_vm_profiler.start_time_ms) / 1000.0;
#else
    double total_sec = 0.0;
#endif
    printf("Summary\n");
    printf("  Total instructions: %llu\n", (unsigned long long)g_vm_profiler.total_instructions);
#if XR_PROFILE_TIMING
    printf("  Runtime: %.3f sec\n", total_sec);
    if (total_sec > 0) {
        printf("  Speed: %.2f MIPS\n", g_vm_profiler.total_instructions / total_sec / 1000000.0);
    }
#else
    printf("  (Timing disabled, set XR_PROFILE_TIMING=1 to enable)\n");
#endif
    printf("\n");
    
    SortedOpStats sorted_stats[256];
    int count = 0;
    
    for (int i = 0; i < 256; i++) {
        if (g_vm_profiler.op_stats[i].count > 0) {
            sorted_stats[count].op = i;
            sorted_stats[count].stats = g_vm_profiler.op_stats[i];
            count++;
        }
    }
    
    if (count == 0) {
        printf("No instruction stats collected\n");
        return;
    }
    
    qsort(sorted_stats, count, sizeof(SortedOpStats), compare_by_count);
    
    printf("Hot Instructions (by count, Top 20)\n");
    printf("%-20s %12s %8s\n", "Instruction", "Count", "Percent");
    printf("%-20s %12s %8s\n", "--------------------", "------------", "--------");
    
    for (int i = 0; i < count && i < 20; i++) {
        int op = sorted_stats[i].op;
        VMOpStats *stats = &sorted_stats[i].stats;
        const char *name = get_opcode_name(op);
        double percent = (double)stats->count * 100.0 / g_vm_profiler.total_instructions;
        
        printf("%-20s %12llu %7.2f%%\n", 
               name, (unsigned long long)stats->count, percent);
    }
    printf("\n");
    
#if XR_PROFILE_TIMING
    qsort(sorted_stats, count, sizeof(SortedOpStats), compare_by_time);
    
    printf("Slow Instructions (by time, Top 20)\n");
    printf("%-20s %12s %12s %10s\n", "Instruction", "Count", "Total(ms)", "Avg(ns)");
    printf("%-20s %12s %12s %10s\n", "--------------------", "------------", "------------", "----------");
    
    for (int i = 0; i < count && i < 20; i++) {
        int op = sorted_stats[i].op;
        VMOpStats *stats = &sorted_stats[i].stats;
        const char *name = get_opcode_name(op);
        double ms = stats->total_ns / 1000000.0;
        double avg_ns = (double)stats->total_ns / stats->count;
        
        printf("%-20s %12llu %12.2f %10.1f\n", 
               name, (unsigned long long)stats->count, ms, avg_ns);
    }
    printf("\n");
#endif
    
    printf("All Executed Instructions (%d types)\n", count);
    printf("%-20s %12s\n", "Instruction", "Count");
    printf("%-20s %12s\n", "--------------------", "------------");
    
    for (int i = 0; i < count; i++) {
        int op = sorted_stats[i].op;
        VMOpStats *stats = &sorted_stats[i].stats;
        const char *name = get_opcode_name(op);
        
        printf("%-20s %12llu\n", 
               name, (unsigned long long)stats->count);
    }
    printf("\n");
}

#endif
