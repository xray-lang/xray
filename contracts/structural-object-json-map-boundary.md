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

anchor-sha256: src/shared/xobject_shape.h 5ec13e213e6ebeb17c2b80f23955534666e7bd016e7632052eb900174c4d607e
anchor-sha256: src/runtime/value/xtype.h e0f9c44c615d8a91f501d3952b1804c793abe68e3eba39744c9a000ae00f10cf
anchor-sha256: src/runtime/value/xtype.c 7b75bb3a18d533d9d2780386c5043e965f3fd012320ef0cb670a119a33836475
anchor-sha256: src/frontend/parser/xtype_ref.h 47900a5055b258d4bd950b09ed56f0e6df0d51f8d837e11580672907a975f877
anchor-sha256: src/frontend/parser/xtype_ref.c 7572cd51332a4fde3f4b27222b06d649e429cfc42e79cdf669f6fbf9b15a43b6
anchor-sha256: src/frontend/analyzer/xanalyzer_capability.h 24822aedfbe4c40e2f3916c8bc804bae3b6d924f0f54986d3e359db6b97a3342
anchor-sha256: src/frontend/analyzer/xanalyzer_capability.c ac463c04a436c01e9f370cadfa88eacf1e0d1c70722339991c9fe1aa246b7ea7
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_expr.c ce05d159b7aa9283e4b1389a27f362273e73b21a3b725acd705aa9346393bc60
anchor-sha256: src/frontend/analyzer/xanalyzer_visitor_call.c c02c334e8832cbb15d47fc903460babe74240e14f7a82ebab81bea04df1b8eba
anchor-sha256: src/frontend/analyzer/xtype_ref_resolve.c a7aba08eee8727e6c3298d1c97246c0bae80ff2437690dcf0cf63775f745bcb1
anchor-sha256: src/analysis/xglobal_summary.h 320c938bbccd0b50b6035a8712417699e4e56b934676b92ef083065b6c2065a2
anchor-sha256: src/ir/xi.h 2c667dda4d9d1d1862dd823995302f006ca0f9b7c5ce3ea8b82dfaa31cc54ea7
anchor-sha256: xisa/xi/ops.def 1c8673cae5118ab8a8cd7f4c3c6820fb55982e509ea6b81a7c8e33e9fd7c3700
anchor-sha256: src/aot/xrt_coll.h aecb7ab9fa6e51e303deafbd82ce1eb5035a2d32357edf269b90b16cb5a8e836
anchor-sha256: src/aot/xi_cgen_dispatch_helpers.inc.c 85fa12cd74b1f7a98de2fe9466d78c246ac8a579ecfdf80ee9ac5bfd20c64af1
anchor-sha256: src/aot/xi_cgen_program_entry.inc.c f6afb1ccff465f73ee3ddec54fd659f7b8a9f6a2499fff356d850c5099da6d47
anchor-sha256: src/runtime/class/xclass.h 78316f01235ffc14a288b5f2a3b5cef43686cd1a8ef81bf196874861c6bb8cb8
anchor-sha256: src/runtime/class/xinstance.c 6825cc9a5d9db270bd569619a0ca5cf68aaef232e7ddd6bcd83868e991b389d2
anchor-sha256: src/runtime/object/xjson.c 4e810c0a2821bbdc2735b88468e0c8229b15931c7b5def75d106cc957c22329c
anchor-sha256: src/runtime/object/xjson_serde.c 34879f782fcc753874fbfca4c195b304c6528e356972ad03e93a42f72a93cf01
anchor-sha256: stdlib/types/json.xr 9e473fe3884a61f6e903c64d3512b27a9d2d2f25f3586226090eb3d644e8486d
anchor-sha256: src/module/xbytecode_io.h 48ea35dc6cd5a6236d4c7dbd633c9908fb0920eaa59be8228d16f81a34863c46
anchor-sha256: src/module/xbytecode_io.c e3747493e8efa24a4e3bb41bbc750794b051140a7a341e4f6732d11170b38ffb
anchor-sha256: src/aot/xaot_verify.c 9f4bbdb1adb242c60cdae763ee53ba9d9145e2954dbef8d482b5561aba7983bb
