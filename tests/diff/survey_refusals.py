#!/usr/bin/env python3
"""Rank differential-net targets by what they would actually finish.

`run_backend_diff.py` reports the first refusal a case hits, so counting first
refusals answers "how many cases does this construct head" -- not "how many
would it finish".  The two differ sharply: a construct heading nine cases
finished zero of them when every case turned out to have a different second
construct waiting behind it.

This walks every not-comparable case with XRAY_COLLECT_ALL_REFUSALS=1, which
enumerates all refusals the current layer can see rather than stopping at the
first, and groups cases by their complete refusal set.  A construct's yield is
the number of cases it is the *sole* remaining refusal for.

That count is still an upper bound, not a promise: the layers run in sequence,
so a case clearing this layer may be refused by the next one.  It is the
sharpest estimate available before doing the work.

Usage:
    python3 tests/diff/survey_refusals.py [--xray PATH] [--jobs N] [--json OUT]
"""
import argparse, collections, concurrent.futures, json, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LIST = os.path.join(ROOT, "tests/diff/known_failures_not_comparable.txt")
SURVEY = re.compile(r"\[refusal-survey\] family=(\S+)(.*)")
ERRLINE = re.compile(r"^Error: .*?: (XR_[A-Z0-9_]+)")


def refusal_key(family, rest):
    """One construct, named by the facts that decide which authority owes it.

    A family alone is too coarse: `calls_and_adapters` covers both "argument
    contract needs unsupported storage" and "signature or result storage is
    incomplete", which are separate constructs owed by separate judgements.
    The refusal message is what separates them, so it joins the key.
    """
    key = family
    message = re.search(r"(XR_[A-Z0-9_]+: [a-z][^\n]*?)(?:\s+(?:operation|function|value)=|$)", rest)
    if message:
        key += "|" + message.group(1).strip()
    selector = re.search(r"selector=(\S+)", rest)
    definer = re.search(r"definer-opcode=(\d+)", rest)
    use = re.search(r"use-opcode=(\d+)", rest)
    opcode = re.search(r"(?<!-)opcode=(\d+)", rest)
    if selector:
        key += "|sel=" + selector.group(1)
    if definer and use:
        key += f"|d{definer.group(1)}->u{use.group(1)}"
    elif opcode:
        key += "|op" + opcode.group(1)
    return key


def survey_one(args):
    case, xray, timeout = args
    path = os.path.join(ROOT, case)
    if not os.path.exists(path):
        return case, None
    env = dict(os.environ, XRAY_COLLECT_ALL_REFUSALS="1")
    out_path = os.path.join("/tmp", "survey_%d.out" % os.getpid())
    try:
        proc = subprocess.run([xray, "build", "--native", path, "-o", out_path],
                              capture_output=True, text=True, timeout=timeout, env=env, cwd=ROOT)
    except subprocess.TimeoutExpired:
        return case, {"kinds": ["TIMEOUT"], "err": "TIMEOUT"}
    text = proc.stdout + proc.stderr
    kinds = set()
    for line in text.splitlines():
        match = SURVEY.match(line.strip())
        if match:
            kinds.add(refusal_key(match.group(1), match.group(2)))
    err = ""
    for line in text.splitlines():
        match = ERRLINE.match(line)
        if match:
            err = match.group(1)
            break
    if proc.returncode == 0 and not kinds:
        return case, {"kinds": [], "err": "BUILDS"}
    return case, {"kinds": sorted(kinds), "err": err or ("rc=%d" % proc.returncode)}


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--xray", default=os.path.join(ROOT, "build/xray"))
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--timeout", type=int, default=90)
    parser.add_argument("--json", default="")
    parser.add_argument("--top", type=int, default=20)
    options = parser.parse_args()

    cases = []
    for line in open(LIST):
        line = line.split("#")[0].strip()
        if line:
            cases.append(line)
    print("surveying %d cases with %s" % (len(cases), options.xray), file=sys.stderr)

    results = {}
    work = [(c, options.xray, options.timeout) for c in cases]
    with concurrent.futures.ThreadPoolExecutor(max_workers=options.jobs) as pool:
        for index, (case, result) in enumerate(pool.map(survey_one, work)):
            if result is not None:
                results[case] = result
            if (index + 1) % 50 == 0:
                print("  %d/%d" % (index + 1, len(cases)), file=sys.stderr)

    sole = collections.Counter()
    sole_cases = collections.defaultdict(list)
    for case, result in results.items():
        kinds = result.get("kinds") or []
        if len(kinds) == 1 and kinds[0] != "TIMEOUT":
            sole[kinds[0]] += 1
            sole_cases[kinds[0]].append(case)

    builds = [c for c, r in results.items() if r.get("err") == "BUILDS"]
    print("\n%d surveyed, %d already build, %d have a single remaining construct"
          % (len(results), len(builds), sum(sole.values())))
    print("\nconstructs ranked by cases they would finish:")
    for key, count in sole.most_common(options.top):
        print("  %4d  %s" % (count, key))

    if options.json:
        json.dump({"cases": results, "sole": dict(sole_cases)}, open(options.json, "w"), indent=1)
        print("\nwrote %s" % options.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
