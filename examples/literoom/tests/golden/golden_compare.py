#!/usr/bin/env python3
"""golden_compare.py - compare two literoom golden render files.

Use it after re-recording the golden file on purpose (a new slider, a new
effect...) to prove that NOTHING ELSE changed:

    python tests/golden/golden_compare.py OLD.txt NEW.txt

OLD is a copy of the golden file made before the change, NEW is the one
recorded after (tests/golden/render_<COMPILER>_<CONFIG>.txt). Both are the
files written by `golden_render_test --record`: one line per scenario,

    name hash width height sumR sumG sumB sumA

with '#' comment lines. Scenarios are matched by NAME. A scenario counts as
changed when its hash, size or channel sums differ.

Output (one line each, sorted by name):

    UNEXPECTED <name>    in both files, but the fingerprint changed
    VANISHED   <name>    in OLD only (renamed or removed)
    NEW        <name>    in NEW only (informational: added on purpose?)
    allowed    <name>    changed, but allowed (see below); shown with -v

Scenarios that set EVERY slider change whenever a slider is added, so these
patterns may change without failing the comparison:

    adj/all_*   sink/*   mask/every_effect   mask/composite_every_effect
    sidecar/sink_json

Add more with --allow PATTERN (fnmatch style, repeatable), or turn the built-in
list off with --no-default-allow.

Old golden files named their slider scenarios by table position
(adj/14_Texture/pos). The index is stripped from OLD names (becoming
adj/Texture/pos) so such a file can still be compared.

Exit code: 0 nothing unexpected changed and nothing vanished (NEW lines alone
do not fail), 1 something is UNEXPECTED or VANISHED, 2 a file could not be
read or has no scenarios.
"""
import argparse
import fnmatch
import re
import sys

DEFAULT_ALLOW = [
    "adj/all_*",
    "sink/*",
    "mask/every_effect",
    "mask/composite_every_effect",
    "sidecar/sink_json",
]

_INDEXED = re.compile(r"^(adj/)\d+_(.+)$")


def strip_index(name):
    """adj/14_Texture/pos -> adj/Texture/pos (old position-based names)."""
    m = _INDEXED.match(name)
    return m.group(1) + m.group(2) if m else name


def read_golden(path, strip=False):
    """name -> (hash, w, h, sumR, sumG, sumB, sumA) as strings, in file order.

    Returns None (after printing why) when the file is unusable.
    """
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()  # handles CRLF
    except OSError as e:
        print(f"error: cannot read {path}: {e}", file=sys.stderr)
        return None
    out = {}
    for n, line in enumerate(lines, 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) != 8:
            print(f"error: {path}:{n}: expected 8 fields, got {len(parts)}: {line[:60]}",
                  file=sys.stderr)
            return None
        name = strip_index(parts[0]) if strip else parts[0]
        if name in out:
            print(f"error: {path}:{n}: scenario '{name}' appears twice", file=sys.stderr)
            return None
        out[name] = tuple(parts[1:])
    if not out:
        print(f"error: {path} has no scenarios", file=sys.stderr)
        return None
    return out


def describe(fp):
    h, w, hh, r, g, b, a = fp
    return f"{h} {w}x{hh} sums {r}/{g}/{b}/{a}"


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Compare two literoom golden render files by scenario name.")
    ap.add_argument("old", help="the golden file from before the change")
    ap.add_argument("new", help="the golden file recorded after it")
    ap.add_argument("--allow", action="append", default=[], metavar="PATTERN",
                    help="also allow scenarios matching this fnmatch pattern to change")
    ap.add_argument("--no-default-allow", action="store_true",
                    help="do not allow the 'every slider' scenarios to change")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="also list allowed changes, with old and new fingerprints")
    args = ap.parse_args(argv)

    old = read_golden(args.old, strip=True)
    new = read_golden(args.new)
    if old is None or new is None:
        return 2

    allow = ([] if args.no_default_allow else list(DEFAULT_ALLOW)) + args.allow

    def allowed(name):
        return any(fnmatch.fnmatchcase(name, p) for p in allow)

    unexpected, vanished, added, allowed_changes, same = [], [], [], [], 0
    for name, fp in old.items():
        if name not in new:
            vanished.append(name)
        elif new[name] == fp:
            same += 1
        elif allowed(name):
            allowed_changes.append(name)
        else:
            unexpected.append(name)
    for name in new:
        if name not in old:
            added.append(name)

    for name in sorted(unexpected):
        print(f"UNEXPECTED {name}")
        if args.verbose:
            print(f"    old: {describe(old[name])}")
            print(f"    new: {describe(new[name])}")
    for name in sorted(vanished):
        print(f"VANISHED   {name}")
    for name in sorted(added):
        print(f"NEW        {name}")
    if args.verbose:
        for name in sorted(allowed_changes):
            print(f"allowed    {name}")
            print(f"    old: {describe(old[name])}")
            print(f"    new: {describe(new[name])}")

    print(f"{len(old)} old, {len(new)} new: {same} unchanged, "
          f"{len(allowed_changes)} changed (allowed), {len(unexpected)} UNEXPECTED, "
          f"{len(vanished)} VANISHED, {len(added)} NEW")
    ok = not unexpected and not vanished
    print("ok" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
