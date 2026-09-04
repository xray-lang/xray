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
anchor-sha256: src/runtime/value/xtype.h e8379d7f493b364c1a1cb1e57d3c39a15ec142a53341dc9334ede38534be10ec
anchor-sha256: src/runtime/value/xtype.c baf6938e5dc336f7a70be7dbbabf6e8bfff9f532ec1e4775637857b496454a86
anchor-sha256: src/frontend/parser/xtype_ref.h 7e1383ce9bf8e2c1cdf8008163a7c95c614651c7134d18c744f1871980e2b7b1
anchor-sha256: src/frontend/parser/xtype_ref.c 534687dba660afca5766f5833652a07615f17c47ed113d244df4edaa9025bf0d
anchor-sha256: src/frontend/analyzer/xanalyzer_capability.h 24822aedfbe4c40e2f3916c8bc804bae3b6d924f0f54986d3e359db6b97a3342
anchor-sha256: src/frontend/analyzer/xanalyzer_capability.c ac463c04a436c01e9f370cadfa88eacf1e0d1c70722339991c9fe1aa246b7ea7
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_expr.c 5c043e015e4727443bf965a18eb0612d244c2bce029266b1dd4de3e46619c9a2
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_call.c 62812f59be934d3a0b660efff8dd177e439934145ee7cc2fb93233766302ad06
anchor-sha256: src/frontend/analyzer/xtype_ref_resolve.c a2d6f09318788dc331585c6a0c1a7f8af60ff8027c09e44fc3c54eaeecf828d0
anchor-sha256: src/analysis/xglobal_summary.h 753ec6e76928c1788a38a4c9e89ac72cb0ead0d8b7e4176464e9518bdbe44e3e
anchor-sha256: src/ir/xi.h 3160abfd948d12cbe076ed8d1c24815ec3e56eb32ec38f4989e8fea7deb4e3dd
anchor-sha256: xisa/xi/ops.def 48d6806b8d845848532b64e4d111d957bd2f2a7699771e9b02687a53b3167ff5
anchor-sha256: src/aot/xrt_coll.h d04cd4b87bbe28e76f75fb3d68077f53976f7a3d91dcb86b307ba63f31fb41a4
anchor-sha256: src/aot/xi_cgen_dispatch_helpers.inc.c 465aad140562f09d026c02a0efedc196a67364f3876a44032a2ea9a6164609bb
anchor-sha256: src/aot/xi_cgen_program_entry.inc.c ea9c4ac67c31537fc6f41326f09481e702c1592f159946d54da89eae8a5a1baf
anchor-sha256: src/runtime/class/xclass.h e9474712cbebd7dc3006e6888fec8e5efe8f7ab54add792d5e6358aa324d8abf
anchor-sha256: src/runtime/class/xinstance.c 1c48741cb48a374c07ae86aefb9ac97f92681c87d51faa2a2e2a6d246ef811d6
anchor-sha256: src/runtime/object/xjson.c 38ba42610108b17fafa212ffef21ae20eb8e9cc77de7347f495432a8f6dc6d77
anchor-sha256: src/module/xproto_codec.h 109ec696fedec4c86fafe43632f8551441f8f1e4c2576a00701c1f2f2750aa3d
anchor-sha256: src/runtime/object/xjson_serde.c 5ed2dcf59b03a663ef6447b57660bc970b93bbd1b193c8b2cec8112a173fc3e2
anchor-sha256: stdlib/types/json.xr 9b4904f85a13bea98b04c616185183f921ed09761bde1cb48d751784fef48161
anchor-sha256: src/module/xproto_codec.c 4221d03406f20d5c8174edb3c4e64b28e89fde25fb3f5b5a2805211097cf1325
anchor-sha256: src/aot/xaot_verify.c d9927f5b3e5ef7f31beb45d7497844bed8724e5b924378cd1c880b27d26f36ce
