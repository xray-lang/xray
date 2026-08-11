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
anchor-sha256: src/runtime/value/xtype.h e0f9c44c615d8a91f501d3952b1804c793abe68e3eba39744c9a000ae00f10cf
anchor-sha256: src/runtime/value/xtype.c 319ad79e6cf938cb923eaa0ddfd5d8af4b54b1637edefa1fc3a87895dadb4f82
anchor-sha256: src/frontend/parser/xtype_ref.h 47900a5055b258d4bd950b09ed56f0e6df0d51f8d837e11580672907a975f877
anchor-sha256: src/frontend/parser/xtype_ref.c 69d8b89f30264f3a17dc67e32b1eb3677a9ec43539ec457668a1ef578c6df07f
anchor-sha256: src/frontend/analyzer/xanalyzer_capability.h 24822aedfbe4c40e2f3916c8bc804bae3b6d924f0f54986d3e359db6b97a3342
anchor-sha256: src/frontend/analyzer/xanalyzer_capability.c ac463c04a436c01e9f370cadfa88eacf1e0d1c70722339991c9fe1aa246b7ea7
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_expr.c f6176a2650e660e65b347bde03f53fc9948ac07136b35eaa8be7de9528a4ec52
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_call.c db1c1bfdce0e098e294852bc67faf37ad8c3bc4ecbfd9410a8698a82132acc28
anchor-sha256: src/frontend/analyzer/xtype_ref_resolve.c 1375247b83e16da337634775d3445506f12c68bf51055832b5e0e9eff7416349
anchor-sha256: src/analysis/xglobal_summary.h 25ad56e34bde00633b74d06673681df3bd2d6e3950604929a78718d2ec382115
anchor-sha256: src/ir/xi.h 1e1d13b755e59794421273c5b9242e6391b76543fc03b3d3a0cb340c10a023be
anchor-sha256: xisa/xi/ops.def 0dbc1ff963d8dc9251052798f93e8a898e6cb93d2b400d1867457ec7caeedf4f
anchor-sha256: src/aot/xrt_coll.h 54bb705175e911ad6a1aaba76c213336b6edea5d8a73126d200d74845e4dcacc
anchor-sha256: src/aot/xi_cgen_dispatch_helpers.inc.c 5b0baa61f2d741ba6129b233f2c698711ca4eeb817dec5e71ee2ff0ed9284ea1
anchor-sha256: src/aot/xi_cgen_program_entry.inc.c b4201c02fc214411accff3be9a6f92b394c9116f86b79fc2b41a5e576a4a7d65
anchor-sha256: src/runtime/class/xclass.h 1442dd4d7e81626acd3c58020e5353511c5c0b0554c9782db49bcf834b906911
anchor-sha256: src/runtime/class/xinstance.c 6825cc9a5d9db270bd569619a0ca5cf68aaef232e7ddd6bcd83868e991b389d2
anchor-sha256: src/runtime/object/xjson.c 4e810c0a2821bbdc2735b88468e0c8229b15931c7b5def75d106cc957c22329c
anchor-sha256: src/runtime/object/xjson_serde.c 27b2d72aabf93e95a66c4f56446310e9745ce715c8d14548d08d885d634bcfaf
anchor-sha256: stdlib/types/json.xr 9e473fe3884a61f6e903c64d3512b27a9d2d2f25f3586226090eb3d644e8486d
anchor-sha256: src/module/xbytecode_io.h 42725948cca8141e45b0f51acca9b522d88006b724ea87dce31904bdf9f12c81
anchor-sha256: src/module/xbytecode_io.c 8421823a5591110ab5b12818cc569eeed115d1336f3ddc67c480f6d3eff47c1e
anchor-sha256: src/aot/xaot_verify.c 815ee7437c68182abad1ab098615c305b5b4950fec71b5bfc6a08437715065fe
