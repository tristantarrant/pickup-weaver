#!/usr/bin/env python3
"""End-to-end test of the built DPF plugin (CLAP + LV2) against the standalone DSP core (pw_cli).

The plugin is loaded by two small test hosts (tests/clap_host.cpp, tests/lv2_host.cpp) that mimic
DAWs: static values are delivered the way a restored session delivers them (CLAP: params.flush
before activation; LV2: control ports, read by DPF in the first run), automation at block starts.
Every output must be bit-identical to pw_cli rendering the same settings.
"""
import hashlib
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
BUILD = os.path.join(ROOT, "build")
DSP = os.path.abspath(os.path.join(ROOT, "..", "dsp"))
OUT = "/tmp/opencode/plugin_test"
os.makedirs(OUT, exist_ok=True)


def sh(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        print("FAILED:", " ".join(cmd)); print(r.stdout[-2000:], r.stderr[-2000:]); sys.exit(1)
    return r


def build():
    sh(["cmake", "-S", ROOT, "-B", BUILD, "-DCMAKE_CXX_FLAGS=-march=x86-64", "-DCMAKE_C_FLAGS=-march=x86-64"])
    sh(["cmake", "--build", BUILD, "-j12"])
    inc = os.path.join(ROOT, "..", "dpf", "distrho", "src")
    sh(["g++", "-std=c++17", "-O2", "-I" + inc, os.path.join(HERE, "clap_host.cpp"), "-ldl", "-o", os.path.join(OUT, "clap_host")])
    lilv = subprocess.run(["pkg-config", "--cflags", "--libs", "lilv-0"], capture_output=True, text=True).stdout.split()
    sh(["g++", "-std=c++17", "-O2", os.path.join(HERE, "lv2_host.cpp"), "-o", os.path.join(OUT, "lv2_host")] + lilv)
    sh(["g++", "-std=c++17", "-O2", "-I" + DSP, os.path.join(DSP, "pw_cli.cpp"), "-o", os.path.join(OUT, "pw_cli")])
    lv2path = os.path.join(OUT, "lv2path")
    os.makedirs(lv2path, exist_ok=True)
    link = os.path.join(lv2path, "pickup_weaver.lv2")
    # relink every run: a stale absolute link (project moved) would otherwise
    # keep pointing at the old checkout
    if os.path.lexists(link):
        os.unlink(link)
    os.symlink(os.path.join(BUILD, "bin", "pickup_weaver.lv2"), link)
    return lv2path


def scenarios():
    S = [("defaults", []),
         ("static_ecustom", ["pickupTo=27", "customX=33", "customY=71", "amount=80", "pickupFrom=0", "tone=7",
                             "bodyAmount=40", "bodyThickness=55", "brightness=20", "gainDb=-3", "volume=8"]),
         ("static_accustom_corner_bypass", ["pickupTo=28", "customX=100", "customY=100", "pickupFrom=3", "bypass=1"])]
    ev = []
    for b in range(1, 110):
        ev.append("@%d:gainDb=%.2f,tone=%.2f,brightness=%.1f,customX=%.1f,customY=%.1f,amount=%.1f,volume=%.2f,"
                  "bodyAmount=%.1f,bodyThickness=%.1f" % (b * 512, (b % 40) - 20, 2 + (b % 9), (b % 50) * 4 - 100, b % 100,
                                                          (b * 7) % 100, 40 + (b * 13) % 60, 3 + b % 7, (b * 3) % 100, 10 + (b * 11) % 90))
    S.append(("automation_ecustom", ["pickupTo=27"] + ev))
    S.append(("switches", ["bodyAmount=60", "bodyThickness=70", "@4096:pickupTo=11", "@8192:pickupTo=3", "@12288:pickupFrom=3",
                           "@16384:pickupEnabled=0", "@20480:pickupEnabled=1", "@24576:bodyPhase=1", "@28672:tone=10",
                           "@30720:tone=4", "@32768:bypass=1", "@36864:bypass=0", "@40960:pickupTo=28,customX=90",
                           "@45056:amount=37", "@49152:bodyEnabled=0"]))
    return S


def main():
    lv2path = build()
    n = 57344
    rng = np.random.default_rng(11)
    x = rng.uniform(-0.5, 0.5, n).astype("<f4")
    inp = os.path.join(OUT, "in.f32"); x.tofile(inp)
    env = dict(os.environ, LV2_PATH=lv2path + ":/usr/lib/lv2")
    clap = os.path.join(BUILD, "bin", "pickup_weaver.clap")
    ok = True
    for name, args in scenarios():
        for bs in (512, 64):
            ref = os.path.join(OUT, "ref.f32")
            sh([os.path.join(OUT, "pw_cli"), "sr=44100", "bs=%d" % bs, "n=%d" % n, "in=" + inp, "out=" + ref] + args)
            r = np.fromfile(ref, dtype="<f4")
            row = []
            for host, target in (("clap_host", clap), ("lv2_host", "http://dataforte.net/plugins/pickupweaver")):
                o = os.path.join(OUT, host + ".f32")
                sh([os.path.join(OUT, host), target, inp, o, str(n), str(bs)] + args, env=env)
                y = np.fromfile(o, dtype="<f4")
                same = np.array_equal(y, r)
                ok &= same
                row.append("%s %s%s" % (host.split("_")[0].upper(), "bit-identical" if same else "DIFFERENT",
                                        "" if same else " (max %.2e)" % np.abs(y - r).max()))
            changed = not np.array_equal(r, x)
            print("%-30s bs=%-4d %s | output differs from input: %s" % (name, bs, " | ".join(row), changed))
    print("ALL BIT-IDENTICAL" if ok else "MISMATCHES FOUND")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
