# Blocker: two parsers read `baseline_failures.txt` differently, and the disagreement is invisible

- **Lane**: 8 (test discipline wiring), round 3
- **Status**: `OPEN` — deliberately not fixed in this branch
- **Owner of the file that would have to change**: whoever next touches
  `scripts/t.py`. It is outside lane 8's allowed file set, and the integrator
  ruled the convergence should ride along with the next change that opens that
  file rather than be forced now.
- **Severity**: latent. It causes no damage today because nobody has ever used
  the syntax the two parsers disagree about. The first person who does will get
  a whole tier reported as red with no explanation.

## What happens

`tests/regression/baseline_failures.txt` is read by two independent parsers,
and they treat a trailing `#` comment on an entry line differently:

- `tests/lib/xraytest/ratchet.py:36`

  ```python
  line = raw.split("#", 1)[0].strip()
  ```

  Strips a trailing comment. `0380_optional_chain.xr  # arrears` parses as the
  entry `0380_optional_chain.xr`.

- `scripts/t.py:286-288`

  ```python
  expected = {line.strip()
              for line in REGRESSION_BASELINE.read_text(encoding="utf-8").splitlines()
              if line.strip() and not line.startswith("#")}
  ```

  Does not. The same line parses as the entry
  `0380_optional_chain.xr  # arrears`, which matches no case name.

Whole-line comments are handled identically by both, so the file works today.

## Why it will bite

The baseline's failure names come from the runner's JSON report, which contains
bare case file names. Under `t.py`'s parser, every entry carrying a trailing
comment silently drops out of the `expected` set. `t.py:293` then computes

```python
newly_broken = sorted(actual - expected)
```

and every commented entry reappears as a *new* failure. Adding trailing
comments to all 45 entries would make `t1`, `t2` and `t3` report 45 fresh
regressions on a tree where nothing changed.

The failure mode is the bad kind: it is not a crash, not a parse error, and not
a message. It is a tier going red with a list of case names that look exactly
like real regressions.

## How it was found

Round 3 integration asked lane 8 to mark the fourteen entries the corpus gate
exposed on installation with a structured tag, so a later reader could tell
arrears from regressions mechanically. The obvious encoding was a trailing
comment:

```
0380_optional_chain.xr  # exposed-on-gate-install 2026-08-28
```

Reading both consumers before writing it is what surfaced the disagreement. The
entries were grouped under whole-line section headings instead, which both
parsers read correctly:

```
# --- exposed-on-gate-install 2026-08-28 (14) --------------------------------
0380_optional_chain.xr
...
```

So the immediate need is met and nothing is broken. What remains is the trap.

## Recommended fix

Have `t.py` use the shared parser instead of its own:

```python
from xraytest import ratchet
expected = ratchet.read_baseline(REGRESSION_BASELINE)
```

`tests/lib/xraytest/ratchet.py` already exists precisely for this, and its
docstring says so: *"Three files today (baseline_failures.txt,
known_failures.txt, filetests_known_failures.txt) encode the same policy in
three shell reimplementations."* `t.py:293-296` is a fourth reimplementation of
`evaluate()` alongside the parser — replacing both with the shared module
removes the divergence and the duplicate policy in one edit.

`scripts/check_regression_corpus.py`, added in this branch, already consumes
`ratchet.read_baseline` and `ratchet.evaluate`, so after the change all
consumers of this file agree by construction.

## Verification once fixed

```bash
# Both parsers must see the same 45 entries.
python3 -c "
import sys; sys.path.insert(0, 'tests/lib')
from xraytest import ratchet
from pathlib import Path
print(len(ratchet.read_baseline(Path('tests/regression/baseline_failures.txt'))))"

# And a trailing comment must then be harmless:
#   append '  # probe' to one entry, re-run scripts/t.py t1, expect no change.
```
