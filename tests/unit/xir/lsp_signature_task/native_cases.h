/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static bool signature_native_fixture(unsigned scenario,const char **text,unsigned *row,unsigned *column) {
    if(scenario==55||scenario==56)scenario=25;
    switch(scenario) {
    case 25:*text="fn main()->i64 {\n const values=[1,2]\n return values.get(\n  0\n )\n}\n";*row=3;*column=2;break;
    case 26:*text="fn main(){\n var values=[1,2]\n values.set(\n  0,\n  3\n )\n}\n";*row=4;*column=2;break;
    case 27:*text="fn main()->Array<i64> {\n return Array<i64>.withCapacity(\n  2\n )\n}\n";*row=2;*column=2;break;
    case 28:*text="fn main(){\n var values=[1]\n const result=values.reserve(\n  4\n )\n}\n";*row=3;*column=2;break;
    case 29:*text="fn main(){\n var values=[1]\n values.push(\n  2\n )\n}\n";*row=3;*column=2;break;
    case 30:*text="fn main(){\n const values=[1]\n const result=values.join(\n  \",\"\n )\n}\n";*row=3;*column=2;break;
    case 31:*text="fn main(){\n const values=[1]\n const result=values.concat(\n  [2],\n  [3],\n  [4]\n )\n}\n";*row=5;*column=2;break;
    case 32:*text="fn main(){\n var values=[1]\n values.clear(\n )\n}\n";*row=3;*column=1;break;
    case 33:*text="fn main(){\n var values=[1]\n const result=values.reverse(\n )\n}\n";*row=3;*column=1;break;
    case 34:*text="fn main(){\n var values=[1]\n values.unshift(\n  2\n )\n}\n";*row=3;*column=2;break;
    case 35:*text="fn main(){\n var values=[1]\n const result=values.pop(\n )\n}\n";*row=3;*column=1;break;
    case 36:*text="fn main(){\n var values=[1]\n const result=values.shift(\n )\n}\n";*row=3;*column=1;break;
    case 37:*text="fn main(){\n var values=[1]\n const result=values.resize(\n  3,\n  2\n )\n}\n";*row=4;*column=2;break;
    case 38:*text="fn main(){\n var values=[1,2,3]\n const result=values.fill(\n  4,\n  0,\n  2\n )\n}\n";*row=5;*column=2;break;
    case 39:*text="fn main(){\n var values=[1]\n const result=values.fill(\n  2\n )\n}\n";*row=3;*column=2;break;
    case 40:*text="fn main(){\n const values=[1]\n const result=values.entries(\n )\n}\n";*row=3;*column=1;break;
    case 41:*text="fn main(){\n const values=[1]\n const result=values.map(\n  fn(item:i64)->string {return \"value\"}\n )\n}\n";*row=3;*column=2;break;
    case 42:*text="fn main(){\n const values=[1]\n const result=values.reduce(\n  fn(acc:i64,item:i64)->i64 {return acc+item},\n  0\n )\n}\n";*row=4;*column=2;break;
    case 43:*text="fn main(){\n const values=[1]\n const result=values.filter(\n  fn(item:i64)->bool {return item>0}\n )\n}\n";*row=3;*column=2;break;
    case 44:*text="fn main(){\n const values=[1]\n values.forEach(\n  fn(item:i64) {}\n )\n}\n";*row=3;*column=2;break;
    case 45:*text="fn main(){\n const values=[1]\n const result=values.find(\n  fn(item:i64)->bool {return item>0}\n )\n}\n";*row=3;*column=2;break;
    case 46:*text="fn main(){\n const values=[1]\n const result=values.findIndex(\n  fn(item:i64)->bool {return item>0}\n )\n}\n";*row=3;*column=2;break;
    case 47:*text="fn main(){\n const values=[1]\n const result=values.every(\n  fn(item:i64)->bool {return item>0}\n )\n}\n";*row=3;*column=2;break;
    case 48:*text="fn main(){\n const values=[1]\n const result=values.some(\n  fn(item:i64)->bool {return item>0}\n )\n}\n";*row=3;*column=2;break;
    case 49:*text="fn main(){\n const values=[1]\n const result=values.indexOf(\n  1\n )\n}\n";*row=3;*column=2;break;
    case 50:*text="fn main(){\n const values=[1]\n const result=values.contains(\n  1\n )\n}\n";*row=3;*column=2;break;
    case 51:*text="fn main()->i64 {\n const values=[1,2]\n return values.get(\n  values.get(0)\n )\n}\n";*row=3;*column=2;break;
    case 52:*text="fn wrapper<Item>(value:Item)->Item {\n const values=[value]\n return values.get(\n  0\n )\n}\nfn main()->i64 {return wrapper(1)}\n";*row=3;*column=2;break;
    case 53:*text="fn main(){\n var values:Array<i16>=[1]\n const value:i8=2\n values.push(\n  value\n )\n}\n";*row=4;*column=2;break;
    case 54:*text="fn main(){\n const values=[1]\n const result=values.join(\n )\n}\n";*row=3;*column=1;break;
    case 57:*text="fn main()->rune {\n const values=['x']\n return values.get(\n  0\n )\n}\n";*row=3;*column=2;break;
    case 58:*text="fn main(){\n var values:Array<fn(i64)->i64>=[fn(value:i64)->i64{return value}]\n const result=values.pop(\n )\n}\n";*row=3;*column=1;break;
    case 59:*text="export fn compute()->i64 {\n const values=[1,2]\n return values.get(\n  0\n )\n}\nfn main()->i64 {return compute()}\n";*row=3;*column=2;break;
    case 60:*text="struct Array<T> {const value:T get(index:i64)->T{return this.value}}\nexport fn compute()->i64 {\n const values=Array<i64>{value:7}\n return values.get(\n  0\n )\n}\nfn main()->i64{return compute()}\n";*row=4;*column=2;break;
    default:return false;
    }
    return true;
}
