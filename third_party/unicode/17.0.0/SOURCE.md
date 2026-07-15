# Unicode 17.0.0 grapheme data

This directory vendors the normative inputs used by Xray's default extended
grapheme cluster implementation. Builds and generators are offline-only.

- Unicode version: 17.0.0
- UAX #29 revision: 47
- Conformance profile: UAX29-C1-1
- Tailoring: none
- Download date: 2026-07-15

Versioned sources:

- `GraphemeBreakProperty.txt`: https://www.unicode.org/Public/17.0.0/ucd/auxiliary/GraphemeBreakProperty.txt
- `GraphemeBreakTest.txt`: https://www.unicode.org/Public/17.0.0/ucd/auxiliary/GraphemeBreakTest.txt
- `DerivedCoreProperties.txt`: https://www.unicode.org/Public/17.0.0/ucd/DerivedCoreProperties.txt
- `emoji-data.txt`: https://www.unicode.org/Public/17.0.0/ucd/emoji/emoji-data.txt
- `LICENSE.txt`: https://www.unicode.org/license.txt
- UAX #29: https://www.unicode.org/reports/tr29/tr29-47.html

The checked-in `SHA256SUMS` file is verified before generation. Refreshing any
input requires an explicit Unicode-version update and regenerated tables,
tests, size measurements, and performance measurements in the same change.

Generation command (after P1 lands):

```sh
python3 scripts/gen_unicode_grapheme_tables.py \
  --unicode-dir third_party/unicode/17.0.0
```

Verification command:

```sh
python3 scripts/check_unicode_generated.py
```
