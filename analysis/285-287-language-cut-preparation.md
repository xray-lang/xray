# Tasks 285 and 287 language-cut preparation

## Source identity

| fact | value |
| --- | --- |
| branch | `prep/285-287-language-cuts` |
| frozen inventory snapshot | `49f71d1ea0f8e12aa2af8e64fb5d8d1c6f3ee79c` |
| frozen inventory tree | `478e72a21e54ae5cc182caaa77cf9e526725a0a6` |
| delivery-base checkpoint | `eabe56cc986cf2147d02254ae1b1acaa1418663f` |
| delivery-base tree | `45f708658f0945bf3f588be332859d3ecde58782` |
| local build | worktree-local `build/`, Ninja, Release, AppleClang, shared ccache only |
| activation state | preparation only; no shared lock granted |

This record is a source-backed preparation inventory. It is not a language
implementation, schema migration, completion claim, or provider qualification.

The snapshot is the coordinator-frozen census identity; the delivery-base
checkpoint is the parent of this preparation commit. There are exactly 82
intervening paths. The source-aware validator computes 4,989 snapshot and 4,990
delivery governed inputs spanning all scanned Xray source,
frontend/runtime/AOT C owners, owner-map anchors, and lane-relevant contracts.
Thirty-two changed paths intersect that union: eleven reviewed contract inputs
and twenty-one C or Xray source inputs. The integrated Task 278 regex/AOT
ownership cut accounts for the new source delta, including the deletion of
`src/aot/xrt_regex.h`; this lane does not claim or reimplement that authority.
The validator binds both sides by content and requires the exact full,
contract, and governed deltas. Task 287 language residue and owner/symbol
anchors remain identical.

The enum declaration, payload, and use totals also remain unchanged, including
327 constructors and 412 patterns. Two new Task 278 regression `.xr` files
raise the tracked file count from 3,554 to 3,556. Edits within
`stdlib/regex/regex.xr` move the source positions of exactly three reviewed
`RegexError.InvalidPattern` uses, but their semantic identity multiset remains
unchanged. The comparable snapshot and delivery enum-report digests (with the
separately frozen revision and tree identities removed) are respectively
`d7a978a782105dceef0d97464607c9eebf4c58e0eff7d3cebf34fcfeec755548`
and `d05e2f0ab80784632f2a8738a9da97f86ee2509df68f2ac21b0d90110d87934e`.
The snapshot and delivery governed digests are respectively
`bcfd1cac3651c8924308b926623d44c3e02d782b10d52d03cf69ec6d114b3386`
and `cc4dd357bf1a6f7d53d058954d2496a152c6f6afc7b4ae71b22dd0d8c932be74`;
their lane-relevant contract-input digests are respectively
`9881bcd7b9af3e215fe39484a160fb2de16058d5d0a4f55914f82337699cf234`
and `776232724c6054694706e1e150fa57b57b70d45b7c613b1e6df3632f980c467b`.

## Matching-base build boundary

`cmake --preset default` configured the worktree-local `build/` with Ninja,
Release, AppleClang 21, tests enabled, and `/opt/homebrew/bin/ccache`. The
source tree remained clean after generation. After synchronization to the Task
278 checkpoint, `cmake --build build --target xray -j1` reconfigured for the
deleted regex runtime header and rebuilt the worktree-local target. A repeated
serial run stopped while generating hosted VM standard-library fastpaths at
this deterministic earlier failure:

```text
stdlib/http/http.xr at backend:
func '_serverListen': v21 has non-backend op SCOPE_ENTER(184) in b5
(must be lowered before STAGE_BACKEND)
```

The preparation commit changes only analysis and inventory scripts, so its
compiler/build inputs are identical to the delivery checkpoint. The
authoritative `eabe56cc986cf2147d02254ae1b1acaa1418663f` FASTPATHS=ON first-red
is the `http.xr::_serverListen` `SCOPE_ENTER(184)` failure above. Its parent
checkpoint `814e5df81` instead reached `sync.xr` and stopped at `XR_SEM_0019`;
after the current metadata drift is repaired, that failure is the expected
next-red, not a current-checkpoint result. Neither checkpoint is I1 clean. The
serial reproduction distinguishes the current failure from a parallel timeout.
No FASTPATHS=OFF build, skipped target, foreign worktree binary, or stale test
result is used as substitute evidence. A matching `build/xray --version --json`
identity is therefore unavailable until the upstream default build reaches the
executable.

## Owned durable fact

This lane owns the preparation evidence for:

- naming every positional enum payload field before the record-style enum cut;
- locating enum declarations, constructions, patterns, AST consumers, codemod
  candidates, formatter/LSP/spec projections, and legacy enum parser paths;
- locating the old `where`, comma-less `match`, computed-property, and
  interface-property surfaces;
- staging the hard cuts in the dependency order `284 -> 285 -> 287`, with the
  Task 287 cuts ordered `where -> match comma -> computed/interface property`.

Task 284 remains an upstream public-contract checkpoint owned by its assigned
lane. This lane consumes the released checkpoint; it does not invent a second
receiver, borrowed-return, ownership-domain, or view-validity authority.

## Allowed preparation layers

Before dependency release and explicit lock assignment, writes are limited to
lane-owned analysis and migration evidence. Read-only inspection may cover the
entire repository. Independent fixtures, negative oracles, codemod prototypes,
or consumer patches may be proposed, but they must not activate public grammar,
change a protected schema, register a new root build target, regenerate a shared
projection, or alter the completion baseline without the owning lock.

## Shared single-writer facts

| lock | protected fact | current disposition |
| --- | --- | --- |
| `LOCK-FRONTEND` | parser, public AST, function/receiver type, enum and interface grammar | read-only inventory until granted |
| `LOCK-SCHEMA` | PSC, SemanticPlan, TargetPlan, XTP rows/codecs/versions/builders/verifiers and governed contract anchors | no schema or anchor writes until granted |
| `LOCK-RUNTIME` | lane-relevant public runtime API and accessor runtime ownership; artifact-identity implications return to the train | no runtime API change requested during preparation |
| `LOCK-BUILD-GEN` | root CMake/test registration and generated manifests/tables/includes/spec/LSP/MCP projections | no registration or regeneration commit until granted |
| `LOCK-RESIDUE` | completion baseline and governed legacy classification | report candidates only until granted |

The main conflict hotspots are `src/frontend/parser/xparse_oop.c`,
`src/frontend/parser/xparse_match.c`, common AST definitions and walkers,
`src/frontend/analyzer/xanalyzer_visitor_expr.c`,
`src/ir/xi_lower_expr.c`, formatter declaration/expression emitters, public
specification sources and their generated projections, root build registration,
and completion/residue inventories. These files cannot receive competing
implementations from parallel lanes.

## Facts explicitly not owned

- no independent PSC, SemanticPlan, TargetPlan, XTP, cache, planner, executor,
  verifier, artifact version, or compatibility reader;
- no completion-baseline reclassification or terminal-zero claim;
- no runtime public API or accessor-runtime cut before assignment;
- no change to the retained `..=` or `?[` semantics except their later focused
  conformance evidence;
- no regex-literal work and no `new`-surface work;
- no task-status edits in the documentation repository and no changes to other
  Tasks 269-292 lanes;
- no alias, shim, legacy schema reader, dual parse, dual write/read, fallback
  selector, migration flag, or transition facade.

## Activation boundary and planned atomic cuts

Preparation ends only when the enum naming manifest, AST/tooling/codemod map,
legacy-residue census, affected-contract inventory, and focused positive and
negative oracle plan are reproducible from the exact source identity above.

After the integration train releases dependencies and grants every applicable
single-writer lock, the public work proceeds as separate atomic cuts:

1. consume the released Task 284 source-contract checkpoint;
2. replace positional/call-shaped enums with one named-record declaration,
   dedicated construction AST, selective named-field pattern, and canonical
   enum schema; migrate consumers and delete every covered old owner;
3. delete `where` while making it an ordinary identifier;
4. require commas between adjacent `match` arms and delete token/newline arm
   boundary heuristics;
5. migrate computed and interface properties to explicit methods, then delete
   accessor AST/analyzer/runtime/backend/tooling owners without deleting
   ordinary stored-field behavior.

Each public cut must establish its new owner, migrate every covered consumer,
provide positive and constructed negative evidence, and delete the covered old
owner in the same delivery slice. No public cut begins from this preparation
record alone.

## Enum migration census

The read-only scanner is `scripts/inventory_enum_migration.py`. It reads an
exact Git revision through `git archive`. Its lexical state covers nested block
comments, inline and block quoted forms, raw/byte/C-byte/rune literals, and
quoted interpolation; malformed tracked negative fixtures are recorded rather
than silently dropped. It enumerates every `Receiver.Variant(...)` whose member
spelling occurs in a payload enum, then validates every candidate against
`analysis/285-enum-use-provenance.tsv` by tree, path, Unicode code-point span,
line, role, source-slice SHA-256, reviewed disposition, and exact declaration
path/line. These offsets are inventory identities only; byte-oriented compiler
or codemod consumers must not use them.

The lightweight import resolver only proposes rows for review. It is not a
compiler resolver or a compatibility path. The committed 753-row frontier is
the preparation oracle: 739 reviewed enum uses and 14 reviewed rejections.
`--emit-provenance` marks every proposal `unreviewed`; the committed reviewed
table is separately frozen by SHA-256 and cannot be reproduced byte-for-byte
from the heuristic emitter.
Those rejections cover eleven same-spelling `TomlValue.Float/String` class
static calls, two nonexistent `TaskResult.Ok` patterns in negative fixtures,
and one namespace-imported `typed_lib.Point` class constructor. Constructed
self-tests additionally cover import aliases, unaliased names, local shadows,
and cross-file enum/class spelling collisions. During the public cut, the
semantic analyzer's enum-member selection must confirm these identities; no
runtime, parser, or codemod path may consult this TSV.

The exact reproduction command validates both the naming decisions and the
reviewed use frontier:

```bash
python3 scripts/inventory_enum_migration.py \
  --revision 49f71d1ea0f8e12aa2af8e64fb5d8d1c6f3ee79c \
  --manifest analysis/285-enum-field-renaming-manifest.tsv \
  --provenance analysis/285-enum-use-provenance.tsv \
  --self-test
```

Frozen-base result:

| fact | count |
| --- | ---: |
| tracked `.xr` files under bench/demos/stdlib/tests | 3,554 |
| enum declarations | 247 |
| payload variants | 196 |
| payload fields | 239 |
| fully named parenthesized variants | 61 |
| positional or mixed variants | 135 |
| unnamed payload fields | 164 |
| old call-shaped constructions | 327 |
| old positional match/catch patterns | 412 |
| reviewed syntactic frontier | 753 |
| explicit non-enum rejections | 14 |

The 412 patterns consist of 404 direct match arms, two guarded match arms, and
six catch patterns. The catch cases are not constructors: the catch parser
calls `xr_parse_match_pattern()`, and the old call-shaped form becomes
`AST_PATTERN_ADT`. This resolves an initial six-site surface-classifier
disagreement without changing the common total of 739 old uses. The accepted
use identity digest is
`75a38bf07e41c4a9e488caefef244ac648900028ee69a721df2c2573518abaf7`.

The field decisions are in `analysis/285-enum-field-renaming-manifest.tsv`.
It contains exactly 135 rows and never synthesizes numbered placeholders.
Every row is confirmed. The six metadata or deliberately positional test rows
use these stable lane-owner decisions:

- `BoundaryState.Busy.value`;
- `State.Busy.value`;
- `NoAllocState.Busy.value`;
- `Tree.Branch.value`;
- `PositionalPayload.Pair.left/right`;
- `TransferState.Busy.value`.

Manifest validation requires the exact row set and field arity, rejects empty
items, duplicates, surrounding whitespace, numbered placeholders, non-name
token sequences, `_`, all lexer keywords, and exact scalar spellings. Positive
and negative mutations are part of `--self-test`.

The regex boundary is not hidden in these totals. `stdlib/regex/regex.xr` has
one already named payload variant and two old constructions, while
`tests/regression/10_stdlib/1101_regex_engine.xr` has one old pattern. Those
three use sites belong to the regex lane and must be coordinated rather than
modified here.

## AST, tooling, and codemod inventory

The machine-readable owner map is
`analysis/285-287-owner-consumer-inventory.tsv`. Its central findings are:

- `AST_ENUM_CONSTRUCT` does not exist;
- `EnumMemberNode.payload_names` still permits positional `NULL` entries;
- `PatternAdtNode` has only positional patterns;
- construction remains a normal call with
  `allow_payload_enum_ctor_value` and call-analyzer special cases;
- match and catch patterns still rewrite `AST_CALL_EXPR` to
  `AST_PATTERN_ADT`;
- runtime layout, module codec, and semantic metadata still normalize or
  tolerate empty field names;
- PSC and SemanticPlan have no dedicated named `EnumVariantSchema`;
- the existing ADT enum TargetPlan call identities are valid downstream
  consumers and must not be deleted as residue;
- builtin enum projections cannot currently carry field names through the
  two-argument `XR_BUILTIN_ENUM_VARIANT` source row;
- generated native types, language docs, knowledge, MCP C, LSP stdlib, and API
  inventory are outputs or consumers, not hand-edit owners.

There is no safe general enum codemod today. `gen_api_inventory.py` has an ad
hoc parser and must not rewrite syntax. Enum migration therefore requires a
dedicated AST-aware rewrite driven by the reviewed manifest, followed by the
new parser/analyzer plan; the rewrite is a one-time tool and never a compiler
compatibility path.

The existing formatter is the correct migration vehicle for match commas.
The atomic comma cut should first teach an exact pre-cut formatter binary to
emit canonical arm commas from the old AST, apply that formatter to the whole
corpus, and then land the strict parser, formatter, migrated corpus, and old
heuristic deletion together. A regular expression must not insert commas.

## Old-surface residue

The full count table is
`analysis/285-287-old-surface-residue.tsv`. The recursive source-aware scanner
is `scripts/inventory_language_cut_residue.py`; it validates the table rather
than trusting copied totals:

```bash
python3 scripts/inventory_language_cut_residue.py \
  --revision 49f71d1ea0f8e12aa2af8e64fb5d8d1c6f3ee79c \
  --residue analysis/285-287-old-surface-residue.tsv \
  --self-test
```

Task 287 preparation establishes:

| surface | current residue |
| --- | --- |
| `where` | 9 trailing clauses in 4 test files; no stdlib or demo use |
| match comma | 382 comma-less multi-arm matches in 198 files, containing 521 missing separators |
| computed property | 35 accessor blocks in 8 files: 35 getters and 6 setters |
| interface property | 4 declarations in one test file |
| native accessor identity | 16 getter registrations, including 14 coroutine rows and Range `get:start`/`get:end` |

The property cut has production migrations: 12 DateTime getters,
`CookieJar.count`, and `Process.id`. Ordinary stored fields, generic dynamic
member capability, `xrt_field_get/set`, and GETPROP/SETPROP opcodes are not
accessor residue and cannot be deleted by name.

The recursive match walk finds 495 match expressions, including 452 multi-arm
expressions. It corrects the earlier shallow census by retaining five nested
comma-less matches. The property parser corrects twelve shallow false positives
where closure-valued stored fields were paired with later method bodies. The
lexer records 17 errors across intentionally malformed negative/fuzz fixtures;
their recovery is deterministic, and none overlaps an accepted enum use or
payload declaration.

The specification inventory found a live disagreement: the expression section
already states that match commas are mandatory, while the statement and EBNF
sections still publish optional-comma grammar. The public cut must repair the
bilingual structured sources and regenerate their projections; it must not
patch generated language documents directly.

## Positive, negative, and mutation oracles

### Named enum cut

- Positive source: unit, single/multiple named fields, generic/import/re-export,
  recursive indirect types, declaration-order and out-of-order construction,
  `{}` tag-only pattern, shorthand, rename, nested selective fields, and enum
  READ/ref/move/static methods after the Task 284 checkpoint.
- Evaluation: source-order initializer side effects, throw from each field
  position, and exactly-once cleanup with canonical declaration-slot output.
- Negative source: old declaration/construction/pattern forms, enum `fn`, empty
  declaration, missing/duplicate/unknown construction fields, duplicate/unknown
  pattern fields, unit braces, payload without braces, and enum pattern rest.
- Metadata mutations: NULL/empty/duplicate names, name/count/type disagreement,
  ordinal corruption, and stale source/schema identity must fail closed.
- Old negative syntax should be constructed by the test runner, not checked in
  as live source that would force a residue allowlist.

### `where` cut

- Cover inline constraints for function, class, struct, interface, and enum,
  including `A & B` and multiple type parameters.
- Prove that `where` lexes as an ordinary identifier after deletion.
- Construct a trailing old clause from a valid inline fixture and require one
  focused parser error with an inline-constraint fix-it.
- The residue check must recognize declaration-header clause shape; a word ban
  would incorrectly reject the newly legal identifier.

### Match comma cut

- Cover expression/block bodies, guards, type and enum patterns, nested match,
  same-line arms, lambda bodies, control flow, and comment trivia.
- From each canonical multi-arm fixture, delete every non-final separator one
  at a time. Every mutant must fail at the preceding arm with exactly
  `expected ',' or '}' after match arm`.
- Freeze pre/post canonical AST signatures plus VM/AOT output and side-effect
  traces to prove that the cut changes only source boundaries.
- Reuse the same balanced-delimiter algorithm for terminal residue; line-end
  comma matching is not sufficient.

### Computed/interface property cut

- Migrate query accessors to normal zero-argument methods and setters to Task
  284 `ref` methods; interface requirements become exact method contracts.
- Contrast a stored field with an explicit same-domain method: field access
  touches only a slot, and only `()` can run user code.
- Mutate class/artifact inputs with stale `get:`/`set:` identities and require
  rejection; no loader or VM fallback may revive accessor dispatch.
- VM/AOT differential and generated-C residue must cover direct/dynamic methods
  and stored class/struct fields while rejecting accessor bridges.

## Contract impact and readiness

`analysis/285-affected-contract-anchors.tsv` records the affected and
conditionally shared anchors. It is an impact inventory only; no contract
digest or anchor is authorized for change.

`scripts/validate_language_cut_preparation.py` validates the manifest,
provenance frontier, residue TSV, all 51 owner-consumer rows, and all 14
lane-relevant contract-impact rows against exact Git revisions. It verifies
that every owner path and named symbol exists, every contract path exists and
has a reviewed lock/status/evidence record, and both revision identities are
reviewed against the exact frozen-to-delivery delta:

```bash
python3 scripts/validate_language_cut_preparation.py \
  --snapshot 49f71d1ea0f8e12aa2af8e64fb5d8d1c6f3ee79c \
  --delivery eabe56cc986cf2147d02254ae1b1acaa1418663f \
  --manifest analysis/285-enum-field-renaming-manifest.tsv \
  --provenance analysis/285-enum-use-provenance.tsv \
  --residue analysis/285-287-old-surface-residue.tsv \
  --owner-map analysis/285-287-owner-consumer-inventory.tsv \
  --contracts analysis/285-affected-contract-anchors.tsv \
  --self-test
```

The preparation artifacts are complete, but the public language cuts are not
ready. The integration train has published the Task 278 regex/AOT
ownership-residue cut, but it has not released an I1-clean checkpoint. Current
checkpoint `eabe56cc` stops first at the `http.xr::_serverListen`
`SCOPE_ENTER(184)` failure; parent `814e5df81` stopped first at `sync.xr`
`XR_SEM_0019`, which is only the expected next-red after the current metadata
drift is repaired. The Task 284 public checkpoint is unavailable, and no
`LOCK-FRONTEND`, `LOCK-BUILD-GEN`, `LOCK-SCHEMA`, `LOCK-RUNTIME`, or
`LOCK-RESIDUE` assignment has been granted. The next implementation action is
therefore blocked on train release, not on an unresolved design choice inside
this lane.
