# Object and Json domain contract

Status: frozen before the object-surface convergence.

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

## Digest anchors

anchor-sha256: src/shared/xobject_row.h 5057c952ae0f809e3aaec6e4c48f64ece296af550a7cd2f23ac36185b2c0f170
anchor-sha256: src/runtime/value/xtype.h 1707a847fdc694f97880c5940306d5bd981027c36ac6053e7036b5a23fe5582a
anchor-sha256: src/runtime/value/xtype.c 319d714a614b1240530b41574618ebf56bfcc28948b5ae05cc8944fbb82523e0
anchor-sha256: src/frontend/parser/xtype_ref.h 13f55898123e2f92fc61233dbfa4f844981307a86df90a4495cd7796e72370ca
anchor-sha256: src/frontend/parser/xtype_ref.c 2c4531e3451f4c277077de6d778897e33fb5bbb3cee7e531efc94b60af68aa03
anchor-sha256: src/frontend/analyzer/xanalyzer_capability.h 94c7b2c2f02b92c6bd324e7719dba1afadb52faf0b0f03bf6dd4ac4164d96123
anchor-sha256: src/frontend/analyzer/xanalyzer_capability.c 8ed8173e90f05be19b9d103b346639e592dc2e356cf607800a5f34338ab43c35
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_expr.c d5c3bfd1956f53272f3fcd4ab3adb8dae4b9883e3bfd8a2a689c23471edbe329
anchor-sha256: src/analysis/xglobal_summary.h 2230be70b30a147c5b885e2e5ab8a1db80354b8cc0f0cf7d78f97d0a61a67d3d
anchor-sha256: src/ir/xi.h 54e3d1e1fc1331504e00fff388dc8cbd04d7b1b5610a0add79449824d5f6cc91
anchor-sha256: xisa/xi/ops.def edac98373bbcbca5c894aabf853c96b12abc977c5ef12d172c853f389d06c132
anchor-sha256: src/aot/xrt_coll.h b4f55426f3680984a94b4e5e8765593f7869a4104bf334edbb5f0fb27fb6eda5
anchor-sha256: src/aot/xaot_verify.c 394cc8c6c53c982413af6d8524e49cdf573da31b0d75fd23c4b13dbdadc2a423
