/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
/* Exact literals retained by the three original document semantic tests. */
static const char *const texts[]={
"fn adjust(view: i64, slot: ref i64, payload: move Array<i64>) -> i64 {\n    slot = slot + view + len(payload)\n    return slot\n}\n\nfn main() {\n    var value = 1\n    var slot = 2\n    var payload = [3]\n    adjust(value, ref slot, move payload)\n}\n",
"var page = r\"\"\"\n<div title=\"quoted\">{ not_xray }</div>\n\"\"\"\nvar bytes = br\"\"\"\"\n${HOME} // literal { bytes }\n\"\"\"\"\nvar after = 7\n",
"enum Result {\n    Ok { value: i64, code: i64 },\n    Err { value: string }\n}\nvar made = Result.Ok { code: 7, value: 42 }\nvar selected = match (made) {\n    Result.Ok { value, code: _ } -> value\n    Result.Err { value: message } -> 0\n}\n"
};
