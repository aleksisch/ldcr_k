#!/usr/bin/env python3
"""Compare the points-to facts of several analyses of the same program.

    scripts/compare.py andersen=a.txt plain=p.txt refine=r.txt

Each file has `var -> obj` lines (ldc --dump-pts / --dump-andersen-pts, written by one run so
that the names agree). The first analysis is the reference: a sound analysis over the same model
must be a subset of it for every variable, so the facts outside it are listed. The variables are
those of all files; a variable missing from a file points to nothing there.
"""
import sys
from collections import defaultdict


def load(path):
    facts = defaultdict(set)
    with open(path) as lines:
        for line in lines:
            var, sep, obj = line.rstrip("\n").partition(" -> ")
            if sep:
                facts[var].add(obj)
    return facts


def main(args):
    if len(args) < 2 or any("=" not in arg for arg in args):
        sys.exit(__doc__)
    names, analyses = [], []
    for arg in args:
        name, path = arg.split("=", 1)
        names.append(name)
        analyses.append(load(path))
    variables = set().union(*analyses)
    width = max(len(name) for name in names)

    print(f"{len(variables)} variables")
    print(f"{'':{width}}  {'sum |pts|':>10}  {'mean':>6}  {'empty':>6}  {'|pts|=1':>7}  {'max':>5}")
    for name, facts in zip(names, analyses):
        sizes = [len(facts.get(v, ())) for v in variables]
        print(f"{name:{width}}  {sum(sizes):>10}  {sum(sizes) / len(sizes):>6.2f}  "
              f"{sizes.count(0):>6}  {sizes.count(1):>7}  {max(sizes):>5}")

    reference, base = names[0], analyses[0]
    for name, facts in zip(names[1:], analyses[1:]):
        smaller = equal = larger = 0
        outside = []
        for v in variables:
            ours, theirs = facts.get(v, set()), base.get(v, set())
            outside += [(v, o) for o in sorted(ours - theirs)]
            if ours < theirs:
                smaller += 1
            elif ours == theirs:
                equal += 1
            else:
                larger += 1
        print(f"\n{name} vs {reference}: {smaller} variables smaller, {equal} equal, "
              f"{larger} not a subset")
        if outside:
            print(f"  {len(outside)} facts outside {reference}, e.g.:")
            for v, o in outside[:10]:
                print(f"    {v} -> {o}")


if __name__ == "__main__":
    main(sys.argv[1:])
