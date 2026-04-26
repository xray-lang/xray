/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xopcode_def.h - Single source of truth for the VM opcode table.
 *
 * KEY CONCEPT:
 *   X-macro listing every VM opcode exactly once. Three downstream
 *   consumers expand it without ever maintaining their own list:
 *
 *     - xchunk.h           -> typedef enum { OP_*, NUM_OPCODES }
 *     - xopcode_info.c     -> name / format / disassembly description
 *     - xvm_jumptab.h      -> &&L_OP_* computed-goto label table
 *
 *   Adding an opcode means adding exactly one XR_OPCODE_TABLE entry
 *   here. The expansion order is the canonical enum order, so the
 *   numeric value of every opcode stays stable between consumers.
 *
 * ENTRY SHAPE:
 *
 *   _(NAME, FMT_TAG, "human-readable description")
 *
 *   - NAME is the bare suffix; the macros prepend OP_ / L_OP_ as
 *     each consumer needs.
 *   - FMT_TAG is one of the FMT_* enumerators in xopcode_info.h.
 *   - The description string is what disassembler / debugger
 *     dumps print next to the mnemonic.
 */

#ifndef XOPCODE_DEF_H
#define XOPCODE_DEF_H

#define XR_OPCODE_TABLE(_) \
    _(MOVE                      , FMT_AB    , "R[A] = R[B]") \
    _(LOADI                     , FMT_AsBx  , "R[A] = sBx") \
    _(LOADF                     , FMT_AsBx  , "R[A] = (float)sBx") \
    _(LOADK                     , FMT_ABx   , "R[A] = K[Bx]") \
    _(LOADNULL                  , FMT_A     , "R[A] = null") \
    _(LOADTRUE                  , FMT_A     , "R[A] = true") \
    _(LOADFALSE                 , FMT_A     , "R[A] = false") \
    _(ADD                       , FMT_ABC   , "R[A] = R[B] + R[C]") \
    _(ADDI                      , FMT_AB_sC , "R[A] = R[B] + sC") \
    _(ADDK                      , FMT_ABC   , "R[A] = R[B] + K[C]") \
    _(SUB                       , FMT_ABC   , "R[A] = R[B] - R[C]") \
    _(SUBI                      , FMT_AB_sC , "R[A] = R[B] - sC") \
    _(SUBK                      , FMT_ABC   , "R[A] = R[B] - K[C]") \
    _(MUL                       , FMT_ABC   , "R[A] = R[B] * R[C]") \
    _(MULI                      , FMT_AB_sC , "R[A] = R[B] * sC") \
    _(MULK                      , FMT_ABC   , "R[A] = R[B] * K[C]") \
    _(DIV                       , FMT_ABC   , "R[A] = R[B] / R[C]") \
    _(DIVK                      , FMT_ABC   , "R[A] = R[B] / K[C]") \
    _(MOD                       , FMT_ABC   , "R[A] = R[B] % R[C]") \
    _(MODK                      , FMT_ABC   , "R[A] = R[B] % K[C]") \
    _(UNM                       , FMT_AB    , "R[A] = -R[B]") \
    _(NOT                       , FMT_AB    , "R[A] = !R[B]") \
    _(STRBUF_NEW                , FMT_A     , "R[A] = new_strbuf()") \
    _(STRBUF_APPEND             , FMT_AB    , "strbuf(R[A]).append(R[B])") \
    _(STRBUF_FINISH             , FMT_A     , "R[A] = strbuf(R[A]).to_string()") \
    _(BAND                      , FMT_ABC   , "R[A] = R[B] & R[C]") \
    _(BOR                       , FMT_ABC   , "R[A] = R[B] | R[C]") \
    _(BXOR                      , FMT_ABC   , "R[A] = R[B] ^ R[C]") \
    _(BNOT                      , FMT_AB    , "R[A] = ~R[B]") \
    _(SHL                       , FMT_ABC   , "R[A] = R[B] << R[C]") \
    _(SHR                       , FMT_ABC   , "R[A] = R[B] >> R[C]") \
    _(EQ                        , FMT_AB_IMM, "if (R[A] == R[B]) != k then PC++") \
    _(EQK                       , FMT_AB_IMM, "if (R[A] == K[B]) != k then PC++") \
    _(EQI                       , FMT_AsB_C , "if (R[A] == sB) != k then PC++") \
    _(LT                        , FMT_AB_IMM, "if (R[A] < R[B]) != k then PC++") \
    _(LTI                       , FMT_AsB_C , "if (R[A] < sB) != k then PC++") \
    _(LE                        , FMT_AB_IMM, "if (R[A] <= R[B]) != k then PC++") \
    _(LEI                       , FMT_AsB_C , "if (R[A] <= sB) != k then PC++") \
    _(CMP_EQ                    , FMT_ABC   , "R[A] = (R[B] == R[C])") \
    _(CMP_NE                    , FMT_ABC   , "R[A] = (R[B] != R[C])") \
    _(CMP_EQ_STRICT             , FMT_ABC   , "R[A] = (R[B] === R[C])") \
    _(CMP_NE_STRICT             , FMT_ABC   , "R[A] = (R[B] !== R[C])") \
    _(CMP_LT                    , FMT_ABC   , "R[A] = (R[B] < R[C])") \
    _(CMP_LE                    , FMT_ABC   , "R[A] = (R[B] <= R[C])") \
    _(IS                        , FMT_ABC   , "R[A] = (R[B] is Type[C])") \
    _(CHECKTYPE                 , FMT_AB    , "assert R[A] is Type[K(B)]") \
    _(ISNULL                    , FMT_AB_IMM, "if (R[A]==null) != k then PC++") \
    _(ISNULL_SET                , FMT_AB    , "R[A] = (R[B] == null)") \
    _(JMP                       , FMT_sJ    , "PC += sJ") \
    _(TEST                      , FMT_AB_IMM, "if R[A] != k then PC++") \
    _(TESTSET                   , FMT_ABC   , "if R[B] != k then PC++ else R[A]=R[B]") \
    _(CALL                      , FMT_ABC   , "R[A] = R[A](R[A+1]...R[A+B-1])") \
    _(CALL_KEEP                 , FMT_ABC   , "R[C] = R[A](R[A+1]...R[A+B]); R[A] kept") \
    _(CALL_STATIC               , FMT_ABC   , "R[A](R[A+1]...R[A+B-1]) - known closure") \
    _(CALLSELF                  , FMT_ABC   , "recursive call opt") \
    _(TAILCALL                  , FMT_ABC   , "tail call opt") \
    _(RETURN                    , FMT_ABC   , "return R[A]...R[A+B-2]") \
    _(RETURN0                   , FMT_NONE  , "return (fast)") \
    _(RETURN1                   , FMT_A     , "return R[A] (fast)") \
    _(NEWARRAY                  , FMT_ABC   , "R[A] = [], B=capacity, C=storage") \
    _(NEWMAP                    , FMT_ABC   , "R[A] = #{}, B=capacity, C=storage") \
    _(NEWSET                    , FMT_AB    , "R[A] = #[], B=storage") \
    _(NEWSTRINGBUILDER          , FMT_AB    , "R[A] = new StringBuilder(), B=storage") \
    _(NEWRANGE                  , FMT_ABC   , "R[A] = Range(R[B], R[C])") \
    _(RANGE_UNPACK              , FMT_AB    , "R[A]=start, R[A+1]=end, R[A+2]=step from Range R[B]") \
    _(ARRAY_GET                 , FMT_ABC   , "R[A] = R[B]:Array[R[C]]") \
    _(ARRAY_GETC                , FMT_ABC   , "R[A] = R[B]:Array[C]") \
    _(ARRAY_SET                 , FMT_ABC   , "R[A]:Array[R[B]] = R[C]") \
    _(ARRAY_SETC                , FMT_ABC   , "R[A]:Array[B] = R[C]") \
    _(ARRAY_PUSH                , FMT_AB    , "R[A]:Array.push(R[B])") \
    _(ARRAY_LEN                 , FMT_AB    , "R[A] = len(R[B]:Array)") \
    _(ARRAY_INIT                , FMT_AB_IMM, "R[A][1..B] = R[A+1..A+B]") \
    _(MAP_GET                   , FMT_ABC   , "R[A] = R[B]:Map[R[C]]") \
    _(MAP_GETK                  , FMT_ABC   , "R[A] = R[B]:Map[K[C]]") \
    _(MAP_SET                   , FMT_ABC   , "R[A]:Map[R[B]] = R[C]") \
    _(MAP_SETK                  , FMT_ABC   , "R[A]:Map[K[B]] = R[C]") \
    _(INDEX_GET                 , FMT_ABC   , "R[A] = R[B][R[C]] (runtime dispatch)") \
    _(INDEX_SET                 , FMT_ABC   , "R[A][R[B]] = R[C] (runtime dispatch)") \
    _(SLICE                     , FMT_ABC   , "R[A] = R[B][R[C]:R[C+1]] (slice)") \
    _(CLOSURE                   , FMT_PROTO , "R[A] = closure(XrProto[Bx])") \
    _(UPVAL_GET                 , FMT_ABC   , "R[A] = cl->upvals[B]") \
    _(CELL_NEW                  , FMT_A     , "R[A] = new_cell(R[A])") \
    _(CELL_GET                  , FMT_ABC   , "R[A] = cell_deref(R[B])") \
    _(CELL_SET                  , FMT_ABC   , "cell_store(R[A], R[B])") \
    _(CLASS_CREATE_FROM_DESCRIPTOR, FMT_ABx   , "R[A] = Class.from_descriptor(K[Bx])") \
    _(CLINIT_CALL               , FMT_A     , "call static init <clinit>") \
    _(GETPROP                   , FMT_ABC   , "R[A] = R[B].K[C]") \
    _(SETPROP                   , FMT_ABC   , "R[A].K[B] = R[C]") \
    _(GETSUPER                  , FMT_ABC   , "R[A] = R[B].super.K[C]") \
    _(INVOKE                    , FMT_ABC   , "R[A] = R[B]:method(...)") \
    _(INVOKE_TAIL               , FMT_ABC   , "tail: R[A+1]:method(...) reuse frame") \
    _(SUPERINVOKE               , FMT_ABC   , "super.method(...)") \
    _(INVOKE_DIRECT             , FMT_ABC   , "R[A] = R[B]:methods[C](...)") \
    _(INVOKE_BUILTIN            , FMT_ABC   , "R[A] = R[A+1]:builtin[B](nargs=C)") \
    _(ABSTRACT_ERROR            , FMT_NONE  , "runtime: called abstract method") \
    _(SET_STORAGE_CTX           , FMT_A     , "set storage mode context A=mode") \
    _(TO_SHARED                 , FMT_AB    , "R[A] = to_shared(R[B])") \
    _(MAP_SETKS                 , FMT_AB    , "R[A].fields[0..B-1] = R[A+1]..R[A+B]") \
    _(GETFIELD                  , FMT_ABC   , "R[A] = R[B]:Instance.fields[C]") \
    _(SETFIELD                  , FMT_ABC   , "R[A]:Instance.fields[B] = R[C]") \
    _(GETFIELD_IC               , FMT_ABC   , "R[A] = R[B].K[C] (IC)") \
    _(NEWJSON                   , FMT_ABx   , "R[A] = new Json(K[Bx]:Shape)") \
    _(JSON_GET                  , FMT_ABC   , "R[A] = R[B].fields[C]") \
    _(JSON_SET                  , FMT_ABC   , "R[A].fields[B] = R[C]") \
    _(JSON_GETK                 , FMT_ABC   , "R[A] = R[B].get(symbol=C)") \
    _(JSON_SETK                 , FMT_ABC   , "R[A].set(symbol=B, R[C])") \
    _(JSON_INIT                 , FMT_ABC   , "R[A].fields[B] = R[C]") \
    _(JSON_INIT_I               , FMT_ABC   , "R[A].fields[B] = C") \
    _(JSON_INIT_N               , FMT_ABC   , "R[A].fields[B] = null") \
    _(GETBUILTIN                , FMT_GLOBAL, "R[A] = builtins[Bx]") \
    _(PRINT                     , FMT_A     , "print(R[A])") \
    _(TYPEOF                    , FMT_AB    , "R[A] = typeof(R[B]) -> int") \
    _(TYPENAME                  , FMT_AB    , "R[A] = typename(R[B]) -> string") \
    _(DUMP                      , FMT_AB    , "dump(R[A], indent=B)") \
    _(TOINT                     , FMT_AB    , "R[A] = int(R[B])") \
    _(TOFLOAT                   , FMT_AB    , "R[A] = float(R[B])") \
    _(TOSTRING                  , FMT_AB    , "R[A] = string(R[B])") \
    _(TOBOOL                    , FMT_AB    , "R[A] = bool(R[B])") \
    _(COPY                      , FMT_AB    , "R[A] = copy(R[B])") \
    _(CHR                       , FMT_AB    , "R[A] = chr(R[B])") \
    _(ENUM_ACCESS               , FMT_ABC   , "R[A] = R[B].variant[R[C]]") \
    _(ENUM_CONVERT              , FMT_ABC   , "R[A] = enum_convert(R[B], R[C])") \
    _(ENUM_NAME                 , FMT_AB    , "R[A] = enum_name(R[B])") \
    _(TRY                       , FMT_SPECIAL, "try block start") \
    _(CATCH                     , FMT_A     , "catch block") \
    _(FINALLY                   , FMT_NONE  , "finally block") \
    _(END_TRY                   , FMT_NONE  , "try block end") \
    _(THROW                     , FMT_A     , "throw R[A]") \
    _(SPILL                     , FMT_AB    , "S[A] = R[B] (spill register to slot)") \
    _(RELOAD                    , FMT_AB    , "R[A] = S[B] (reload from slot)") \
    _(BOX_I64                   , FMT_AB    , "R[A] = box(R[B] as i64)") \
    _(BOX_F64                   , FMT_AB    , "R[A] = box(R[B] as f64)") \
    _(UNBOX_I64                 , FMT_AB    , "R[A] = unbox(R[B]) as i64") \
    _(UNBOX_F64                 , FMT_AB    , "R[A] = unbox(R[B]) as f64") \
    _(ARRAY_GET_NOCHECK         , FMT_ABC   , "R[A] = R[B]:Array[R[C]] (no check)") \
    _(MAP_INCREMENT             , FMT_AB    , "R[A]:Map[R[B]]++") \
    _(SUBSTRING                 , FMT_ABC   , "R[A] = R[B].substring(R[C], R[C+1])") \
    _(STR_REPEAT                , FMT_ABC   , "R[A] = R[B] * R[C] (string repeat)") \
    _(IMPORT                    , FMT_ABx   , "R[A] = import(K[Bx])") \
    _(EXPORT                    , FMT_ABC   , "export(K[A], R[B], C=const?)") \
    _(EXPORT_ALL                , FMT_A     , "export * from R[A]") \
    _(ASSERT                    , FMT_ABC   , "if !R[A] throw AssertError(K[B]); C=1: negate") \
    _(ASSERT_EQ                 , FMT_ABC   , "if R[A] != R[B] throw AssertError(K[C])") \
    _(ASSERT_NE                 , FMT_ABC   , "if R[A] == R[B] throw AssertError(K[C])") \
    _(REGEX_COMPILE             , FMT_ABC   , "R[A] = regex.compile(K[B], K[C])") \
    _(GO                        , FMT_ABC   , "R[A] = go R[B](R[B+1]..R[B+C])") \
    _(GO_INVOKE                 , FMT_ABC   , "R[A] = go R[B].method(args)") \
    _(SPAWN_CONT                , FMT_NONE  , "spawn continuation") \
    _(AWAIT                     , FMT_ABC   , "R[A] = await R[B], C=discard") \
    _(AWAIT_TIMEOUT             , FMT_ABC   , "R[A] = await(timeout: R[C]) R[B]") \
    _(AWAIT_ALL                 , FMT_AB    , "R[A] = await R[B]:Array") \
    _(AWAIT_ANY                 , FMT_ABC   , "R[A] = await.any R[B]:Array, C=mode") \
    _(YIELD                     , FMT_NONE  , "yield") \
    _(CANCELLED                 , FMT_A     , "R[A] = cancelled()") \
    _(LOCK_THREAD               , FMT_NONE  , "Coro.lockThread()") \
    _(UNLOCK_THREAD             , FMT_NONE  , "Coro.unlockThread()") \
    _(SET_LOCAL                 , FMT_AB    , "Coro.setLocal(R[A], R[B])") \
    _(GET_LOCAL                 , FMT_AB    , "R[A] = Coro.getLocal(R[B])") \
    _(SET_PRIORITY              , FMT_AB    , "Coro.setPriority(R[A], R[B])") \
    _(CORO_CTRL                 , FMT_ABC   , "coro monitoring, C=sub_op") \
    _(CHAN_NEW                  , FMT_AB    , "R[A] = Channel(B)") \
    _(CHAN_NEW_NAMED            , FMT_ABC   , "R[A] = Channel(R[B], R[C]) - named") \
    _(CHAN_SEND                 , FMT_ABC   , "R[B].send(R[C])") \
    _(CHAN_RECV                 , FMT_AB    , "R[A], R[A+1] = R[B].recv()") \
    _(CHAN_TRY_SEND             , FMT_ABC   , "R[A] = R[B].trySend(R[C])") \
    _(CHAN_TRY_RECV             , FMT_AB    , "R[A], R[A+1] = R[B].tryRecv()") \
    _(CHAN_SEND_TIMEOUT         , FMT_ABC   , "R[A] = R[B].send(R[C], timeout: R[C+1])") \
    _(CHAN_RECV_TIMEOUT         , FMT_ABC   , "R[A] = R[B].recv(timeout: R[C])") \
    _(CHAN_CLOSE                , FMT_A     , "R[A].close()") \
    _(CHAN_IS_CLOSED            , FMT_AB    , "R[A] = R[B].isClosed()") \
    _(SELECT_START              , FMT_NONE  , "select start") \
    _(SELECT_CASE               , FMT_ABC   , "case A=type, B=channel, C=value") \
    _(SELECT_END                , FMT_NONE  , "select end") \
    _(DEFER                     , FMT_A     , "defer R[A]") \
    _(BYTES_NEW                 , FMT_AB    , "R[A] = Bytes(B args)") \
    _(SCOPE_ENTER               , FMT_A     , "enter scope, A=mode(0=wait,1=linked,2=supervisor)") \
    _(SCOPE_EXIT                , FMT_AB    , "exit scope, A=mode, B=result_reg") \
    _(SLEEP                     , FMT_A     , "time.sleep(R[A]) ms") \
    _(TIME_AFTER                , FMT_AB    , "R[A] = time.after(R[B]) ms") \
    _(SELECT_BLOCK              , FMT_AB    , "select block wait") \
    _(GETSHARED                 , FMT_GLOBAL, "R[A] = shared[Bx]") \
    _(SETSHARED                 , FMT_GLOBAL, "shared[Bx] = R[A]") \
    _(TARRAY_GET                , FMT_ABC   , "R[A].i = R[B]:TypedArray[R[C]]") \
    _(TARRAY_GETC               , FMT_ABC   , "R[A].i = R[B]:TypedArray[C]") \
    _(TARRAY_SET                , FMT_ABC   , "R[A]:TypedArray[R[B]] = R[C].i") \
    _(TARRAY_PUSH               , FMT_AB    , "R[A]:TypedArray.push(R[B].i)") \
    _(TFIELD_GET                , FMT_ABC   , "R[A].i = R[B]:compact_fields[C]") \
    _(TFIELD_SET                , FMT_ABC   , "R[A]:compact_fields[B] = R[C].i") \
    _(INST_TYPE_ARGS            , FMT_ABx   , "R[A]:Instance.gc.extra = packed type args from Bx") \
    _(LOOP_BACK                 , FMT_ABC   , "tail recursion: R[0..B-1]=R[A+1..A+B]; PC=entry") \
    _(NEW_STRUCT                , FMT_ABx   , "R[A] = alloc struct in struct_area (Bx=layout_id)") \
    _(STRUCT_GET                , FMT_ABC   , "R[A] = struct(R[B]).field[C]") \
    _(STRUCT_SET                , FMT_ABC   , "struct(R[A]).field[B] = R[C]") \
    _(STRUCT_COPY               , FMT_AB    , "R[A] = memcpy struct R[B]") \
    _(NOP                       , FMT_SPECIAL, "no-op / spawn metadata")

#endif // XOPCODE_DEF_H

