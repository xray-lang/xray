# Structural object and JSON/Map boundary contract

Status: frozen after exact-object convergence and the removal of the public
`Json` value type.

1. A structural object is a statically shaped value. Every structural-object
   value has exactly the fields in its concrete type; the language has no open
   row, dynamic tail, extension zone, or runtime field dictionary for it.
2. The trailing type syntax `...` and `...: T` does not exist. Width is a
   relation used only when a generic structural constraint is satisfied:
   `T: { name: string }` requires at least that field while preserving the
   caller's concrete exact type. Ordinary assignment and argument matching
   between concrete structural-object types remain exact.
3. A field type `T?` is the only spelling for a nullable field. A field-name
   suffix `field?: T` is not part of the grammar and does not define a second
   optional-field model.
4. Dot access is resolved at compile time to a declared field identity and
   canonical ordinal. Structural objects reject computed-key access, dynamic
   insertion, deletion, key enumeration, and Map-style lookup. They may store
   any ordinary field type, including function values, subject to that type's
   normal ownership and capability rules.
5. Structural-object literals always infer one exact concrete shape. Object
   spread accepts only operands whose exact fields can be enumerated at compile
   time and produces another exact shape. It never imports keys from a Map or
   `JSON.Object`.
6. Exact structural-object hot paths use unchecked ordinal access. They do not
   allocate extension maps, register field symbols, perform name lookup, or add
   shape guards. Generic width constraints preserve this direct access by
   monomorphizing the concrete type and carrying the resolved ordinal.
7. The public language surface has no nominal `Object`, `Record`, or `Json`
   source value type. `JSON` is a prelude namespace and cannot be used as a
   variable type. Schema-less source annotations use `JSON.Value` or
   `JSON.Object` explicitly.
8. `JSON.Value` is the closed recursive boundary domain `null | bool | int |
   float | string | Array<JSON.Value> | JSON.Object`. It is not `any`: it has no
   dot access, computed subscript, iteration, or `len` operation. Code must
   commit to a typed schema, use `JSON.asObject` / `JSON.asArray`, or use the
   `JSON.Path` API.
9. `JSON.Object` is a pure alias for `Map<string, JSON.Value>`. It has exactly
   the Map identity, API, insertion-order rule, ownership behavior, and entity
   storage. The VM uses `XrMap`, AOT uses `xrt_map_t`, and a `JSON.Value` object
   arm may only tag or reference that same Map; no second dynamic JSON-object
   store is permitted.
10. JSON scalar values (`null`, `bool`, `int`, `float`, and `string`) widen to
    `JSON.Value` only where an explicit target type requires it. Widening does
    not influence unconstrained inference or overload preference and must not
    allocate, traverse, deep-copy, or call a generic conversion helper.
    Structural objects, arrays, nominal values, and other composites require an
    explicit `JSON.value(...)` conversion and a compile-time encodability proof.
11. `JSON.Encodable` and `JSON.Decodable` are compile-time capabilities rather
    than runtime interfaces. Unsupported functions, opaque handles,
    unconstrained type parameters, or recursively unsupported fields produce a
    stable compile-time rejection; serialization and parsing never fall back to
    dynamic best-effort behavior.
12. `JSON.parse<T>` directly constructs a decodable target from the token
    stream. Unknown fields default to `JSON.UnknownFields.Reject`; ignoring
    them requires the explicit `Ignore` policy. It must not first materialize an
    unowned schema-less DOM.
13. `JSON.parseObject` directly constructs the canonical Map-backed
    `JSON.Object`, while `JSON.parseValue` accepts an arbitrary JSON root.
    `JSON.decode` and `JSON.decodeObject` consume existing schema-less storage
    without introducing a structural-object/Map conversion protocol.
14. `JSON.parseWithRest<T>` accepts only a top-level exact structural object or
    a supported derived nominal object. It constructs `value: T` and a
    top-level `rest: JSON.Object` in one parse, with explicit Reject/Ignore
    policy for unknown fields nested under known fields. It does not claim
    recursive rest preservation or byte-for-byte proxy fidelity.
15. `JSON.Path` is an array of string object keys and integer array indices.
    The same get, require, contains, set, and remove semantics apply whether the
    root is `JSON.Value` or `JSON.Object`; path traversal never adds dynamic dot
    or bracket behavior to `JSON.Value` or structural objects.
16. VM and AOT consume the same stable exact-shape identity, canonical field
    ordinals, codec capability decisions, unknown-field policy, Map-backed
    object domain, and path semantics. Missing or stale required evidence is an
    error; neither backend may silently fall back to name lookup, open-row
    dispatch, a second object store, or decode through an intermediate DOM.
    AOT consumes the verified `XgJsonCodecSummary` row directly; it must not
    copy that row into a private codec action, evidence, or rejection plan.
17. AOT structural-object shapes are process-lifetime file-scope descriptors.
    Each instance borrows one descriptor; its header contains no extension-map
    pointer, owned field-name array, or other dynamic-key metadata. Equal
    structural identities are established by stable shape keys and field-table
    equality rather than descriptor or class pointer equality.
18. Bytecode persists the stable shape manifest, exact field identities, JSON
    codec plans, and policy operands required by the VM. Incompatible manifests
    are rejected by the bytecode version and verifier rather than reconstructed
    as a weaker dynamic model.

## Digest anchors

anchor-sha256: src/shared/xobject_shape.h ab7d5fdd49717836bcebd9d5af50d98054b71c698d0ecfa0f8e66a3cc1d97856
anchor-sha256: src/runtime/value/xtype.h 119e472f269de280d3859a8b2cb030f521bbd86b1251ed62e655c97471415df4
anchor-sha256: src/runtime/value/xtype.c cceecfaf0314ef90698dfc9e13d7adb6ef5baa9ebe4c451332cce4771bfcfd2c
anchor-sha256: src/frontend/parser/xtype_ref.h 7e1383ce9bf8e2c1cdf8008163a7c95c614651c7134d18c744f1871980e2b7b1
anchor-sha256: src/frontend/parser/xtype_ref.c 534687dba660afca5766f5833652a07615f17c47ed113d244df4edaa9025bf0d
anchor-sha256: src/frontend/analyzer/xanalyzer_capability.h 24822aedfbe4c40e2f3916c8bc804bae3b6d924f0f54986d3e359db6b97a3342
anchor-sha256: src/frontend/analyzer/xanalyzer_capability.c 631929798226365ce84a13b8205600c04b4b57816adc99300d2c455cc1b2685f
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_expr.c ffe661b543bd26af4195e0121e4c80db30f2f54bc0690456b8d556c759b58843
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_call.c 6ff80ebb347cdb101415c149121dbc991cbb3d9d147b3e8f082460167b6b27e1
anchor-sha256: src/frontend/analyzer/xtype_ref_resolve.c b9fbf7dd3eaaaa4306c0cc6b932bc954f254e8630159f03d1e7f714d18cc00c4
anchor-sha256: src/analysis/xglobal_summary.h cd374efb1ead868680eaa60fb5aa0eb23742a70a57815cec054e38f84e616230
anchor-sha256: src/ir/xi.h 15f047ffecad4967687608cde8c42adb8d0cf1c069d351f6c4125108dd61afb4
anchor-sha256: xisa/xi/ops.def b0ee1506901125db0b442d3df89cad7b6efdc27699c54eb79e4b7eff9ab8a6c7
anchor-sha256: src/aot/xrt_coll.h f699e3aecd8f3c408deca50e306274be74d0d700a61b29ca1dd170be48086511
anchor-sha256: src/aot/xi_cgen_dispatch_helpers.inc.c 7cede8bd2f4387becdcc418822ab5c54aad3573ec3262dad56363b5feae11f41
anchor-sha256: src/aot/xi_cgen_program_entry.inc.c ea9c4ac67c31537fc6f41326f09481e702c1592f159946d54da89eae8a5a1baf
anchor-sha256: src/runtime/class/xclass.h b284519f255629f4a99be7677768841a0a494f8cecc0be73ead793f9507240b3
anchor-sha256: src/runtime/class/xinstance.c af51b5c0d8398b3d509c1f951f9aec062e4b91fd87d5f0eab112fe57ff6b8998
anchor-sha256: src/runtime/object/xjson.c 38ba42610108b17fafa212ffef21ae20eb8e9cc77de7347f495432a8f6dc6d77
anchor-sha256: src/module/xproto_codec.h 109ec696fedec4c86fafe43632f8551441f8f1e4c2576a00701c1f2f2750aa3d
anchor-sha256: src/runtime/object/xjson_serde.c 5ed2dcf59b03a663ef6447b57660bc970b93bbd1b193c8b2cec8112a173fc3e2
anchor-sha256: stdlib/types/json.xr 9b4904f85a13bea98b04c616185183f921ed09761bde1cb48d751784fef48161
anchor-sha256: src/module/xproto_codec.c a38e80f950df3be9b5b367af3de31f5dc83e26b75f3d218fe923918629875de5
anchor-sha256: src/aot/xaot_verify.c 05f64e17ba6b9afed57cdb01a4630305238ba9fa2a7b494086f741dc3b3313d8
