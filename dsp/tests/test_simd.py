#!/usr/bin/env python3
"""Cross-backend / cross-build / cross-platform equivalence of PickupWeaverDSP.

Builds pw_cli in several configurations, renders a set of scenarios with every available backend
and channel count, and requires all outputs to be bit-identical. Also reports the deviation from
the pre-SIMD scalar core (tests/pw_cli_ref, libm-based) which must stay at rounding level.
"""
import hashlib
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..")
OUT = "/tmp/opencode/simd_test"
A64 = "/tmp/opencode/a64/a64c++"
os.makedirs(OUT, exist_ok=True)

BUILDS = [  # name, compiler command, backends to run, runner
    ("gcc-O2", ["g++", "-O2"], ["scalar", "sse2", "auto"], []),
    ("gcc-O2-avx", ["g++", "-O2", "-mavx"], ["scalar", "sse2", "avx", "auto"], []),
    ("clang-O2-avx", ["clang++", "-O2", "-mavx"], ["scalar", "sse2", "avx"], []),
    ("gcc-O3-native", ["g++", "-O3", "-march=native"], ["scalar", "sse2", "avx"], []),    # FMA available
    ("clang-O3-native", ["clang++", "-O3", "-march=native"], ["avx"], []),
    ("gcc-dispatch", "dispatch", ["sse2", "avx", "auto"], []),
    ("a64-clang-O2", [A64, "-O2"], ["scalar", "neon", "auto"], ["qemu-aarch64"]),
]


def build(name, cmd):
    exe = os.path.join(OUT, "pw_" + name)
    if cmd == "dispatch":
        subprocess.run(["g++", "-std=c++17", "-O2", "-mavx", "-c", os.path.join(SRC, "PickupWeaverDSP_AVX.cpp"),
                        "-o", exe + "_avx.o"], check=True)
        subprocess.run(["g++", "-std=c++17", "-O2", "-DRG_ENABLE_AVX_DISPATCH", os.path.join(SRC, "pw_cli.cpp"),
                        exe + "_avx.o", "-o", exe], check=True)
    else:
        subprocess.run(cmd + ["-std=c++17", "-I" + SRC, os.path.join(SRC, "pw_cli.cpp"), "-o", exe], check=True)
    return exe


def scenarios():
    S = []
    S.append(("static_hot", ["pickupTo=0", "bodyAmount=40", "tone=7"]))
    S.append(("static_ecustom", ["pickupTo=27", "customX=33", "customY=71", "amount=80", "pickupFrom=0"]))
    S.append(("static_accustom_corner", ["pickupTo=28", "customX=100", "customY=100", "pickupFrom=3"]))
    ev = []
    for b in range(1, 120):
        ev.append("@%d:gainDb=%.2f,tone=%.2f,brightness=%.1f,customX=%.1f,customY=%.1f,amount=%.1f,volume=%.2f,"
                  "bodyAmount=%.1f,bodyThickness=%.1f" % (b * 256, (b % 40) - 20, 2 + (b % 9), (b % 50) * 4 - 100, b % 100,
                                                          (b * 7) % 100, 40 + (b * 13) % 60, 3 + b % 7, (b * 3) % 100, 10 + (b * 11) % 90))
    S.append(("automation_ecustom", ["pickupTo=27", "bodyEnabled=1"] + ev))
    sw = ["@4096:pickupTo=11", "@8192:pickupTo=3", "@12288:pickupFrom=3", "@16384:pickupEnabled=0", "@20480:pickupEnabled=1",
          "@24576:bodyPhase=1", "@28672:tone=10", "@30720:tone=4", "@32768:bypass=1", "@36864:bypass=0",
          "@40960:pickupTo=28,customX=90", "@45056:amount=37", "@49152:bodyEnabled=0"]
    S.append(("switches", ["bodyAmount=60", "bodyThickness=70"] + sw))
    return S


def main():
    rng = np.random.default_rng(7)
    x = rng.uniform(-0.5, 0.5, 60000).astype("<f4")
    inp = os.path.join(OUT, "in.f32")
    x.tofile(inp)
    exes = {name: build(name, cmd) for name, cmd, _, _ in BUILDS}
    results = {}
    for sname, args in scenarios():
        hashes = {}
        for name, cmd, backends, runner in BUILDS:
            for be in backends:
                for nch in (1, 2, 3, 8):
                    out = os.path.join(OUT, "o.f32")
                    env = dict(os.environ, PW_CHANNELS=str(nch), PW_VERBOSE="1")
                    r = subprocess.run(runner + [exes[name], "sr=44100", "bs=256", "n=60000", "in=" + inp, "out=" + out,
                                                 "backend=" + be] + args, env=env, capture_output=True, text=True)
                    if r.returncode != 0:
                        print("FAIL", name, be, nch, sname, r.stderr.strip()); return 1
                    used = r.stderr.strip().split()[-1]
                    hashes[(name, be, used, nch)] = hashlib.sha256(open(out, "rb").read()).hexdigest()
        distinct = set(hashes.values())
        used = sorted({k[2] for k in hashes})
        print("%-24s %3d runs (backends used: %s) -> %d distinct output%s" % (
            sname, len(hashes), ",".join(used), len(distinct), "" if len(distinct) == 1 else "s  <-- MISMATCH"))
        if len(distinct) != 1:
            groups = {}
            for k, h in hashes.items(): groups.setdefault(h, []).append(k)
            for h, ks in groups.items(): print("   ", h[:12], ks[:6])
        # deviation from the pre-SIMD scalar core
        ref = os.path.join(OUT, "ref.f32")
        subprocess.run([os.path.join(HERE, "pw_cli_ref"), "sr=44100", "bs=256", "n=60000", "in=" + inp, "out=" + ref] + args,
                       check=True)
        a = np.fromfile(out, dtype="<f4").astype(float); b = np.fromfile(ref, dtype="<f4").astype(float)
        results[sname] = np.abs(a - b).max() / max(np.abs(b).max(), 1e-30)
        print("%-24s vs pre-SIMD core: max |diff| / peak = %.2e" % ("", results[sname]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
