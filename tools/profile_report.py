#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reads a GuestProfiler .tsv (atlan/profiler.hpp) and prints the busiest guest functions.

usage: profile_report.py PROFILE.tsv [--top N] [--frames N]
  --frames N  divide every count by N (per-frame figures)
Names are demangled with c++filt when it is installed.
"""
import argparse, collections, subprocess, sys

def demangle(names):
    try:
        out = subprocess.run(["c++filt"], input="\n".join(names), capture_output=True, text=True, check=True).stdout.split("\n")
        return dict(zip(names, out))
    except (OSError, subprocess.CalledProcessError):
        return {n: n for n in names}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("profile")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--frames", type=float, default=1.0)
    a = ap.parse_args()
    funcs, callers = {}, collections.defaultdict(list)
    for line in open(a.profile):
        p = line.rstrip("\n").split("\t")
        if p[0] == "F":
            funcs[p[1]] = dict(addr=p[1], name=p[2], calls=int(p[3]), self=int(p[4]), self_ms=float(p[5]), incl=int(p[6]), incl_ms=float(p[7]))
        elif p[0] == "E":
            callers[p[2]].append((int(p[3]), int(p[4]), p[1]))
    pretty = demangle([f["name"] for f in funcs.values() if f["name"] != "-"])
    label = lambda addr: (lambda f: pretty.get(f["name"], "0x" + addr) if f and f["name"] != "-" else "0x" + addr)(funcs.get(addr))
    total = sum(f["self"] for f in funcs.values())
    total_ms = sum(f["self_ms"] for f in funcs.values())
    n = a.frames
    print(f"total: {total / n:,.0f} instructions, {total_ms / n:.2f} ms" + (f" per frame ({n:g} frames)" if n != 1 else ""))
    for title, key in (("self instructions", "self"), ("inclusive instructions", "incl"), ("self host time", "self_ms")):
        print(f"\n== by {title} ==")
        print(f"{'calls':>10} {'self ins':>12} {'self%':>6} {'self ms':>9} {'incl ins':>12} {'incl%':>6} {'incl ms':>9} {'ins/call':>9}  function")
        for f in sorted(funcs.values(), key=lambda f: -f[key])[:a.top]:
            per = f["incl"] / f["calls"] if f["calls"] else 0
            print(f"{f['calls'] / n:10.1f} {f['self'] / n:12,.0f} {100 * f['self'] / max(total, 1):6.2f} {f['self_ms'] / n:9.3f} "
                  f"{f['incl'] / n:12,.0f} {100 * f['incl'] / max(total, 1):6.2f} {f['incl_ms'] / n:9.3f} {per:9.1f}  {label(f['addr'])}")
            for calls, incl, caller in sorted(callers[f["addr"]], reverse=True)[:3]:
                print(f"{'':>10} {'':>12} {'':>6} {'':>9} {incl / n:12,.0f} {'':>6} {'':>9} {'':>9}    <- {calls / n:.1f} calls from {label(caller)}")

if __name__ == "__main__":
    sys.exit(main())
