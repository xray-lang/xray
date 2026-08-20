# Cross-target ABI contract

Status: re-frozen after the first exact non-empty cleanup authority was cut
over for explicit releases of fresh String-concatenation owners. Semantic
identity and the ownership certificate select the release; TargetPlan binds
it to the exact owned dynamic slot; refinement verifies the live release edge;
and C emission schema 30 binds the same operation and slot to `xrt_release`.
Missing, extra, or mutated rows fail closed, with no selector, name, type,
arity, or cleanup inference. Raw-pointer aggregate loads and ordinary slices remain
classified as borrowed views, StringBuilder and Iterator storage became
ARC-managed, and implicit error cleanup was confined to the existing cold
propagation branch. Iterator values retain their traversed source without
changing their public value tag or body layout. The public target and Slice
ABIs are unchanged.
The embedded-bytecode program entry maps every nonzero VM evaluation result to
`EXIT_FAILURE`. This normalizes the host process status without changing the VM
result contract, generated bundle ABI, or runtime representation.
Portable SIMD values crossing hosted module shared slots recover their fixed
aggregate layout from the tagged reference; scalar, native, and cross-endian
lowering retain one lane-order contract.
Value-struct aggregates do the same: a shared slot always stores a boxed
XrValue, so a read planned as a native struct aggregate dereferences the payload
rather than assigning the box. A borrowed struct place parameter — a method
receiver included — keeps the native value ABI (`xrt_struct_abi_* `), recovering
its layout through the PLACE_LOAD its field ops read, and its whole-aggregate
temporary exists only under XRAY_AOT_DEBUG_LOCALS.
The name-keyed property store is the fallback for shapes that carry named
properties at run time — a Map and a JSON/record object — and it fails closed on
every other receiver. A store it cannot perform is a CGen gap, not a value to
discard: reporting it is the only alternative to a program that reads back a
value it was never given.
The same generated translation unit may now be compiled as GNU/Clang C++11,
but remains a C ABI artifact: exported definitions use C linkage, atomic fields
retain their scalar C layout and memory ordering through compiler builtins, and
typed pointer casts do not change the prepared target ABI. C11 remains the
default generated language and performance path.
An explicitly selected restricted-C90 translation unit is also a C ABI
artifact, but is confined to scalar, freestanding, shared-library core graphs
for LP64 Linux or Darwin targets. Its public scalar ABI uses C90 primitive
integer types guarded by width checks, and the build fails closed before host
compilation if the reachable graph needs ordinary runtime, standard-library,
native-input, aggregate-export, main-entry, or SIMD support. This opt-in lane
does not alter the default C11 or generated-C C++ target ABI.
Task 245 adds provider code-shape capability probes and typed adapters without
changing the selected target ABI: fallback may change the compiler provider,
never pointer width, calling convention, object format, runtime artifact, or
native target identity.
Task 253 adds per-function return-ownership metadata to analysis and Xi. The
metadata controls caller ARC placement only; it does not change the public
value representation, calling convention, parameter list, or native return
ABI.
Resolved import references additionally carry the callee function and owning
module they bind to, so ARC can read a cross-module callee's borrow signature
and keep caller-side ownership of an argument the callee only borrows. Like the
return-ownership metadata above, this controls caller ARC placement only: the
public value representation, calling convention, parameter list, and native
return ABI are unchanged, and an unresolved reference keeps the conservative
moved-argument convention.
A native SOURCE_EXPORT call does not rediscover that ABI from the resolved Xi
import. Its verified TargetPlan row binds the dependency fingerprint, export
and callee stable identities, and one dense argument row per dependency
parameter. A `ref Ptr<T>` row must name the exact caller `LOCAL_ADDR` value,
addressable operand contract, dependency parameter ordinal, writeback mode,
caller slot, and RAW_PTR machine representation. Prepare resolves the unique
dependency ABI only through those identities, and C emission projects the
additional pointer level only from that row. There is no import-name, Xi-type,
shared-slot, or legacy value-plan fallback.
SOURCE_EXPORT suspendability is intentionally absent from a standalone
SemanticPlan: the dependency plan is the only owner of the exported callee's
coroutine fact. That unresolved fact propagates only through exact
DIRECT_LOCAL and local source-method target rows, never through a name or type
guess. The ordered module-set verifier independently reconstructs the frozen
dependency callee, closes the transitive local call graph, and requires every
SOURCE_EXPORT and dependent local call to carry exactly the resulting
coroutine-state row before TargetPlan extraction may begin.
Task 254 makes mutable capture cells explicit in Xi and changes the internal VM
cell opcode operand shape. Closure upvalues remain tagged `XrValue` slots and
the public target ABI, calling convention, and closure layout are unchanged.
Task 257 changes provider probe capture from implicit C text to bounded byte
buffers. Target triples are accepted only as strict ASCII; this changes no
target identity, generated-C ABI, calling convention, or runtime layout.
Task 256 replaces parallel VM/AOT value declarations with the versioned public
`xray_value_abi.h`, introduces `hosted_fragment` as a real artifact kind, and
generates the first scalar stdlib fragment from authoritative Xray source. The
fragment borrows the embedding runtime and exports only manifest-declared C ABI
entries; it cannot synthesize a program main, runtime implementation, or
constructor lifecycle.
Hosted Fragment ABI v7 adds a layout-neutral, call-scoped byte-span view for
`Array<byte>` and `Slice<byte>`. Generated fragments receive only `(data,
length, readonly)` from the host operation table and never reinterpret a VM
container header. A mutable `ref` parameter rejects readonly provenance before
entry; this explicit ABI revision changes no Xray value tag or native Slice
layout.
Each Xi function now carries its exact immutable SemanticPlan function index.
Hosted adapters use that record to promote a borrowed reference result before
releasing adapter-owned arguments. The index is compiler-internal and the
balanced retain changes no public value representation, calling convention,
parameter list, or native return ABI.
Task 260 carries recursive typed-Json schemas across VM bytecode and generated
C. Decoding a derived value struct writes its existing native aggregate layout;
heap materialization uses the registered type destructor and storage promoter.
This adds no public field, tag, calling-convention change, or per-instance
metadata to the target ABI.
Task 275 requires a current frozen coroutine plan before AOT may lower an
uncaptured closure to a direct C symbol. A missing, stale, incomplete, or
suspendable plan keeps the runtime closure representation; AOT must not
rediscover synchronous behavior from Xi. This tightens compiler authority
without changing the public closure layout or calling convention.
Representation refinement is likewise retained as one immutable authority per
module, bound to the exact SemanticPlan, TargetPlan, target policy, and backend
materialization. AOT prepare runs only after every required BOX or UNBOX is
present at its recorded source and use, and rejects a missing, extra, reordered,
or stale adapter before ABI planning. This adds no public value representation,
layout, or calling-convention change.
Refinement asks which storage a value holds, not which instruction defined it
or which one consumes it. A reference family that carries its own carrier names
that carrier first and keeps its priority, and a use site that requires a
carrier other than the operand's own states it at that site. Where neither
speaks, the representation the TargetPlan already froze for the value is the
answer on both sides: the value is in that storage, and a use site naming no
carrier of its own consumes it there, so no adapter is recorded between a
storage and itself. That binding is made from the value's semantic type,
uniformly for every operation result and parameter, so re-deriving it per
opcode restated one rule once per spelling and refused any spelling nobody had
written yet. Fail-closed moves onto the storage fact and stays there: a value
the TargetPlan bound no representation for, or bound to a class this authority
names no storage for -- object, code, dynamic, aggregate, vector, view, and
every nullable or aggregate type -- is refused exactly as before, and no
storage may be guessed from an Xi type, an opcode, or a use site's shape.
`XI_TAIL_CALL` remains an exact native Xi opcode through AOT prepare and uses
the source-generated portable-C call driver. Before emission, an independent
conformance verifier binds every frozen tail operation to its unique live Xi
member, ordered operands, direct-local TargetPlan call, exact callee, and
applied direct-call authority record. Missing, duplicate, reordered, stale, or
ordinary-`XI_CALL` substitutions fail closed; prepare has no normalization or
compatibility path. This changes no public ABI or plan schema.
The C emission projection schema 30 preserves the exact materialization recipe
and immutable byte payload for every verified String literal row. CGen
mechanically consumes that row and cannot recover literal bytes, a dynamic
tag, field spelling, or ownership from mutable Xi values. Missing, extra,
reordered, stale, or incorrectly spelled rows fail before emission; this does
not authorize general owned Strings, tuples, or object bodies.
For an exact String concatenation, C emission schema 30 freezes every ordered logical
operand identity separately from the exact source identity consumed by C. An
owned String remains a tagged part. An exact non-null, non-aggregate `u64`
part is selected only by its verified TargetPlan U64 register and memory rows
and emits `xrt_strpart_init_u64` directly. Representation refinement may insert
one backend-only BOX adapter for the logical tagged use; CGen may elide that
adapter only after the immutable recipe, both semantic identities, and the
exact AOT adapter independently agree. Missing, swapped, widened, stale, or
wrong-kind rows fail closed, and selector text, mutable Xi types, or tagged
fallback display grant no authority.
Schema 16 also carries exact unaliased SemanticPlan `Ptr` and `MutPtr` values
through TargetPlan `RAW_PTR`, target-profile pointer layout, and a trivial,
non-root, null-zero lifecycle into the immutable C emission row. Builder,
Target verifier, AOT prepare/verifier, and C-emission verifier independently
reconstruct that identity; C constness comes only from the frozen SemanticPlan
type key. No mutable Xi type, value name, or legacy plan can supply or repair
raw-pointer representation authority.
Schema 14 additionally projects the sealed `StringBuilder()` Target call as an
owned `TAGGED`/`XrValue` result with the fixed zero-argument `xrt_strbuf_new`
recipe. Ordinary and coroutine CGen mechanically consume that recipe; neither
may rediscover the builtin by Xi auxiliary text or fall back to generic
allocation. The recipe grants no generic builtin, object layout, root-map,
cleanup, alias, or allocation-table authority.
Schema 14 also preserves the exact borrowed `TAGGED`/`XrValue` row for a frozen
direct-local shared callee token. CGen consumes that immutable row mechanically;
it cannot infer callable representation from Xi type or representation state.
The live materialization verifier walks the caller parent chain to the frozen
callee's first lexical shared-slot owner and requires that owner's unique child
and slot pointer to match, so root-owned sibling helpers are accepted without
name or type guessing. The row grants no closure body, allocation, root, or
cleanup authority.
For an exact scalar `XI_CHAN_TRY_RECV`, schema 30 preserves the receiver semantic
value and exact scalar unbox helper spelling. Sync and coroutine CGen consume
that recipe mechanically; they cannot infer it from Xi type/representation or
fall back to a legacy adapter. This grants no Channel object layout, receive
scheduling, ownership transfer, aggregate/tuple payload, root, or cleanup
authority. For the exact String byte-slice intrinsic, schema 30 preserves the
borrowed `xr_span_t` view, its source semantic value, and the fixed
`xrt_span_from_string_bytes` recipe. CGen has no selector-, alias-, or
type-derived fallback; this grants no generic String method or Slice ABI.
For an exact payload-bearing source enum constructor, schema 30 owns the
immutable enum layout ID, discriminant, declaration/member spellings,
namespace receiver identity, and ordered payload semantic identities. Target
call rows authorize the constructor and tagged result; CGen only consumes the
verified recipe and cannot infer its type, selector, arity, or representation
from Xi. Direct-local enum arguments may bind distinct owned/borrowed physical
representation rows, but their caller and callee ABI remains exactly tagged.
For exact `Array.withCapacity` and `Array.filled` constructors, schema 30 owns
the constructor kind, element storage, owned dynamic result, fixed runtime
symbol, and ordered capacity/count/fill semantic identities. The Target call
and argument rows bind those facts before refinement, and CGen consumes only
the verified recipe; selector text, Xi type, packed immediates, and mutable
operand metadata grant no fallback authority. This does not authorize any
other Array intrinsic, direct-local container ABI, span representation, root
map, or cleanup behavior.
For exact scalar `Array.fill(value)`, schema 30 owns the receiver and fill
semantic identities, scalar element storage, receiver-alias result, exact
Target call identity, and immutable C materialization recipe. SemanticPlan,
TargetPlan, refinement, and C emission independently reconstruct the same
two-operand call; selector text, live Xi types, range-fill overloads, mutable
metadata, and generic method dispatch grant no authority or fallback.
For exact `String.runes()`, schema 30 owns the borrowed String receiver, owned
`Iterator<rune>` dynamic result, exact Target call identity, fixed
`xrt_string_runes` symbol, slot, layout, and extent. Refinement independently
rebuilds the same frozen SemanticPlan and TargetPlan facts, while CGen consumes
only the immutable recipe. Selector text, live Xi types, arity, and mutable
operand metadata grant no fallback authority; this does not authorize other
Iterator members or a general method ABI.
For exact `Iterator<rune>.hasNext()`, schema 30 owns the scalar boolean result,
the immutable `xrt_iterator_rune_has_next` recipe, and the receiver identity
already proven by the exact prior `String.runes()` authority. Refinement
independently reconstructs the SemanticPlan and TargetPlan call facts before
CGen mechanically consumes the recipe. Selector text, live Xi types, arity,
and mutable operands grant no fallback authority; this does not authorize
`next`, `nth`, another Iterator specialization, or a general iterator ABI.
For exact `Iterator<rune>.next()`, schema 30 separately owns the trivial native
rune result and immutable `xrt_iterator_rune_next` recipe. The shared semantic
judgement first proves that its receiver is the unique same-function result of
an exact frozen `String.runes()` operation; TargetPlan, refinement, and
CEmission independently rebuild that chain. CGen consumes only the projected
operand identity and helper spelling and preserves the pending-error poll. No
selector, live type, arity, `nth`, other Iterator specialization, or generic
method path can substitute for that authority.
For exact `rune.toUInt32()`, schema 30 owns the trivial native-u32 result, its
single native-Rune recipe operand, and fixed `xrt_rune_to_uint32` helper. The
semantic receiver must be the unique exact `Iterator<rune>.next()` result in
the same function, and TargetPlan, refinement, and CEmission independently
rebuild that chain. CGen consumes the frozen recipe without selector, live
type, arity, or generic-method fallback.
For exact `rune.isWhitespace()`, schema 30 owns the trivial native-bool result,
its single native-Rune recipe operand, and fixed `xrt_rune_is_whitespace`
helper. The semantic receiver must be the unique exact
`Iterator<rune>.next()` result in the same function. TargetPlan, refinement,
and CEmission independently rebuild that chain, and CGen consumes only the
frozen recipe. This does not authorize `runes().nth(...)`, another Rune
predicate, selector dispatch, live-type inference, or a generic method ABI.
For exact `Array.reserve`, the existing array-member Target call binds the
stable Semantic intrinsic, array receiver, signed capacity value, receiver
alias result, and owned dynamic slot. The same family idempotently supplies the
dynamic layout when the receiver originated as an array literal. Representation
refinement independently freezes the receiver's tagged carrier and the native
signed capacity operand from the same exact Target call. CGen emits an unused
result as a raw effect statement from that authority and cannot recover it from
selector text, live Xi type, arity, or legacy auxiliary metadata. This reuses
existing plan records and changes no serialized schema or public ABI.
For exact `String.slice(start, end)`, schema 30 owns the unique frozen String
receiver identity, two ordered i64 bound identities, owned dynamic result slot,
tail call identity, and fixed `xrt_string_slice_range` recipe. Refinement owns
the two exact tagged-call scalar adapters, while CGen projects the recipe bounds
to native i64 and preserves pending-error polling. No selector, live type,
arity, `nth`, one-argument slice form, or generic method path can substitute for
that authority.
An AOT cross-execution transfer row binds its site and payload to exactly one
representation authority. A TargetPlan value binding and a legacy value row
are mutually exclusive; the only accepted legacy rows are the independently
verified enum-ordinal and backend representation-adapter families. Transfer
verification reconstructs the site, payload, mode, action, storage domains,
proof identities, and evidence without calling the prepare collector. Missing,
duplicate, swapped, backend-only, or non-durable value authorities fail closed.
This tightens internal plan verification without changing the public target ABI.

Schema 17 additionally projects a scalar-lane fixed array as an exact backing
place. TargetPlan fixes the aggregate layout, its uniform lane rows, and the
trivial slot, and CEmission owns the lane count, native kind, C spelling, and
value whose address is projected. The allocation identity stays with the
representation authority that proves the construction, so a fixed array read
back out of the cell it was stored in projects the same backing place its
allocation does. Prepare keeps the tagged wrapper ABI while CGen addresses the
verified native backing; neither may recover this identity from a mutable Xi
type, name, arity, or representation. This authority excludes object, raw,
code, vector, nested, nullable, and non-scalar lane families.

Schema 23 additionally projects an ordinary address-taken scalar UNBOX alias as
an exact materialization recipe. SemanticPlan fixes the alias, LOCAL_ADDR use,
and source value; TargetPlan requires identical scalar register and memory
rows; CEmission freezes the initializer's semantic identity. Representation
preparation may insert backend adapters, but CGen initializes the distinct C
object only from that frozen source and never reconstructs it from mutable Xi
types, names, arity, or representation. This authority excludes object, raw
pointer, code, vector, view, aggregate, array, Iterator, and projected-place
families.

Schema 23 also projects one exact direct-local borrowed `ref Array<T>`
parameter as `XrValue *`. SemanticPlan and TargetPlan jointly bind the caller
place, callee parameter, element storage, parameter ordinal, ownership,
transfer, and dense call-argument identity. CEmission freezes a separate
call-argument recipe, and CGen consumes only that row when adding the pointer
level. A mutable Xi type, value name, arity, or representation plan cannot
create or repair this authority. This does not authorize owned/moved Array
parameters, SOURCE_EXPORT parameters, another container, or general pointer
ABI inference.

The same generation binds two further exact direct-local `Array<T>` boundaries,
and both stay in the tagged carrier instead of taking the pointer level a ref
parameter needs. A borrowed by-value parameter shares the caller's allocation
for the extent of the call, so it is bound to a borrowed dynamic parameter slot
and states no place, no element storage, and no addressability of its own; the
caller may hold that allocation owned or borrowed, so the two sides agree on
representation and are allowed to differ in ownership alone. A call result is a
transfer, so it is bound to an owned dynamic temporary in the caller's own
frame, exactly as an owned String result is. SemanticPlan, TargetPlan, and
representation refinement each rebuild the parameter mode, ownership, transfer,
and element storage independently rather than reading one another's conclusion.

What the element is stays outside both of these two boundaries. Neither ever
reaches an element: each copies one tagged value and shares one allocation, and
the element storage it states is whatever the type's own layout row records,
which is none when the element is reference capable. `Array<String>` therefore
crosses them exactly as `Array<i64>` does. The scalar element type remains a
requirement of the ref parameter alone, which hands the callee a pointer into
the caller's cell and may index and rewrite through it; a reference-capable
element there would need an element ownership and drop contract no row states.
The three carriers ask one shared judgement that takes which of them is asking
as its parameter, so the wide two cannot silently inherit the narrow one's
requirement again. None of the three authorizes an owned or moved Array
parameter, a ref result, a SOURCE_EXPORT parameter or result, or another
container.

The same generation binds two exact `string` bindings, one on each side of a
direct-local call. A String is immutable and shared and carries nothing but the
outer tagged value, so a by-value parameter is bound to a dynamic parameter slot
and states no place and no addressability of its own. What the declaration adds
is who releases the allocation: a body that only reads the string borrows it for
the extent of the call and releases nothing, while a body that consumes it holds
an owning reference and releases it itself. Consuming is not an exotic shape --
concatenation consumes its operands -- so an owning String parameter is the
ordinary shape of a function that appends to its own argument, and the parameter
slot carries whichever of the two ownerships the declaration proved. The call
site must state the same answer the declaration did: a callee that holds an
owning reference is one the caller hands its own over to, and a callee that
borrows is one the caller keeps answering for. Ownership on the caller's side is
otherwise its own business, so the two sides agree on representation and are
allowed to differ in ownership, exactly as they are for an `Array<T>` passed by
value, which is always borrowed. The value a caller reads back out of a shared
cell is the other binding:
the read borrows the one allocation the cell holds, so it is bound to a borrowed
dynamic temporary in the reader's own frame. That read is a String value in its
own right rather than something a call boundary confers, so every exact one is
bound whether or not an argument ever reaches it, exactly as an `Array<T>` read
of a shared cell already is.

The shape a shared read must have is the shape a read of any container has --
no operands, no constant, no callee, no intrinsic, a borrowed result, and a
single definition for the value -- stated once and asked by both containers,
with each proving its own result type on top of it. The declaration shape a
by-value parameter must have is stated once the same way, in the shared String
shape judgement. TargetPlan construction, TargetPlan verification and
representation refinement each ask those judgements and then prove their own
slot, representation and call rows against them, and none of them reads
another's conclusion. Every way a String can reach a use site already holding
that tagged carrier -- a literal, a concatenation, a direct-local call result, a
shared read, a by-value parameter -- is one list, so a carrier the length read
admits cannot be one a call argument, an equality, a retain, a store into a
shared cell, or a print refuses. A use that consumes a reference without caring
which container it is -- a refcount adjustment, a store into a shared cell, a
print -- asks one further list naming every reference family this authority can
name, each in the tagged carrier its own family bound, so no such site can
admit a reference another one refuses either. An identity copy gives a value
that already exists a second name and allocates nothing, so each of those
lists, and the definition oracle alike, resolves a value through its chain of
renames before asking which family it belongs to. That resolution is one shared
judgement rather than a walk each list repeats: it terminates on the operand
numbering Xi guarantees, refuses a plan that breaks that numbering, and leaves
a value that is not a rename standing for itself. A carrier therefore cannot
depend on which of its names a use site happens to spell. These bindings do not
authorize a `ref` or moved String parameter, a SOURCE_EXPORT String parameter,
an optional String, a shared read of any other container this generation has not
bound, or a String reached through a slice receiver.

Representation selection keeps every reference-capable container -- `Array<T>`
and `string` alike -- in the tagged carrier at every boundary it crosses:
parameter, argument, call result, and function return. That is one judgement
about the kind rather than a special case restated per boundary, so no
compensating BOX/UNBOX adapter is valid on any of them, and no boundary can
authorize one by disagreeing with the others.

Target semantics are selected before analysis, Xi lowering, generated-C
emission, and native linking:

The build and verification entry points derive one frozen TargetProfile from
the numeric toolchain target and canonical runtime/provider contracts, then
pass that same authority into AOT. A target spelling cannot reconstruct or
override ABI facts after planning, and an unavailable profile fails before
emission rather than falling back to compiler-host layout.

- T1: every supported target profile supplies one pointer width and byte order;
  target `usize`, pointer, aggregate, and native-load layouts derive from that
  profile rather than from the compiler host.
- T2: the native `Slice<T>` value ABI is always 16 bytes with 8-byte alignment,
  `data` at offset zero, and its signed 64-bit length at offset eight. ILP32 C
  targets carry explicit padding; they must not silently expose a 12-byte host
  structure against the frozen Xi representation.
- T3: portable SIMD reinterpretation is byte-order neutral. Logical byte zero
  is the least-significant byte of logical numeric lane zero on every target;
  big-endian C lowering reconstructs lanes instead of treating native `memcpy`
  bit patterns as the language semantics.
- T4: a named target may be published only when its profile, C-toolchain triple,
  pointer width, and endianness agree. Unsupported SIMD modes fail closed.
  Toolchain discovery is provider-neutral: target identity lives in the shared
  toolchain model, while probing resolves an installed provider for that model.
  Automatic selection tries the ordered host/provider set by capability, not by
  executable presence alone. A rejected host provider may fall back to Zig, but
  the fallback must retain the already selected target ABI (including native
  `x86_64-windows-msvc`) and pass compile, SDK, runtime-link, native-run, and LTO
  probes before it is reported ready.
- T4a: x86 runtime SIMD dispatch probes CPU and OS state together. AVX-512F is
  selectable only when CPUID leaf 7 reports AVX-512F and XCR0 enables XMM, YMM,
  opmask, ZMM high-256, and high-16 ZMM state. Baseline, AVX2, and AVX-512F
  functions remain in separately attributed feature islands. Explicit static
  SIMD selection is provider-neutral compile intent: each verified provider
  emits its own flag dialect, while `dispatch` keeps the translation-unit
  baseline free of global AVX2/AVX-512 enablement. SIMD mode and features are
  part of both object and link-output cache identities. Providers that predate
  Clang's `evex512` feature spelling retain the same AVX-512 island through the
  portable `avx512f` function target instead of dropping the whole attribute.
  Clang AVX2 islands carry a 256-bit minimum-vector-width attribute so explicit
  256-bit Xi operations are not silently legalized as paired 128-bit work.
  In a static SIMD build, an explicitly `@inline` cross-module vector wrapper
  retains hidden external linkage plus the native always-inline contract; a
  runtime-dispatch wrapper remains baseline and calls separately attributed
  feature leaves.
  A module initializer is always emitted as an ordinary hidden external
  definition because a separately generated entry translation unit invokes
  dependency initializers. It never uses external inline-only linkage; this
  preserves an exact linkable provider on MSVC and every other C provider.
- T4b: `loongarch64-linux-musl` defaults to scalar because LSX is not implied
  by the base target triple. Explicit `--simd lsx` or `--simd native` adds
  `-mlsx`, uses the portable 128-bit lane contract, and may publish native
  vector evidence only after an LSX-capable target binary executes.
- T4c: `aarch64-linux-musl --simd sve` preserves the exact lane count of the
  fixed-width vector family and gives `U8xNative`, `U32xNative`, and
  `U64xNative` a bounded runtime-selected active prefix. Hardware vector
  lengths of 128 bits select 16 bytes, lengths from 256 through 511 bits
  select 32 bytes, and lengths of at least 512 bits select 64 bytes. Inactive
  storage is not part of the language value; VM and non-SVE AOT execute a
  zero-initialized 16-byte fallback. Explicit predicated SVE intrinsics remain
  enabled, while implicit LLVM loop and SLP vectorization stay disabled until
  their fixed-trip-count lowering is valid for non-power-of-two vector lengths.
- T4d: `--c-dialect c90` is valid only for the frozen restricted profile: an
  LP64 Linux or Darwin target, scalar lowering, `--freestanding --shared
  --emit-c-only`, no program main, and no reachable runtime, standard-library,
  native-input, or aggregate public-ABI dependency. Unsupported profiles fail
  before generated C is handed to a host compiler. Dialect identity is part of
  both object and link cache keys.
- T4e: Windows native provider selection freezes the runtime archive ABI before
  probe or link. MSVC resolves `windows-msvc` COFF `.lib` artifacts; Zig keeps
  the already selected native ABI and may consume the same probe-validated COFF
  archive for a `windows-msvc` target, while an explicit `windows-gnu` target
  resolves its own GNU-spelled artifact set. Runtime manifests reject an ABI or
  object-format mismatch even when the digest is valid. A true cross-target
  probe may reuse installed host headers, but it never advertises or links the
  host runtime into the cross artifact. The COFF AOT runtime archive contains
  only VM-neutral translation units: because COFF resolves every undefined
  symbol in a selected object before section garbage collection, VM registration
  code may not share an archive member with an AOT-reachable core helper.
- T4f: provider capability identity contains four independent code-shape
  states: force-inline, preserve-call, value-opacity, and compiler-fence. A
  required state participates in selection and probe-cache identity. An
  installed host provider is tried first; when it cannot satisfy the requested
  baseline, an installed or managed Zig provider may be selected while retaining
  the already frozen target ABI. Missing capabilities fail closed rather than
  being reported as honored. The generated-C adapters are typed integer/pointer
  identities and compiler-only scheduling constructs; they cannot introduce a
  pointer-to-integer ABI round trip, hosted runtime dependency, hardware fence,
  or C++ linkage change.
- T4g: a Windows multi-module freestanding relocatable artifact is one COFF
  object compiled from the verified amalgamated translation unit. COFF has no
  ELF/Mach-O-style partial-link operation, so this path performs no link stage,
  rejects external objects, system libraries, linker flags, and linker scripts,
  and preserves an explicitly configured post-compile objcopy step. This is an
  artifact-kind lowering, not permission to substitute an archive or DLL. A
  hosted fragment selected for direct MSVC compilation must contain no GNU
  statement expression and must pass a real MSVC compile probe; failure remains
  fail-closed and does not downgrade the already selected target ABI.
- T5: a scalar place may alias its source field only when the semantic value,
  AOT representation, and generated-C type are identical. A value-preserving
  conversion such as ILP32 `usize` to 64-bit `int` must materialize distinct
  storage before its address is taken.
- T6: cross-target hosted time queries lower to target-owned header code, while
  native builds retain the runtime's shared OS clock. A cross-target binary
  must never consume the compiler host's AOT support archive merely to read
  wall, monotonic, or process CPU time.
- T7: shared-library format, suffix, link flags, and symbol visibility derive
  from the selected target rather than the compiler host. A default C export
  is externally visible; a hidden C export must not leak into the public image.
- T8: typed numeric conversion carries source and target scalar identities plus
  the selected target pointer width from analysis through Xi and VM bytecode.
  Integer conversion is modulo the target width followed by an explicit
  two's-complement interpretation; integer-to-float and binary64-to-binary32
  use round-to-nearest, ties-to-even; binary32 NaNs use the canonical Xray
  payload; float-to-integer truncates toward zero only after a range proof and
  otherwise raises `XR_ERR_OVERFLOW`. VM and AOT C consume the shared numeric
  conversion core and must not substitute host-language signed casts, the host
  floating-point environment, canonical type spellings, or compiler-host
  pointer width for this evidence.
- T9: a first-class `CFn` call may lower to generated-C `musttail` only when the
  call is the return block's final owned instruction, any following error check
  has no ARC cleanup, and the caller and callee native C signatures match
  exactly, including hidden closure parameter, arity, return type, and every
  parameter type. Otherwise CGen emits the call once and returns its stored SSA
  result without replaying side effects.
- T10: a noncoroutine native function accepts `CFn` parameters as raw native
  entry pointers. A statically proven top-level noncapturing function converts
  directly to that pointer; an already-native `CFn` forwards unchanged. The
  boundary must not allocate an Xray closure or recover a function address from
  a closure-tagged `XrValue`, and unsupported conversions fail closed. The AOT
  value plan assigns every non-nullable `CFn` the single `RAWPTR`
  representation even when the source Xi opcode historically produced tagged
  storage; CGen rejects any `CFn` coercion whose frozen plan says otherwise.
- T11: hosted AOT gives every physical coroutine one execution arena. Generic
  and embedded-header heap allocations retain their normal RC behavior while
  the arena owns the complete residual graph at coroutine teardown. A value
  published to TRANSFERABLE, CONST_SHARED, or SYNC_SHARED storage must detach
  its entire owned graph, including native-class reference fields, before the
  source arena can be destroyed. Native class type registration therefore
  carries a generated storage-promoter callback beside its destructor; missing
  graph evidence is a hard contract failure, never root-only promotion. The
  generated entry owns the root arena and shuts it down on every exit path.
- T12: `XrValue` and every object crossing VM/generated-fragment code use the
  single versioned public value/object ABI. `XrObjHeader` is the first field,
  carries the canonical object kind and ownership counters, and is validated by
  compile-time size/alignment/offset assertions on both sides. A fragment may
  borrow these objects but cannot invent a second layout, retain an unowned
  runtime root, or bypass the declared argument/return ownership convention.
  Hosted Fragment ABI v7 is the only accepted fragment interface. Contiguous
  byte input crosses through `byte_span_view`, never through a cast of the host
  Array or Slice representation; its pointer is borrowed for the call only,
  length must fit the signed native Slice ABI, and readonly provenance makes a
  mutable `ref` argument invalid rather than silently copying or mutating it.
- T13: when a native ADT aggregate crosses a tagged direct-call parameter,
  `READ` constructs a temporary box with independently retained payload lanes;
  a consuming parameter transfers the lanes without retaining. The bridge may
  change representation but cannot silently duplicate or discard ownership.
- T14: a named value aggregate receives a native C spelling only when its
  ordered field names, source declaration, scalar field representations, and
  physical layout are all reconstructible from frozen SemanticPlan and
  TargetPlan rows. CEmission stores that exact spelling and independently
  verifies it before CGen consumes it. Mutable Xi type, name, and arity facts
  cannot fill a missing row; anonymous, nested, rooted, fixed-array, raw,
  object, code, and vector families remain unsupported and fail closed.
- T15: a fixed array receives a C backing-place projection only when its
  uniform scalar element kind and count, every Target field row, the aggregate
  layout, and the trivial slot all agree. Its wrapper remains `XrValue`, but
  index and address lowering consume the immutable lane identity directly. The
  projection asks which aggregate slot the value holds, not which instruction
  defined it, so the same fixed array projects the same backing place at its
  allocation and at every later read of it; proving one exact
  `XI_FIXED_ARRAY_NEW` result, its canonical allocation identity, and its
  declared element kind stays with representation refinement, which is where an
  allocation is the subject. An element access re-proves its receiver as that
  same aggregate family and demands the single element type an index names.
  Representation selection admits one container family for the read and the
  write of an index, so a fixed-array lane keeps its native index and native
  element on both and no compensating BOX/UNBOX adapter appears on either. The
  lane kind is read as the scalar layout it selects and never tested for
  presence against zero, which is the tag of a 64-bit signed lane. A missing,
  stale, renamed, type-guessed, or non-scalar projection fails closed and cannot
  enter the named-aggregate path.
- T16: direct `Array.map`, `Array.filter`, and `Array.reduce` C lowering is
  authorized only by one verified SemanticPlan HOF operation, its exact
  TargetPlan call/result/storage rows, and one CEmission recipe naming the
  same-module uncaptured pure callback function and complete native callback
  ABI. Representation selection preserves tagged map/filter results and the
  tagged callback operand, while reduce keeps its accumulator and seed in the
  frozen scalar representation; no compensating BOX/UNBOX adapter is valid.
  CGen consumes the verified direct-symbol closure plan and emits portable C11
  statement loops. Missing, duplicate, suspendable, unreachable, captured,
  cross-module, extra-use, noncanonical-ABI, or incomplete recipe authority
  fails closed and cannot fall back to selector dispatch or the legacy runtime
  map/filter/reduce helpers. An unused reduce still executes every callback;
  only storing its result may be elided.
- T17: a tuple receives a C backing-handle projection, never a named C
  spelling. Its wrapper is `XrValue` because the construction owns a runtime
  allocation; the aggregate layout, its scalar lane rows, and the trivial slot
  supply the lane count and per-lane representation, and lanes may differ from
  one another. Representation refinement admits the same aggregate family the
  target plan itself admitted, keeps every lane in the tagged carrier across
  construction, and hands an element read back as the tagged carrier a native
  consumer adapts from. A non-scalar, nested, rooted, renamed, or stale lane
  fails closed, and no tuple may enter the named-aggregate path.
- T18: a bare structural object occupies no aggregate slot and receives no C
  projection at all. It is reference capable and roots its own ownership, so
  the shared aggregate judgement places it outside every aggregate family and
  the target plan states no representation for it; representation refinement
  admits such a value only while that absence holds, and demands it of the name
  it is asked about as well as of the allocation that name resolves to, so a
  row some other family placed can never be claimed here. The authority is
  rebuilt independently from the frozen rows rather than read back: the
  construction proves its canonical allocation identity, its field-name
  metadata and its field count; a shared-cell read proves the borrowed load
  that hands the same allocation back; and a field access proves its receiver
  as one of those two, as a field read whose own result is an object, or as a
  rename of any of them, within a fixed nesting bound. Every
  field holds a full tagged value, so a write reaches the store in the carrier
  whatever native storage its own definition named, and a read hands the
  carrier back for a native consumer to adapt from. CGen converts on both edges
  through the storage the emission plan named and never references the emitted
  local directly. A value-flagged struct object, an object crossing a call
  parameter or a call result, and any operation shape that is not exactly
  reconstructible fail closed, and no object may enter the named-aggregate
  path.

The release evidence includes generated-C filetests, the eleven-case
cross-target smoke matrix, executed PowerPC64 big- and little-endian
portable-SIMD fixtures, 180-vector xxHash VSX KATs for both byte orders, and a
LoongArch64 LSX binary that executes the 180-vector KAT plus exact 49-symbol C
ABI oracle under QEMU's `la464` CPU model, plus AArch64 SVE binaries that run
the same 180-vector KAT and exact 49-symbol C ABI oracle at 128-, 256-, 384-,
and 512-bit vector lengths. Retained SVE assembly proves that the runtime-native
xxHash stripe and scramble values remain in sizeless vector SSA rather than
fixed aggregate stack storage. Emulation proves semantic and ABI correctness,
not native Power, LoongArch, or SVE performance. A compile-only
cross artifact is not sufficient to claim platform support. AVX-512F evidence
may retain exact generated C and assembly plus execution of the same dispatch
binary on an AVX2-only host, but native AVX-512F execution and performance
remain separate release gates. Windows x86_64 evidence additionally includes
native execution of all five xxHash CLI names, a 24-case byte-exact upstream CLI
differential, the exact 49-export PE gate, an executing C ABI oracle, and complete
31-sample alternating Xray/API throughput matrices. On the verified host, MSVC
passes the minimal C/link probe and directly compiles the Task 256 scalar hosted
fragment plus the amalgamated freestanding COFF object. General whole-program
generated C still contains a required GNU statement expression, so MSVC is
rejected for that broader shape before any capability claim is made. With the
explicit workspace Zig 0.16.0 candidate, automatic selection falls back to Zig,
passes compile, SDK, runtime-link, native-run, LTO, and all four code-shape
capability probes, and preserves the `x86_64-windows-msvc` ABI. Without an
installed/configured Zig candidate the same general request reports unavailable;
the compiler core does not download a provider.

## Digest anchors

anchor-sha256: src/aot/xaot_link.c 350f8b20fef687d5d989c1926d9d98e234c15116d3de082761402165a3c36919
anchor-sha256: src/aot/xaot_callable.c 13d17addb6fe4492d78a2b010bc8329c2bc2bc912a9cd7e7cd8be59269c56d6a
anchor-sha256: src/aot/xaot_prepare.c 6344b5d731e0ae7b9047735ac862ccd468facf467fef6c82112e0b2c12310557
anchor-sha256: src/aot/xaot_prepare.h 3ffab2bce95306292132ed27fd9191670f6ca6a0d4ef1c25f08aca4e74fe6d10
anchor-sha256: src/aot/xaot_bundle.c 24e71e047ae95638aa9a597c2746462a45beafe64ee6da20f555a52c7a720420
anchor-sha256: src/aot/xaot_verify.c ab5e9bb717d8197a6bc665cb30aa77f448ee9ac045621fb9562890507b5fbfb5
anchor-sha256: src/aot/refine/xr_aot_representation_refinement.c 2a50faa7749f66e618da6153d7f3289db990efc518ad4343d3be9788c378871f
anchor-sha256: src/aot/refine/xr_aot_scalar_value.c 092c181dd8674dc8ca75f8328f4e457d1c20a1fb2aa713ad685c428838e49e2d
anchor-sha256: src/aot/refine/xr_aot_tail_call_conformance.h 4cbaa554291c41085a3e9b2d3372f21b9715630b9ecbb682de4392c0facc7739
anchor-sha256: src/aot/refine/xr_aot_tail_call_conformance.c 5d2a94e1664e8e6fd2951880562c1259795dc51d46c4c60032d0d6097c3181be
anchor-sha256: tests/unit/aot/test_xr_aot_refinement.c 451163c1d1e5d8af6aabe8ce0d0bab796a7daa7826f0a0143c6101e1dcda6164
anchor-sha256: src/aot/emit_c/xr_c_emission_schema.h 4510b64dbe8955b2b82d6419369d90bd2461286b3ae56f21c53dd294d5ba4081
anchor-sha256: src/aot/emit_c/xr_c_emission_plan.c 63e701ee3490d33c49408974d4539ecd6dbd6ba209147d622055666029523c1c
anchor-sha256: src/aot/xi_cgen_value_helpers.inc.c 2424b762735b2f2291a746571d296706b8567a8fcaee230a5b44c55ee8d6febc
anchor-sha256: src/aot/xr_target_aggregate_c_projection.h abfe201fb679c49334634af0db17ad23152fe896e7f665ae0464f4084505ca65
anchor-sha256: src/aot/xr_target_aggregate_c_projection.c 83e9445eb7b1fbf5004cbd198fd0c703839c6ce67f6d6cc34c4f8c78e4938e97
anchor-sha256: src/plan/semantic/xr_semantic_value_aggregate_shape.h 9ad3a46f3f41de669f91aa5a6e00452952ef4b0f23c2e9a92ec237a896c5b3b1
anchor-sha256: src/plan/semantic/xr_semantic_shared_read_shape.h c82c3ac533b4e4b0ef944e66b8a8b1ce1c1f2d96d3772438c7fc5ab9dc9ee0ce
anchor-sha256: src/plan/semantic/xr_semantic_string_shape.h 1ac7abc18731cb62b4ff0efd2d6ba29c623a002f95acd3c00e12baaee0dd3db9
anchor-sha256: src/plan/semantic/xr_semantic_string_runes_shape.h f5725458cdd6af16c555c1a8145aea90fb7f1b50cd599420590f2cfbb96980f2
anchor-sha256: src/plan/semantic/xr_semantic_string_slice_shape.h 2b0db2abc1652ec45f6a8090ad973cd60bafe039eb4f64c7d0e38674fd388dce
anchor-sha256: src/plan/semantic/xr_semantic_iterator_rune_has_next_shape.h 520152cb6e93b1cdd6639e094772a652905206e97bf15677ba753eecb4d075f6
anchor-sha256: src/plan/semantic/xr_semantic_iterator_rune_next_shape.h 4e4ac253f3837afde84345a2ea24a548f6c18378024ca9ac131ab3ad482433fd
anchor-sha256: src/plan/semantic/xr_semantic_rune_to_uint32_shape.h ceb823267ab7c4beb06b12f2946f449c763a6258e09457aa60aaa45a920ddbf7
anchor-sha256: src/plan/semantic/xr_semantic_rune_is_whitespace_shape.h 838d082dc4da2ea5bd55b799582e9ccfc1c959d8dc655d2753b80271889797a5
anchor-sha256: src/aot/xi_cgen_abi_helpers.inc.c 2f484fcb5e05b1b75adb008483f9902df089c164594f1321496c3f53c67ff5fd
anchor-sha256: src/aot/xi_cgen_class_native_helpers.inc.c 46ac29e4ba199a0568083b7a6e10953acc342350bde3f7fc7091a494bc76f70c
anchor-sha256: src/aot/xi_cgen_array_helpers.inc.c f46a22af26375384b861374d679e856b654425275d06e9d8318056b405092a0d
anchor-sha256: src/aot/xi_cgen_dispatch_helpers.inc.c e3881fee2d9dba7af5f66c401e5d0779139f7a1ca6fe11f56b9aecbda54f5a1d
anchor-sha256: src/aot/xi_cgen_program_entry.inc.c 4a875d43bbae8475a318d3d6153cf67aae484dcec86fb18c3d0e11562ff89e32
anchor-sha256: src/aot/xi_cgen_struct_helpers.inc.c 9ea6ac1e4e4afc494618bc7e50110f8c028ec3338aedce4503ffc80a5da40754
anchor-sha256: src/aot/xi_cgen.c ccd8b7ad6cbf261647bb3d103794702bdee3a1e6c1ba7707706bb5504399d858
anchor-sha256: src/ir/xi_opt.c dd0f3899748e06604b0b82f5b48eb3b1246d52b35ab8dd4536b6ea52d86a63ea
anchor-sha256: src/aot/xrt_coll.h 6c653d90dfdea6a5d2d03c64966c0594f9a262f902eecf7c5e96e31bb1efea16
anchor-sha256: src/aot/xrt_core_freestanding.h 89f686aeea86fd390110e3ecfe171bd33d96a12557867cb2e5a5730160888658
anchor-sha256: src/aot/xrt_method.h ec13b3e2ea5b3c71ccc09d495c2c7ff8f207116778f31db05b53008f5efd8e4b
anchor-sha256: src/aot/xrt_time.h 4d65fd48c6014eebffd2747b89c42652a1f1380a24cddbb07d0f1f79fa2c6aa7
anchor-sha256: include/xray_hosted_fragment_abi.h bcf50466f8320c265a49c6776f669912b83ac4ac3d04f397d4f6c527f1ead02c
anchor-sha256: src/app/cli/xcmd_build.c 8e9382e3a305668ffb7bf9591f4165b5e9c7b232f3180633f7bbc0004be0a5da
anchor-sha256: src/app/toolchain/xtc_model.c 91a6446ae4ffcda1178a979849c38c835b3092b4f8fdbffbf928c474a5ee1ac6
anchor-sha256: src/app/toolchain/xtc_probe.c 8d1cb7212b432a7cefe7e3e3d202509c75dd84190e084c3e7d2a88af62ca4eb1
anchor-sha256: src/ir/xi.h 64baa7c7b93cc9d01fa43fcbb3b96276f0d1f95aa514e63eed8942f7d0281679
anchor-sha256: stdlib/simd/simd.xr 0eb9b7955449743c09f7ba122cce51f8a772bb426413cde53c991b0ec664af24
anchor-sha256: src/aot/xaot_coro.h 51edaa56bb72326f5bacd0998b00d505e0c0533190f4ba0289c10ee954049995
anchor-sha256: src/aot/xi_cgen_class_helpers.inc.c 0488c328fc9d2eda313728e80c1ebe380363e08e367a588194d1061ae1bdfcec
anchor-sha256: src/aot/xrt_provider_abi.h 4deebceb145b02ba5c5836c8688b9c4788a130ab9fd0856d7ecc21dbcd5ce840
