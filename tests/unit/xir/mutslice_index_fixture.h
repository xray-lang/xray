/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Local mutable ranges retain their original Array owner and exact element. */
#ifndef MUTSLICE_INDEX_FIXTURE_H
#define MUTSLICE_INDEX_FIXTURE_H
static const char mutslice_index_source[]=
    "var ticks=0\n"
    "fn index()->i64{ticks=ticks*10+1;return 0}\n"
    "fn replacement()->i64{ticks=ticks*10+2;return 99}\n"
    "fn sliceExample(){\n"
    " var values=[2,3,4,5]\n"
    " var view:MutSlice<i64>=values[1:4]\n"
    " for(i in 0..len(view)){view[i]=view[i]*10}\n"
    " print(\"scaled view: ${view[0]}, ${view[1]}, ${view[2]}\")\n"
    " print(\"owner after view write: ${values[1]}, ${values[2]}, ${values[3]}\")\n"
    "}\n"
    "export fn test()->i64{\n"
    " sliceExample()\n"
    " var values=[2,3,4,5];const old=values\n"
    " var view:MutSlice<i64>=values[1:4]\n"
    " for(i in 0..len(view)){view[i]=view[i]*10}\n"
    " assert(view[0]==30 && view[1]==40 && view[2]==50)\n"
    " assert(values[0]==2 && values[1]==30 && values[2]==40 && values[3]==50)\n"
    " assert(old[0]==2 && old[1]==3 && old[2]==4 && old[3]==5)\n"
    " values[1]=31;assert(values[1]==31)\n"
    " var bytes:Array<u8>=[1,2,3];const oldBytes=bytes\n"
    " var writable:MutSlice<u8>=bytes[:]\n"
    " writable[0]=7;writable[1]=9\n"
    " assert(writable[0]==7 && writable[1]==9 && writable[2]==3)\n"
    " assert(bytes[0]==7 && bytes[1]==9 && oldBytes[0]==1 && oldBytes[1]==2)\n"
    " var owner=[1,2];var parent:MutSlice<i64>=owner[:]\n"
    " var child:Slice<i64>=parent[0:1];assert(child[0]==1)\n"
    " ticks=0;parent[index()]=replacement();assert(ticks==12 && parent[0]==99)\n"
    " assert(owner[0]==99)\n"
    " return 42\n"
    "}\n"
    "export fn bounds()->i64{var a=[1];var v:MutSlice<i64>=a[:];v[1]=2;return 0}\n"
    "export fn slow()->i64{var a=[1];var v:MutSlice<i64>=a[:];for(i in 0..1000000){v[0]=i};return v[0]}\n"
    "export fn escape()->Array<i64>{var a=[1,2];var v:MutSlice<i64>=a[:];v[0]=42;return a}\n";
static const char mutslice_index_expected[]=
    "scaled view: 30, 40, 50\nowner after view write: 30, 40, 50\n";
#define MUTSLICE_INDEX_ENTRIES 4
static const char *const mutslice_index_names[MUTSLICE_INDEX_ENTRIES]={"test","bounds","slow","escape"};
#endif
