#!/usr/bin/env python3
"""Compare PickupWeaverDSP's parameter-change behavior against the plugin's step recordings.

Each recording (hookprobe.py --ir --change=BLK:id:norm) is replayed through pw_cli with the
identical input signal (regenerated exactly as the guest did) and compared sample by sample.
"""
import json
import math
import os
import random
import struct
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
# Folder with the reference impulse-response captures (ir_*.f32), passed via the environment.
TMP = os.environ.get("PW_CAPTURE_DIR")
if not TMP or not os.path.isdir(TMP):
    sys.exit("set PW_CAPTURE_DIR to the folder containing the reference captures (ir_*.f32)")
CLI = os.path.join(HERE, "pw_cli")
KEYS = {0: ("bypass", lambda n: int(n > 0.5)), 1: ("pickupEnabled", lambda n: int(n > 0.5)),
        2: ("pickupFrom", lambda n: int(round(n * 3))), 3: ("pickupTo", lambda n: int(round(n * 28))),
        4: ("amount", lambda n: 100 * n), 5: ("customX", lambda n: 100 * n), 6: ("customY", lambda n: 100 * n),
        7: ("bodyEnabled", lambda n: int(n > 0.5)), 8: ("bodyThickness", lambda n: 10 + 90 * n),
        9: ("bodyAmount", lambda n: 100 * n), 10: ("brightness", lambda n: 200 * n - 100),
        11: ("gainDb", lambda n: 40 * n - 20), 12: ("volume", lambda n: 10 * n), 13: ("tone", lambda n: 10 * n),
        14: ("bodyPhase", lambda n: int(n > 0.5))}


def guest_input(m):
    """Channel-0 input exactly as hookprobe.py --ir generated it (float32)."""
    bs, nb, sr, amp, sig = m["block"], m["blocks"], m["sr"], m["amp"], m["sig"]
    rnd = random.Random(1234)
    x = np.zeros(bs * nb, dtype=np.float32)
    for b in range(nb):
        for c in range(m["chans"]["in"]):
            for i in range(bs):
                n = b * bs + i
                if sig == "impulse": v = amp if n == 0 else 0.0
                elif sig.startswith("sine:"): v = amp * math.sin(2 * math.pi * float(sig[5:]) * n / sr)
                elif sig == "noise": v = amp * (2 * rnd.random() - 1)
                elif sig == "dc": v = amp
                else: v = 0.0
                if c == 0: x[n] = v
    return x


def run(tag, smooth=None):
    raw = open(os.path.join(TMP, "ir_%s.f32" % tag), "rb").read(); nl = raw.index(b"\n")
    m = json.loads(raw[:nl])
    y_ref = np.frombuffer(raw[nl + 1:], dtype="<f4").reshape(m["chans"]["out"], -1)[0].astype(float)
    x = guest_input(m)
    x.tofile("/tmp/opencode/pws_in.f32")
    pn = m["params"]
    args = [CLI, "sr=%g" % m["sr"], "bs=%d" % m["block"], "n=%d" % len(x), "in=/tmp/opencode/pws_in.f32",
            "out=/tmp/opencode/pws_out.f32"]
    if smooth is not None:
        args.append("smooth=%r" % smooth)
    for i in range(15):
        k, f = KEYS[i]
        args.append("%s=%r" % (k, f(pn[str(i)])))
    s0 = None
    for blk, items in m["change"].items():
        s0 = int(blk) * m["block"]
        args.append("@%d:" % s0 + ",".join("%s=%r" % (KEYS[i][0], KEYS[i][1](v)) for i, v, _ in items))
    subprocess.run(args, check=True)
    y = np.fromfile("/tmp/opencode/pws_out.f32", dtype="<f4").astype(float)
    e = np.abs(y - y_ref) / m["amp"]
    pre = e[:s0].max()
    trans = e[s0:s0 + 2048].max()
    post = e[s0 + 4096:].max() if len(e) > s0 + 4096 else float("nan")
    return m, pre, trans, post


def main():
    tags = ["gstep_ctl", "vstep", "bstep", "tstep", "bodystep", "thkstep", "tostep", "amtstep", "bypstep",
            "pkoffstep", "fromstep"] + ["xf_%s_512_step" % k for k in ("pkon", "pkoff", "bodyon", "phase", "from", "to",
                                                                       "tone_en", "tone_dis", "byp")]
    print("max |C++ - plugin| relative to input amplitude (bs=512 recordings; Option B ramp = 512 samples here)")
    print("%-20s %-28s %10s %12s %12s" % ("recording", "change", "before", "transition", "after"))
    for t in tags:
        m, pre, trans, post = run(t)
        ch = "; ".join("%s->%s" % (KEYS[i][0], KEYS[i][1](v)) for items in m["change"].values() for i, v, _ in items)
        print("%-20s %-28s %10.1e %12.2e %12.1e" % (t, ch[:28], pre, trans, post))
    print()
    print("same steps recorded at bs=128 (plugin ramps 128 samples, Option B still 512 -> expected difference):")
    for k in ("pkon", "bodyon", "phase", "tone_en"):
        m, pre, trans, post = run("xf_%s_128_step" % k)
        print("%-20s %10.1e %12.2e %12.1e" % ("xf_%s_128_step" % k, pre, trans, post))
    return 0


if __name__ == "__main__":
    sys.exit(main())
