/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_history_cell_fixture.h - Shared real Source inputs for both backends
 */
#ifndef SOURCE_HISTORY_CELL_FIXTURE_H
#define SOURCE_HISTORY_CELL_FIXTURE_H
static inline const char *source_history_cell_input(unsigned generation,unsigned kind) {
    bool mutating=kind==1 || kind==2 || kind==4 || kind==5;
    if (generation==1 && kind==2) return
        "var value:Array<i64> = [41]\nfn old()->i64{value[0]=value[0]+1;return value[0]}\nfn identity<T>(value:T)->T{return value}\nstruct Item{value:i64}\nprint(17)\nexport fn seed()->i64{return 0}";
    if (generation==1 && kind==3) return
        "const value:Array<i64> = [41]\nfn old()->i64{return value[0]+1}\nfn identity<T>(value:T)->T{return value}\nstruct Item{value:i64}\nprint(17)\nexport fn seed()->i64{return 0}";
    if (generation==1 && kind==4) return
        "var value:Item = Item(41)\nfn old()->i64{value.value+=1;return value.value}\nfn identity<T>(value:T)->T{return value}\nclass Item{value:i64 init(value){this.value=value}}\nprint(17)\nexport fn seed()->i64{return 0}";
    if (generation==1 && kind==5) return
        "var value:Item = Item(41)\nfn old()->i64{value.value.set(0,value.value[0]+1);return value.value[0]}\nfn identity<T>(value:T)->T{return value}\nclass Item{value:Array<i64> init(first:i64){this.value=[first]}}\nprint(17)\nexport fn seed()->i64{return 0}";
    switch(generation) {
    case 1:return mutating ?
        "var value:i64=41\nfn old()->i64{value+=1;return value}\nfn identity<T>(value:T)->T{return value}\nstruct Item{value:i64}\nprint(17)\nexport fn seed()->i64{return 0}" :
        "var value:i64=41\nfn old()->i64{return value+1}\nfn identity<T>(value:T)->T{return value}\nstruct Item{value:i64}\nprint(17)\nexport fn seed()->i64{return 0}";
    case 2:return "const value=\"shadow\"\nexport fn answer()->i64{return old()}";
    case 3:return "const value=true\nexport fn answer()->i64{return old()}";
    case 4:return mutating ? "const value=99\nexport fn fail()->i64{const seen=old();assert(false);return seen}" :
        "const value=99\nexport fn fail()->i64{assert(false);return old()}";
    default:return NULL;
    }
}
#endif // SOURCE_HISTORY_CELL_FIXTURE_H
