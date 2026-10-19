# Task325 unfinished input inventory

This directory is part of the unified development handoff. It preserves work
that could not be overlaid onto the current compiler without replacing newer
interfaces, assertions or ownership checks. It is **not a second compiler**, a
compatibility implementation, or an automatically built source directory.

The active implementation is still `src/`, `include/`, `stdlib/` and `tests/`.
Presence here, or ancestry of a historical commit, does not establish that a
feature is connected, compiled, tested or qualified.

- `intake-manifest.json` maps every captured dirty source file to its fixed
  origin, hash and disposition. The `source_heads` list identifies the historical
  inputs whose commits are retained in the unified history, with explicit
  exceptions for rejected inputs.
- `dirty/<source_id>/<original_path>` contains exact unfinished input bytes.
  The local attributes preserve historical line endings and make these archived
  inputs binary diffs. They do not change attributes for active source or tests.
- `committed/P10/` and `committed/P12/` expose source seams that are absent from
  the active tree. Their full original commits retain the surrounding context.
- `P13/` retains overlapping spread fixtures. Active fixture assertions were
  kept; these candidate variants still need to be combined and registered.
- `P20/` retains historical lifecycle oracle expansions which conflict with
  current owned Program packets and newer late-value assertions. The current
  active assertions were preserved.

Source identifiers are stable inventory identifiers, not task numbers. In
particular, `032` is the original P05 tree, `037` is P10 and `039` is P12.
Read the manifest before using any input. Do not copy a whole archived tree over
the active implementation. Reconcile a named feature against the current
protocol, port its existing implementation and independent tests, then document
the exact disposition of its archived files.

Retired standard-library VM fastpath inputs are historical references only.
They do not authorize restoring the deleted automatic native cache. Native AOT
and bidirectional VM/native execution remain separate supported design goals.

The platform-rejected C08 implementation, the sealed P02 pointer-corruption
extension, and sealed experimental control repositories were not imported here.
Their original preservation locations and exclusions remain in the external
handoff inventory. Do not retry or route around those restrictions.

The detailed Chinese handoff and follow-on work order are in the companion
`xray-docs/analysis/task325-unified-code-handoff-2026-10-19-cn.md` document.
