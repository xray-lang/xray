# Object and Json domain contract

Status: frozen after object-surface convergence and typed nominal Json construction.

1. Structural objects and Json values are distinct semantic domains. A
   structural object is not implicitly converted to Json, and an encodable
   type is not thereby a Json value.
2. Object row mode is independent of runtime object domain. An exact row has
   exactly its declared field set. An open row accepts a concrete structural
   object with additional fields, but exposes only its declared fields and
   never grants dynamic extension, computed-key access, enumeration, or
   reflection over hidden fields.
3. Mutable fields are invariant across exact and open row assignment. A row
   relation must not permit an alias to change the type of a visible or hidden
   field.
4. A structural object literal always creates one exact concrete shape, even
   when contextually assigned to an open row. The compiler may preserve the
   concrete shape as refinement evidence; open row itself is not an allocation
   shape.
5. Dot access and a static string-key bracket access to the same visible field
   resolve to the same field identity and ordinal. Computed keys are rejected
   for structural objects and remain available only to dynamic Json objects.
6. Json is the closed recursive value domain `null | bool | int | float |
   string | JsonArray | JsonObject`. Composite Json values carry constant-time
   domain provenance; membership is not inferred from encode capability.
7. `JsonValue(T)`, `JsonEncodable(T)`, and `JsonDecodable(T)` answer different
   questions and are never interchangeable. Unsupported codec capability is a
   compile-time result with a stable reason; it does not trigger a runtime
   fallback.
8. Typed values cross the Json boundary explicitly. `Json.encode<T>`
   materializes Json, `Json.stringify<T>` may stream an encodable value, and
   `Json.parse<T>` validates and directly constructs an exact decodable target.
   `Json.parse<T>` must not materialize an unowned intermediate DOM.
9. VM and AOT may use different physical object layouts, but consume the same
   stable shape identity, canonical field ordinals, access decisions, and
   codec decisions. Missing or stale required evidence is an error; neither
   backend may silently fall back to name lookup or DOM decoding.
10. Exact structural-object hot paths use unchecked ordinal access. They do not
    allocate extension maps, register field symbols, perform name lookup, or
    add shape guards. Adding a surface spelling must not change this code shape.
11. The public language surface contains no nominal `Object` source type and,
    after convergence, no `Record` type name. Internal structural-object and
    Json-value domain identities remain explicit even when their runtime
    object category is shared.
12. AOT object shapes are process-lifetime, file-scope `static const`
    descriptors. An object instance borrows exactly one descriptor and never
    owns or releases a field-name array. Static-shape construction performs
    only the object-body allocation.
13. The AOT instance header contains only the unified object header, the shape
    descriptor pointer, and the lazy Json extension-map pointer before its
    flexible field storage. Replacing per-instance field metadata must not grow
    this header; structural-object paths never allocate an extension map.
14. Stable shape identity hashes domain, canonical field count, UTF-8 field
    names, stable field-type keys, and optional/readonly flags through the
    repository hash primitive. Equal keys require field-table equality;
    translation units and backends never use descriptor or class pointer
    equality as structural type evidence.
15. VM hidden classes expose the same domain, stable shape key, and field
    ordinal manifest as AOT descriptors. Bytecode persists stable field-type
    keys and shape flags, and rejects incompatible serialized manifests rather
    than reconstructing a weaker identity.
16. A payload-free enum and a class or value struct explicitly deriving Json
    may participate in `JsonDecodable`, but do not thereby become Json values.
    Their recursive field schema survives bytecode and generated-C boundaries.
    Decoding constructs the declared nominal target without invoking its
    constructor; native value structs decode into their established aggregate
    layout, and stringify streams that layout without first materializing a
    Json object.

## Digest anchors

anchor-sha256: src/shared/xobject_row.h 5057c952ae0f809e3aaec6e4c48f64ece296af550a7cd2f23ac36185b2c0f170
anchor-sha256: src/shared/xobject_shape.h d7f814c82990bc4eb1c8e9b31f15cda4582a1a582f9016ac2a890b342b82be72
anchor-sha256: src/runtime/value/xtype.h 684f0351f5de9e545591c1feb00cbd6c7f16c63adf3a8d8afa78737a62d80ea6
anchor-sha256: src/runtime/value/xtype.c efe8411062422bcdbb32f6aad21309151213323608a570cb010cc53d8223bd87
anchor-sha256: src/frontend/parser/xtype_ref.h 4c3823b5f8ca3dc0164605a6824070710f6efb7e0209dc431726ae389ea9dd24
anchor-sha256: src/frontend/parser/xtype_ref.c 3a2b839172e60f9801d5b54e4be986186884c3fabe5fcaacd1d006907a2678a3
anchor-sha256: src/frontend/analyzer/xanalyzer_capability.h 94c7b2c2f02b92c6bd324e7719dba1afadb52faf0b0f03bf6dd4ac4164d96123
anchor-sha256: src/frontend/analyzer/xanalyzer_capability.c db1d019c684b320cd2340ce071f84ac0b0efcb60c54c65f5240e3ff84c724106
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_expr.c 1bc97ec734b0aa3e41dc376793ee90260838cfd814c32b3c29dd039e4d461f40
anchor-sha256: src/analysis/xglobal_summary.h d65498c445e52292e38fbe6cdd6b5e49bc941347ee31968e95653dcd355c04a3
anchor-sha256: src/ir/xi.h 7155a9e179743a61aa942fe98a5f9d3eb13e8a42b1517eb713fd104de736c959
anchor-sha256: xisa/xi/ops.def 9091256d150269d3b16cd9b7f60371292a5d8d357d48f513e0608cb32d663ce5
anchor-sha256: src/aot/xrt_coll.h 34a75b817ae34925f374bccb32f343f0af49a4801c7c97aa244ab12db5e10446
anchor-sha256: src/aot/xi_cgen_dispatch_helpers.inc.c 09432448fbb00b2a7b025d2c9af60d25c9b74e44e04c7ccb8e247c486beb0ef6
anchor-sha256: src/aot/xi_cgen_program_entry.inc.c d4ddd1ecd7b7edfc296137267dd5472acc38ea65c9a4e403873eae24e41d8460
anchor-sha256: src/runtime/class/xclass.h 1623dd05f8e6fdf462407206090a1c98fd822c51010375272a21a1cd26959183
anchor-sha256: src/runtime/class/xinstance.c 70442d0f568f1e5e119ba084df12d6e8d5ce151a4b1e61c8544f4ff3347bac0d
anchor-sha256: src/module/xbytecode_io.h be21ee7702928d3f28804e3543b18b1c80a1db07c2b95d887e5073e0cf709b36
anchor-sha256: src/module/xbytecode_io.c bacfe362ee607b2918615e3df7f2056b363fc1ad033f8502bddb31ec3660b307
anchor-sha256: src/aot/xaot_verify.c ce65476db888067cafed913b3ea516c1ca889de0f2502075ed348f4c7b682532
