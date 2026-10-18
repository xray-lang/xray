/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_hash_sendable.c - Direct and nested collection transmission proofs agree
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "p09_hash_source_check.h"

int main(void) {
    const char *const sources[]={
        "fn send<T:Sendable>(value:T)->T{return value}\nexport fn entry()->i64{const m=#{1:2}\nconst result=send(m)\nreturn 0}",
        "fn send<T:Sendable>(value:T)->T{return value}\nexport fn entry()->i64{const s=#[1,2]\nconst result=send(s)\nreturn 0}",
        "fn send<T:Sendable>(value:T)->T{return value}\nexport fn entry()->i64{const m=#{1:2}\nconst result=send((m,7))\nreturn 0}",
        "fn send<T:Sendable>(value:T)->T{return value}\nexport fn entry()->i64{const s=#[1,2]\nconst result=send((s,7))\nreturn 0}",
        "fn send<T:Sendable>(value:T)->T{return value}\nexport fn entry()->i64{const m=[#{1:2}]\nconst result=send(m)\nreturn 0}",
        "fn send<T:Sendable>(value:T)->T{return value}\nexport fn entry()->i64{const m:Map<i64,i64>?=null\nconst result=send(m)\nreturn 0}",
        "fn send<T:Sendable>(value:T)->T{return value}\nexport fn entry()->i64{const m=#{\"key\":[#[1,2]]}\nconst result=send(m)\nreturn 0}",
        "fn send<T:Sendable>(value:T)->T{return value}\nfn relay<K:Hashable & Sendable,V:Sendable>(m:Map<K,V>)->Map<K,V>{return send(m)}\nexport fn entry()->i64{const m=relay(#{1:2})\nreturn 0}",
        "fn send<T:Sendable>(value:T)->T{return value}\nfn relay<T:Hashable & Sendable>(s:Set<T>)->Set<T>{return send(s)}\nexport fn entry()->i64{const s=relay(#[1])\nreturn 0}",
        "fn send<T:Sendable>(value:T)->T{return value}\nfn relay(t:Task<Map<i64,i64>>)->Task<Map<i64,i64>>{return send(t)}\nexport fn entry()->i64{return 0}",
        "fn send<T:Sendable>(value:T)->T{return value}\nfn bad<K:Hashable,V:Sendable>(m:Map<K,V>){const result=send(m)}",
        "fn send<T:Sendable>(value:T)->T{return value}\nfn bad<K:Hashable & Sendable,V>(m:Map<K,V>){const result=send(m)}",
        "fn send<T:Sendable>(value:T)->T{return value}\nfn bad<T:Hashable>(s:Set<T>){const result=send(s)}",
        "fn send<T:Sendable>(value:T)->T{return value}\nexport fn entry()->i64{const f=fn()->i64{return 1}\nconst m=#{1:f}\nconst result=send(m)\nreturn 0}",
        "fn send<T:Sendable>(value:T)->T{return value}\nexport fn entry()->i64{const f=fn()->i64{return 1}\nconst m=[#{1:f}]\nconst result=send(m)\nreturn 0}",
        "fn send<T:Sendable>(value:T)->T{return value}\nexport fn entry()->i64{const f=fn()->i64{return 1}\nconst m=#{1:f}\nconst result=send((m,7))\nreturn 0}",
        "fn send<T:Sendable>(value:T)->T{return value}\nfn bad(t:Task<Map<i64,fn()->i64>>){}\nexport fn entry()->i64{return 0}"
    };
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);unsigned rejected=0;
    for(unsigned row=0;row<17;++row) {
        XrXirSourceResult result={0};XrXirStatus status=hash_source_check(&owner.context,sources[row],&result);
        printf("Sendable direct Map/Set and Tuple control row=%u status=%u\n",row,(unsigned)status);
        XrXirStatus expected=row<10?XR_XIR_OK:XR_XIR_BAD_TYPE;
        rejected+=status!=expected;
        if(status!=XR_XIR_OK)continue;
        XrXirArtifact *lowered=hash_source_replay(&owner.context,&result);
        xr_xir_compile_artifact_free(lowered);
    }
    source_fixture_owner_free(&owner);
    CHECK(!rejected);puts("Ten positive and seven negative direct/nested/generic Map/Set Sendable proofs PASS");return 0;
}
