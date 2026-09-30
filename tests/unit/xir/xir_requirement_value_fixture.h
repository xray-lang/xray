/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_requirement_value_fixture.h - Escaping receiver-owning method values
 *
 * KEY CONCEPT:
 *   Method values escape their producer frame and retain a single receiver value.
 */
#ifndef XIR_REQUIREMENT_VALUE_FIXTURE_H
#define XIR_REQUIREMENT_VALUE_FIXTURE_H
static const char requirement_value_owned_source[] =
    "interface Read<A>{get()->A;map<U:Sendable>(value:U)->U}\n"
    "struct Holder<X,Y> implements Read<Y>{label:X;value:Y;get()->Y{return this.value};map<V:Sendable>(value:V)->V{return value}}\n"
    "var visits=0\n"
    "fn touch<A,T:Read<A>>(receiver:T)->T{visits=visits+1;return receiver}\n"
    "fn bind<A,T:Read<A>,K>(receiver:T,unused:K)->fn()->A{return touch<A,T>(receiver).get}\n"
    "fn dropped<T:Read<string>>(receiver:T)->i64{const saved=receiver.map<i64>;return 0}\n"
    "export fn answer()->string{const saved=bind<string,Holder<i64,string>,bool>(Holder<i64,string>{label:0,value:\"map\"+\"ped\"},true);const first=saved();const second=saved();const ignored=dropped<Holder<i64,string>>(Holder<i64,string>{label:1,value:\"un\"+\"used\"});return second}\n"
    "export fn count()->i64{return visits}\n";
#endif // XIR_REQUIREMENT_VALUE_FIXTURE_H
