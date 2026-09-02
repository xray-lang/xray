# Record enum syntax baseline

- Source revision: `61d5cab8ef512f5fb18536b140ec7f08e8f6d3b8`
- Source tree: `0f5b4f634bfefb7ad3e8e1494ce2995ae52df976`
- Product identity: `xray-lang 0.9.2`, clean Release build, macOS arm64
- Inventory: 3,586 Xray files, 249 enum declarations, 197 payload variants,
  240 payload fields, and 740 reviewed old qualified payload uses
- Migration split: 327 constructors and 413 patterns
- Positional surface: 136 positional or mixed declarations with 165 unnamed fields

The focused baseline ran parser, formatter, analyzer, monomorphization, Xi,
enum metadata, dense enum runtime, LSP navigation, and canonical VM/AOT
contract tests. Fourteen of sixteen tests passed. The following two failures
were reproduced serially before any enum-source change:

1. `test_analyzer`: the gzip builtin contract is absent
   (`gunzip_contract != NULL`).
2. `test_xi_cgen`: the `SnapshotValue` enum fixture has an incomplete
   TargetPlan representation binding for `GET_SHARED`.

These are baseline defects outside the record-enum source cut. They remain
visible and are not accepted as regressions in a changed test.

The inventory is reproduced with:

```sh
python3 scripts/inventory_enum_migration.py \
  --root . \
  --revision 61d5cab8ef512f5fb18536b140ec7f08e8f6d3b8 \
  --self-test \
  --manifest contracts/canonical-program/enum-record-syntax/positional-field-renaming-manifest.tsv \
  --provenance analysis/285-enum-use-provenance.tsv \
  --json
```
