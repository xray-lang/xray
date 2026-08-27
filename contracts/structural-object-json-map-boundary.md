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
anchor-sha256: src/runtime/value/xtype.h d4084248d406b13acd38b2f8dfbbd593eeda7d8868b6ca12bb301e9e2c740ba2
anchor-sha256: src/runtime/value/xtype.c 319ad79e6cf938cb923eaa0ddfd5d8af4b54b1637edefa1fc3a87895dadb4f82
anchor-sha256: src/frontend/parser/xtype_ref.h cb42e858a220cb1ad723f680b685875bec6d845a0837dc6c45418a322356038c
anchor-sha256: src/frontend/parser/xtype_ref.c 2a7d52e0b71976c8ae33db0872f4444340a8240dd3ee45fa29653a136ef9946d
anchor-sha256: src/frontend/analyzer/xanalyzer_capability.h 24822aedfbe4c40e2f3916c8bc804bae3b6d924f0f54986d3e359db6b97a3342
anchor-sha256: src/frontend/analyzer/xanalyzer_capability.c ac463c04a436c01e9f370cadfa88eacf1e0d1c70722339991c9fe1aa246b7ea7
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_expr.c c1e35c20048b11b58ff084cefb189bd303727f173c585bbfe778a80aa689b0d5
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_call.c d1c86a031eb0f9fdbbbacb2680e25bb9953b1dac3f50549035a9d1a01a8279a6
anchor-sha256: src/frontend/analyzer/xtype_ref_resolve.c 10a842d1d2fde691f560e9bc209a77d11b9f6fbda8fc5004c5a00180009a4562
anchor-sha256: src/analysis/xglobal_summary.h 5b9a69ed62c0297b1878507d41008b7c02586181449779d86c641d58a248691e
anchor-sha256: src/ir/xi.h 089354e3324b62754b0c53c65f9e7451e735ce3bcbdcebcd3afc33c6ba44336e
anchor-sha256: xisa/xi/ops.def cd873e45558cba139d9675bcf9dce00223a514d3b4d811b494c55da65779de7d
anchor-sha256: src/aot/xrt_coll.h 37e45c48a5f5a68e523a853ddf3d557d3ee6976337d7ab620df4d88d39228879
anchor-sha256: src/aot/xi_cgen_dispatch_helpers.inc.c 743a8a37bea8b103a494ab76ce15e3807208963819750bd9982ff044cec8be49
anchor-sha256: src/aot/xi_cgen_program_entry.inc.c 187dd4fd0f00f13d3cb46a5a76651334f47529d5f41629915497fa80fe1570e2
anchor-sha256: src/runtime/class/xclass.h 1442dd4d7e81626acd3c58020e5353511c5c0b0554c9782db49bcf834b906911
anchor-sha256: src/runtime/class/xinstance.c 6825cc9a5d9db270bd569619a0ca5cf68aaef232e7ddd6bcd83868e991b389d2
anchor-sha256: src/runtime/object/xjson.c 4e810c0a2821bbdc2735b88468e0c8229b15931c7b5def75d106cc957c22329c
anchor-sha256: src/module/xproto_codec.h 109ec696fedec4c86fafe43632f8551441f8f1e4c2576a00701c1f2f2750aa3d
anchor-sha256: src/runtime/object/xjson_serde.c 5fa5d9147857c3e210414ee14b8e591463a6c8df8cbc7849cfc8a500ccb91157
anchor-sha256: stdlib/types/json.xr 9b4904f85a13bea98b04c616185183f921ed09761bde1cb48d751784fef48161
anchor-sha256: src/module/xproto_codec.c fd0259678a73d79bd9703d4c736f2fb3ef9f9b67c3b8a172416f0e6bbce85688
anchor-sha256: src/aot/xaot_verify.c 77792254ac2246618a0eff6644fae9d6107bf5a4afcc1b1333f1275f45b4eb0e
