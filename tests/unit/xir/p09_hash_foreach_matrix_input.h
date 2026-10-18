/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_hash_foreach_matrix_input.h - Independent bounded graphs retain every traversal case
 */
#ifndef P09_HASH_FOREACH_MATRIX_INPUT_H
#define P09_HASH_FOREACH_MATRIX_INPUT_H
#include "xir/xxir.h"
static const char *const foreach_matrix_sources[]={
"fn walkMap<K:Hashable,V>(m:Map<K,V>,visit:fn(K,V)){m.forEach(visit)}\nfn walkSet<T:Hashable>(s:Set<T>,visit:fn(T)){s.forEach(visit)}\nfn order()->i64{\nvar m=#{3:30,1:10,2:20}\nm.set(1,11)\nm.delete(3)\nm.set(3,33)\nconst old=m\nvar trace=0\nvar sum=0\nwalkMap(m,fn(k,v){trace=trace*10+k\nsum+=v\nm.clear()\nm.set(9,99)})\nif(trace!=123){return -1}\nif(sum!=64){return -1}\nif(len(old)!=3){return -2}\nif(old[1]!=11){return -2}\nif(m[9]!=99){return -2}\nvar s=#[3,1,2]\ns.delete(1)\ns.add(1)\nconst saved=s\ntrace=0\nwalkSet(s,fn(v){Coro.yield()\ntrace=trace*10+v\ns.clear()\ns.add(9)})\nif(trace!=321){return -3}\nif(len(saved)!=3){return -3}\nif(!s.contains(9)){return -3}\nreturn 0}\nexport fn entry()->i64{const result=order()\nif(result!=0){return result}\nreturn 123}\n",
"fn walkMap<K:Hashable,V>(m:Map<K,V>,visit:fn(K,V)){m.forEach(visit)}\nfn walkSet<T:Hashable>(s:Set<T>,visit:fn(T)){s.forEach(visit)}\nfn evaluation()->i64{\nvar trace=0\nvar factories=0\nvar m=#{1:1,2:2}\nfn receiver()->Map<i64,i64>{trace=trace*10+1\nreturn m}\nfn factory()->fn(i64,i64){trace=trace*10+2\nfactories+=1\nm.clear()\nreturn fn(k,v){trace=trace*10+k}}\nreceiver().forEach(factory())\nif(trace!=1212){return -4}\nif(factories!=1){return -4}\nMap<i64,i64>().forEach(factory())\nif(trace!=12122){return -5}\nif(factories!=2){return -5}\nvar s=#[3,4]\nvar seen=0\nfn setfactory()->fn(i64){factories+=1\ns=#[9]\nreturn fn(v){seen=seen*10+v}}\ns.forEach(setfactory())\nif(seen!=34){return -6}\nif(factories!=3){return -6}\nif(!s.contains(9)){return -6}\nreturn 0}\nexport fn entry()->i64{const result=evaluation()\nif(result!=0){return result}\nreturn 123}\n",
"fn walkMap<K:Hashable,V>(m:Map<K,V>,visit:fn(K,V)){m.forEach(visit)}\nfn walkSet<T:Hashable>(s:Set<T>,visit:fn(T)){s.forEach(visit)}\nfn arbitrary()->i64{\nconst m=#{7:#[4,5]}\nvar seen=0\nwalkMap(m,fn(k,values){seen=k+len(values)\nvar copy=values\ncopy.add(9)\nif(values.contains(9)){seen=-1}})\nif(seen!=9){return -7}\nif(len(m[7])!=2){return -7}\nvar count=0\nwalkSet(Set<i64>(),fn(v){count+=1})\nif(count!=0){return -7}\nreturn 0}\nexport fn entry()->i64{const result=arbitrary()\nif(result!=0){return result}\nreturn 123}\n",
"fn walkMap<K:Hashable,V>(m:Map<K,V>,visit:fn(K,V)){m.forEach(visit)}\nfn walkSet<T:Hashable>(s:Set<T>,visit:fn(T)){s.forEach(visit)}\nfn stopped()->i64{\nconst m=#{1:1,2:2,3:3}\nvar visits=0\ntry{m.forEach(fn(k,v){Coro.yield()\nvisits=visits*10+k\nassert(k!=2)})}catch panic(p){if(visits!=12){return -8}\nreturn 0}\nreturn -8}\nexport fn entry()->i64{const result=stopped()\nif(result!=0){return result}\nreturn 123}\n"};
static const uint32_t foreach_matrix_yields[]={3,0,0,2};
static inline uint32_t foreach_matrix_entry(const XrXirModule *module) {
    uint32_t entry=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==5 && !memcmp(module->functions[f].name,"entry",5)) {
            CHECK(entry==UINT32_MAX);entry=f;
        }
    CHECK(entry<module->function_count);return entry;
}
#endif // P09_HASH_FOREACH_MATRIX_INPUT_H
