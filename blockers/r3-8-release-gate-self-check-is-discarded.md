# Blocker: the release gate's own "these exclusions can come back" check is discarded, and CI turns it off

- **Lane**: 8 (test discipline wiring), round 3
- **Status**: `OPEN` — recorded, not fixed. The fix is in
  `scripts/run_release_09_gate.py`, outside this lane's allowed file set, and
  the integrator ruled it out of scope for this round.
- **Severity**: a working self-check that reports to nobody. It is worse than
  an absent one, because code review sees it and moves on.

## What exists

`scripts/run_release_09_gate.py:97-109` already does the right thing. After the
main run it re-runs the *excluded* suites and says so when they pass:

```python
"Known boundary suites now pass; remove the 0.9 exclusion before release."
```

That is exactly the check a suppression list needs: not "is the entry still
justified in prose" but "does the thing it suppresses still fail".

## Why it reports to nobody

Two independent choices, each harmless on its own:

1. **Its return value is discarded.** The re-run's result is not propagated;
   `main` returns 0 either way. The sentence is printed into a log, and nothing
   acts on it.
2. **CI passes `--skip-boundary-report`.** `.github/workflows/ci.yml`'s
   unit-tests job invokes
   `run_release_09_gate.py --no-build --skip-boundary-report`, so the re-run
   does not happen at all in the one place it would be read.

Either alone would be a weak check. Together they make it a no-op that looks
present in the source.

## Why it matters more now

`tests/ci_exclusions.txt`, added in this branch, gives every excluded lane a
reason, an owner, and a 30-day expiry. That closes the "nobody remembers why"
half of the problem. It does not close this half: **between renewals, a lane
that has been fixed can stay excluded for up to 29 days**, because the gate
checks the paperwork, not the subject.

The measurement that motivated the manifest shows the size of that window in
practice. Of the twenty-one exclusions, seven were passing when finally
measured, and one of them (`http_static_route_full_parse`) had its stated cause
fixed three days after it was excluded — and stayed excluded for another 39.
Nothing was watching. This check was the thing that should have been.

## Fix

Two lines of intent, both in `scripts/run_release_09_gate.py`:

- Propagate the boundary re-run's result, so a passing excluded suite fails the
  gate with the message it already prints.
- Drop `--skip-boundary-report` from `ci.yml`'s unit-tests job, or invert the
  flag's default so skipping is the exception.

Then the manifest and the re-run cover each other: the manifest catches
exclusions nobody can justify, the re-run catches exclusions nobody needs.
