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
anchor-sha256: src/shared/xobject_shape.h 21eae050b1e0f3f7dfc00d5d7bee4e79f943d51bdc4b89096c0e328174a65d98
anchor-sha256: src/runtime/value/xtype.h 3fc485e57ddc2bd95778f995f5df0e6f334b0f86197b40dfe468564e1b0b6426
anchor-sha256: src/runtime/value/xtype.c 84761e16d37abaeffb9b5969812f1e1c34f7172874a28d52face5529ee149026
anchor-sha256: src/frontend/parser/xtype_ref.h 4c3823b5f8ca3dc0164605a6824070710f6efb7e0209dc431726ae389ea9dd24
anchor-sha256: src/frontend/parser/xtype_ref.c 3a2b839172e60f9801d5b54e4be986186884c3fabe5fcaacd1d006907a2678a3
anchor-sha256: src/frontend/analyzer/xanalyzer_capability.h 94c7b2c2f02b92c6bd324e7719dba1afadb52faf0b0f03bf6dd4ac4164d96123
anchor-sha256: src/frontend/analyzer/xanalyzer_capability.c e32f303a6be4201c1aed25f78e16c925f5ec2449f5a8c0ba35ced653baaf40d7
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_expr.c 83ce300e65c2bd225ece5d8c5ea57d4ccc2bb9a2f0e130ac2aac2ca85c6558ad
anchor-sha256: src/analysis/xglobal_summary.h 76668edea6dd3924bfff9b06dc0df7758d06419de65fa87b1b576e3c71c43cc9
anchor-sha256: src/ir/xi.h 298de848b014831ded0c48e0f95ec176d45d356d2c0da85fb680fecf2372cea8
anchor-sha256: xisa/xi/ops.def 9091256d150269d3b16cd9b7f60371292a5d8d357d48f513e0608cb32d663ce5
anchor-sha256: src/aot/xrt_coll.h b74b9f2b7eee1666774d5ea2e0a946048fd373f77da42e40adb812339842351f
anchor-sha256: src/aot/xi_cgen_dispatch_helpers.inc.c b4f50ee6ffa9500c9848daa3f61475e708ccd35addf4728c18e362dde0eb4718
anchor-sha256: src/aot/xi_cgen_program_entry.inc.c 0bba73642490bb76d403a29495323b04852027bfabb83ecead32a833d23caa36
anchor-sha256: src/runtime/class/xclass.h 5d92a59177ecf5550463fb81740f68f344a1f6abe669a5220151f681b0ffdf75
anchor-sha256: src/runtime/class/xinstance.c 6d0b1501e7d8790aa9f6476d96f45162199026d5fc7913071d8cc742c6feddea
anchor-sha256: src/module/xbytecode_io.h 8ba68bcfe8a8dcddf16ece7678be7d8ead446f1163be8889d5e2ece8bd08e55f
anchor-sha256: src/module/xbytecode_io.c 496a0ebaa2bb04ac3bb08aad0d0c61c31369e9005e3d70d01a24f5670ffbd59f
anchor-sha256: src/aot/xaot_verify.c 7fe97d173ac206666af4d63052ee0abb41b26eac8451da308ab5f8110fad6e16
