# Legacy consumer preparation

This directory preserves migration obligations and independent expectations before a
published product baseline is available. It does not run Xray, claim a capability,
change a product interface, or authorize deletion of an old test.

- `responsibilities.json` maps the 27 historical capability families, eight retained
  goals, 14 qualification families and 18 root-effect requirements to their owning
  tasks and packages. Historical names come from a fixed Git object; retired
  contracts governance, layer states and exclusions are not reinstated.
- `consumer_routes.json` assigns an explicit primary family and producer packages
  to all 108 original Source responsibilities. It preserves 26 other files and
  all 160 indexed function bodies, including seven helper files with no counted
  test functions. Native harness success exits stay separate from language results;
  all replacement qualification and new-entry mappings remain OPEN.
  The class cleanup target fixture uses ordinary methods and contextual init,
  retains both named tests and adds during-call guard checks. It has not compiled
  on a product producer; its real test discovery/execution remains required.
- `runner_routes.json` binds 1647 explicitly reviewed FFI/JSON/generic/nullable/cleanup/
  generator/numeric/closure/module/call/ownership/control/comptime and regression inputs to their original source and expectation hashes,
  compiler positions/codes/text and actual runner entries. Future files are not
  automatically assigned by a directory wildcard. Missing expectations and
  diagnostic position gaps remain explicit. Twenty-two reviewed imported inputs bind
  their twenty-four complete XR/XRD companion links; an earlier import-authority failure
  cannot satisfy the requested callback effect, method or parameter-mode error.
  Existing Task318 Q12 names the
  frozen comptime/protocol scope without changing the 27 historical families.
  Five old no-call rejection cases are explicitly marked for required positive
  Task320 migration; the original rejection records remain historical evidence.
  Three read-class cases require ordinary method/init source ports. Six old
  generator prohibitions become positive resumable/wait/cleanup obligations.
  The incomplete enum implementation still rejects for missing its interface
  method, paired with a separate implemented witness. Read structs/generic
  receiver obligations and the unresolved cycle strategy stay distinct.
  Reviewed syntax/stdlib inputs preserve removed spellings, typed public options
  and constrained parallel boundaries. Old C-union syntax needs an extern union
  port before it can prove the field/method/generic restriction.
  All 891 original rejection inputs now have reviewed primary responsibility;
  one missing oracle and all original diagnostic gaps remain unresolved.
  Eighteen new type-source ports separate value copying/class identity, exact
  same-family numeric union members and the allowed local Tuple view wrapper.
  Readonly Slice writes and ordinary collection view storage remain rejected.
  Prior class ports explicitly type non-field init parameter v as i64.
  Seventy reviewed memory/OOP/advanced regression files retain their original
  positive entry; with the previously reviewed Atomic file, 69 files bind 387
  textual test marker segments. Those numbers
  are declaration obligations, never a runtime discovery/execution count.
  Four target sources preserve the complete selected files; three map every
  original named identity, including two record-copy test renames. An additional reverse program covers only three
  of thirteen original self-return tests and cannot delete the other ten or
  restore StringBuilder. Its partial status is enforced by negative controls.
  The coroutine review adds 114 unique programs and 180 entry links; 66 have
  both regression and differential entries. The reviewed set now has 1498 entry
  links, distinct from 1432 unique inputs. All 325 reviewed differential inputs
  bind parsed arguments, exact stdin bytes and present/missing sidecar identities.
  All currently reviewed argument/stdin streams are empty; that is checked, not
  assumed for future files. With coroutine tests, 142 files preserve 487 textual
  declarations, still without claiming actual runtime discovery.
  Ten additional complete target sources preserve sticky Array results, five
  Channel send responsibilities, select payload isolation, 64 close waiters,
  all 4 x 5000 Map sends and four ordinary class synchronization compositions.
  The mutable-class Task result port instead requires the specific Sendable
  rejection after contextual init; it is not a positive capability. The old
  repeated-await rejection is retained as evidence and marked for positive
  sticky-result migration. Closed and Timeout cannot share a success sentinel,
  and cancellation completion must follow cleanup exactly once.
  The collection/view/order review adds 107 differential inputs, giving 1605
  reviewed entry links and 432 bound differential input contexts. Fifteen complete
  selected source ports retain strict negative reserve/resize/fill rejection with
  unchanged receivers, explicit MutSlice reborrows, unique view-return signatures,
  all original sequencing/range/type observations and spread value-copy isolation.
  The imported view-return target binds its complete transformed helper as well
  as the historical original. All 51 source ports are still unexecuted.
  Frozen Slice/MutSlice values have no member methods. Three byte/endian oracles
  preserve semantic data only, without an executable replacement source; type
  renaming cannot restore old load/store/window/compare methods. Unknown library
  entries and unsafe overlapping loans remain unresolved producer obligations.
  The type-positive review adds all 108 selected regression inputs, with 89
  test files/429 new textual declarations and nineteen original run entries.
  Totals are 1713 entry links and 231 files/916 textual declarations. Thirty
  selected complete source drafts preserve all named identities and original
  program chains, including seventeen READ Array return/store/capture programs.
  Retained values stay independent after source writes and caller growth.
  Other drafts declare equality at the function or specific method, retire
  move-Array, require explicit mutable fixed-array views and actual ref writeback.
  The JSON extraction draft mutates a named logical Map and explicitly rebinds
  the original JSON.Value. Type-query, generic proof, reflection/opaque TypeId
  and protocol gaps still require published owners. There are 81 unexecuted
  source drafts, plus the earlier partial reverse program. A loopback oracle
  rejects native-type qualification from an early successful return.
- `oracles.json` records 138 independent output/result/rejection expectations.
  Preserved original literals remain byte-for-byte inputs. Target source ports
  are separately named and hashed; they do not overwrite historical originals.
  New Atomic fixtures separately require legal direct-var rebinding and
  explicit child parameter transfer, and reject worker reads of the concrete
  module slot. They are not product-compiled or wired to a new entry adapter.
  The StringBuilder input still requires
  a reviewed semantic migration to its replacement; it is not an API to restore.
- `audit.py snapshot` calls the existing runners' actual discovery and expectation
  parsers and checks the entire existing source-consumer inventory. It preserves
  missing expectations, absent historical anchors and unmapped case responsibilities
  explicitly. Counts overlap and are never a completion percentage.
- `compare_required` checks a single observation against the independent expected
  fields. It is preparation infrastructure, not a product execution driver. The
  checks recurse through nested arrays/records, keeping numeric and boolean types
  distinct. The tests include identical-but-wrong backends, absent versus empty output, exact
  cleanup order, missing requirements and a runtime failure posing as a compile
  rejection. Printed cleanup does not prove physical resource reclamation.

The standalone CMake project uses Ninja and `LANGUAGES NONE`; it validates preparation
artifacts only. It does not compile a product binary or replace production CTest
registration, budgets, sanitizer lanes, or P20's mixed-execution tests.

Run from the owning code worktree:

```powershell
cmake -S tests/unit/xir/legacy_consumers -B build-p19-preparation -G Ninja
ctest --test-dir build-p19-preparation --output-on-failure -j 2
python -X utf8 -B tests/unit/xir/legacy_consumers/audit.py snapshot --output .tmp_p19/inventory.json
```

The inventory distinguishes the legacy per-file `run_regression_tests.py` entry
selection from the registered `regression_corpus` gate. The latter uses directory
level `xray test` and the exact named-test ratchet in `xir_passing.txt`; its runtime
test discovery has not run here. The default backend-diff net invokes `run`, so a
matching source file does not establish that regression `@test` bodies executed.

After fixed dependencies arrive, review their documented interfaces and accepted
ancestor SHAs, re-inventory changed source inputs without erasing the seed evidence,
complete case-level mappings and independent missing oracles, and connect the real
consumer driver and production registration. Fresh VM and real generated-C native
observations must each satisfy the oracles, with separate full applicable ownership,
FI, resource, cancellation, physical cleanup and safety evidence. Until then every
product result remains `NOT_RUN` and every capability remains `OPEN`.


Reviewed execution duties preserve the complete original large-positive inputs,
optimized loop exits/latches, default and optional-call effects, class identities,
READ aggregate values, UTF-8 and exact output publication. Selected complete source
ports remain uncompiled preparation. Absolute observations reject identical wrong
backends, reduced input dimensions, partial print publication, missing receiver
retention and value aliases. Empty call results are Unit rather than null; the old
reflection assertions, raw-pointer clobber case and target-dependent union byte
layout require actual published replacement declarations before execution. Historical
implementation comments in copied fixtures are context, not current product claims.


Explicit review gaps are counted from the actual runner input/entry sets. Historical
anchor matching remains evidence about a family, never a substitute for a complete
case record. Selected process arguments and original binary stdin bytes are bound
separately; CRLF bytes are not normalized away. Provider effect duties distinguish
real symlink/socket/thread/descriptor/wait/recycle activity from fake true branches,
markers, counts or clean output. Full parallel, SIMD and OS-thread dimensions stay
required; published replacements and all applicable safety gates are still OPEN.


The reviewed differential set is now explicit in full. Library duties preserve
complete byte/property corpora, typed errors, nested payloads, HTTP UTF-8 framing,
protocol vectors, lexical path semantics and actual provider boundaries. Selected
full drafts follow declared non-Optional UTF-8 results, required CSV rune payloads
and typed catch bindings while retaining original sources. Known hash/CRC/HMAC and
wire answers are independently calculated; output equality never grants entropy,
constant-time, authentication, connection or physical cleanup qualification.

Reviewed standard-library regression records preserve every named source segment
and its whole direct/transitive imported module closure. Full selected drafts use
contextual class init, logical Array/channel/return copies, actual timer elapsed
bounds and owned string chunks joined through the declared API. Generic definition
proofs, enum metadata and native SIMD profiles remain explicit pending duties.
Every oracle demands the actual test entry and all named bodies; a run-only banner
or reduced property/warmup/lifetime dimensions cannot qualify the replacement.

Format/crypto/calendar review preserves whole typed corpora and strengthens weak
shape-only cases with exact content, retained original pressure plus actual>64KB
payloads, all padding boundaries and independent complete AES/HMAC wire vectors.
YAML block-scalar independent expectations follow1.2.2 chomping, including absent
physical final LF; the seed source discrepancy remains a producer qualification
gap, not an observed product failure. Exact values do not establish entropy,
constant-time, real timezone/IO/provider provenance or physical resource release.

JSON/system/runtime duties preserve complete primitive/Unicode/large numeric
corpora, raw invalid UTF8 bytes, actual parent-temp and environment cleanup,
full memory pressure and ordered buffered log delivery. Source-location markers
and historical collector/JIT comments never qualify real callsites or current
strategy. Whole directory refusal conflicts with the seed best-effort removal
contract; Windows literal-backslash filesystem and JSON query/Equal/range/error
replacements remain explicit unresolved duties, not skipped execution or fake
platform success. All actual backend/provider/FI/physical gates stay OPEN.

Every original runner input and entry is now explicitly routed. Check/snapshot
gates reject an omitted input even when historical anchors still match it.
Every483 named regression input now has its complete textual declaration
manifest; actual runtime discovery and qualification remain NOT_RUN. Network
drafts preserve all real setup, byte/framing/timeout/half-close/child lifetime
duties; listener or TLS skips never qualify provider capability. RFC HPACK and
BE control-frame answers are independently computed, while opaque Buffer/view
transport and actual project context remain unpublished replacement duties.

Core literal/variable/operator review binds all485 newly reviewed named bodies
and per-test independent observations. Narrow wrapping, full MIN division/shift/
NaN loops, unsigned digits, actual radix spellings and outer Some(inner None)
remain exact. Decimal-only float separator labels grant no lexical coverage;
unpublished canonical queries and nested Optional/reverse replacement duties
remain abstract. No prepared source or comparison qualifies product execution.

Control/function review preserves all276 newly bound named bodies and values,
including labeled targets/latches,100000-depth inputs, independent capture
cells, full returned payloads and exact default stdout. Panic and enum errors
stay separate; direct/indirect callees observe held state before cleanup.
Owned callback captures register their cycle break before allocation-sensitive
assignments; actual FI/physical/frame/TCO and canonical Iterator/JSON proof
remain unqualified. Function values never inherit item hoisting or defaults.

Collection/string review retains all360 new named duties, exactnestedpayloads,
logicalget/copy/writeback, strictfill failure atomicity andexclusiveview/NLL
semantics. OwnedUTF8accumulation replaces retiredbuilder calls; allUnicode
scalar/octet/malformed-input and500-element/workerdimensions are independent.
Historical64-byte/buffer/pool/JIT labels grant no implementation provenance,
and claimed200-job pressure is recorded separately from original100-job body.
ActualcanonicalJSON/Key/Compare/callback/formatter replacement remains OPEN.

Error/byte/narrowing review binds the final27 files and108 per-test duties.
Readonly Slice and exclusive MutSlice, full64+4096 growth, owned Array capture,
module/reexport declaration identity and loop zero-entry/backedge facts remain
exact. Frozen Optional decisions preserve a nullable last field as two layers;
the older baseline spec flattening sentence is a pending synchronization duty.
The complete13-test retained-result draft also prepares the10 duties absent
from the supplementary reverse program, using owned UTF-8 text and logical
Array result independence. Sort/Compare, capacity, Set and multi-point optional
chain/query/imported-definition replacements remain abstract. Full named
preparation never proves actual execution, physical release or product readiness.
