#!/usr/bin/env python3
"""Compare PickupWeaverDSP (via pw_cli) against every static impulse capture of the real plugin."""
import glob
import json
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
# Folder with the reference impulse-response captures (ir_*.f32), passed via the environment.
TMP = os.environ.get("PW_CAPTURE_DIR")
if not TMP or not os.path.isdir(TMP):
    sys.exit("set PW_CAPTURE_DIR to the folder containing the reference captures (ir_*.f32)")
CLI = os.path.join(HERE, "pw_cli")


def plain(pn):
    g = lambda i: pn[str(i)]
    return dict(bypass=int(g(0) > 0.5), pickupEnabled=int(g(1) > 0.5), pickupFrom=int(round(g(2) * 3)),
                pickupTo=int(round(g(3) * 28)), amount=100 * g(4), customX=100 * g(5), customY=100 * g(6),
                bodyEnabled=int(g(7) > 0.5), bodyThickness=10 + 90 * g(8), bodyAmount=100 * g(9),
                bodyPhase=int(g(14) > 0.5), brightness=200 * g(10) - 100, gainDb=40 * g(11) - 20,
                volume=10 * g(12), tone=10 * g(13))


def main():
    rows = []
    for path in sorted(glob.glob(os.path.join(TMP, "ir_*.f32"))):
        raw = open(path, "rb").read()
        nl = raw.index(b"\n")
        m = json.loads(raw[:nl])
        if m.get("sig") != "impulse" or m.get("change") or m.get("pchg") or m.get("blocks", 1) != 1 \
                or m.get("block", 0) < 16384 or "params" not in m:
            continue
        h = np.frombuffer(raw[nl + 1:], dtype="<f4").reshape(len(m["chans"]) and m["chans"]["out"], -1)[0].astype(float)
        n = len(h)
        p = plain(m["params"])
        out = "/tmp/opencode/pwc_out.f32"
        args = [CLI, "sr=%g" % m["sr"], "bs=%d" % n, "n=%d" % n, "amp=%r" % m["amp"], "out=" + out] + \
               ["%s=%r" % kv for kv in p.items()]
        subprocess.run(args, check=True)
        y = np.fromfile(out, dtype="<f4").astype(float)
        H = np.fft.rfft(h / m["amp"]); Y = np.fft.rfft(y / m["amp"])
        fr = np.fft.rfftfreq(n, 1 / m["sr"])
        sel = (fr >= 20) & (fr <= 20000)
        mask = sel & (np.abs(H) > np.abs(H[sel]).max() * 1e-4)
        err = np.abs(Y[mask] / H[mask] - 1).max() if mask.any() else 0.0
        td = np.abs(y - h).max() / max(np.abs(h).max(), 1e-30)
        rows.append((err, td, os.path.basename(path)[3:-4], p))
    rows.sort(key=lambda r: -r[0])
    print("%d static captures compared" % len(rows))
    print("worst 10 (freq-domain max complex rel err | time-domain max err rel to peak):")
    for err, td, tag, p in rows[:10]:
        print("  %-16s %.2e | %.2e   to=%d from=%d amt=%.0f tone=%.1f" % (tag, err, td, p["pickupTo"], p["pickupFrom"], p["amount"], p["tone"]))
    errs = np.array([r[0] for r in rows])
    print("median %.2e, 90th pct %.2e, max %.2e" % (np.median(errs), np.percentile(errs, 90), errs.max()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
