/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xforward_decl.h - Centralized forward declarations
 *
 * KEY CONCEPT:
 *   Single place for forward declarations of all core types.
 *   Include this instead of ad-hoc typedef in each header.
 */

#ifndef XFORWARD_DECL_H
#define XFORWARD_DECL_H

/* ========== Core Types ========== */

typedef struct XrayIsolate XrayIsolate;

/* ========== Memory Management ========== */

typedef struct XrGC XrGC;
typedef struct XrMemoryTracker XrMemoryTracker;

/* ========== Symbols and Globals ========== */

typedef struct XrSymbolTable XrSymbolTable;
typedef struct XrGlobalsTable XrGlobalsTable;
typedef struct XrGlobalObject XrGlobalObject;

/* ========== Type System ========== */

typedef struct XrTypeRegistry XrTypeRegistry;
typedef struct XrTypeInferContext XrTypeInferContext;
typedef struct XrTypeTable XrTypeTable;

/* ========== Module System ========== */

typedef struct XrModule XrModule;
typedef struct XrModuleRegistry XrModuleRegistry;

/* ========== Strings ========== */

typedef struct XrStringPool XrStringPool;
typedef struct XrString XrString;

/* ========== Compiler ========== */

typedef struct XrProto XrProto;
typedef struct AstNode AstNode;
typedef struct XrCompiler XrCompiler;
typedef struct XrCompilerContext XrCompilerContext;

/* ========== Runtime Objects ========== */

typedef struct XrClass XrClass;
typedef struct XrInstance XrInstance;
typedef struct XrClosure XrClosure;
typedef struct XrArray XrArray;
typedef struct XrMap XrMap;
typedef struct XrSet XrSet;
typedef struct XrCoroutine XrCoroutine;
typedef struct XrChannel XrChannel;
typedef struct XrJson XrJson;
typedef struct XrShape XrShape;
typedef struct XrBigInt XrBigInt;
typedef struct XrIterator XrIterator;
typedef struct XrException XrException;
typedef struct XrStringBuilder XrStringBuilder;
typedef struct XrCFunction XrCFunction;
typedef struct XrBoundMethod XrBoundMethod;

/* ========== Object Pools ========== */

typedef struct XrJsonPoolManager XrJsonPoolManager;

/* ========== Coroutine Scheduling ========== */

typedef struct XrCoroState XrCoroState;
typedef struct XrRuntime XrRuntime;
typedef struct XrWorker XrWorker;
typedef struct XrProc XrProc;
typedef struct XrMachine XrMachine;

/* ========== VM State ========== */

typedef struct XrVMState XrVMState;
typedef struct XrVMContext XrVMContext;
typedef struct XrBcCallFrame XrBcCallFrame;

#endif  // XFORWARD_DECL_H
