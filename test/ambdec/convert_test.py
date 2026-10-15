#!/usr/bin/env python3
"""ctest `ambdec_convert`: tools/decoder/ambdec_to_json.py against a known fixture.

fixture.ambdec is a two-band 2H1V decoder (chan_mask 0x11f = ACN 0 1 2 3 4 8) in N3D scaling,
six speakers listed in a different order from layout.json's indices. Every coefficient is
(row + 1) + (col + 1) / 10 (LF) or twice that (HF), so each converted gain has a closed form:

    gain[ACN k] = value * order_gain[l(k)] * sqrt(2 l(k) + 1)        (N3D input -> SN3D input)

The checks: the channel set and crossover, the speaker-by-direction mapping (AmbDec azimuth is
positive LEFT, room-left is +x), the scaling, the column expansion (zeros outside the mask), the
FuMa and SN3D variants, the xover_ratio split, a full mask naming its ORDER (full1/full3), the
near-field-compensation note (the fixture asks for it; the engine never applies it), the --into
merge of per-order decoders, and each refusal.
Exit 0 = pass, 1 = fail, 77 = skipped (the converter is missing from this checkout).
"""
import json
import math
import os
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
CONV = ROOT / "tools" / "decoder" / "ambdec_to_json.py"
FIX = (HERE / "fixture.ambdec").read_text()
LAYOUT = HERE / "layout.json"

fails = 0


def check(cond, msg):
    global fails
    if not cond:
        print("FAIL: " + msg)
        fails += 1


def run(ambdec_text, *extra, layout=LAYOUT):
    """Run the converter on `ambdec_text`; (exit code, stdout+stderr, block or None)."""
    with tempfile.TemporaryDirectory() as td:
        src = pathlib.Path(td) / "dec.ambdec"
        src.write_text(ambdec_text)
        out = pathlib.Path(td) / "block.json"
        args = [sys.executable, str(CONV), str(src), "--layout", str(layout), "-o", str(out)] + list(extra)
        p = subprocess.run(args, capture_output=True, text=True)
        block = json.loads(out.read_text()) if p.returncode == 0 and out.exists() else None
        return p.returncode, p.stdout + p.stderr, block


def rows_by_index(dec, band):
    return {r["index"]: r["gains"] for r in dec[band]}


def value(row, col, band):            # the fixture's coefficient
    v = (row + 1) + (col + 1) / 10.0
    return v if band == "lf" else 2.0 * v


ORDER = [0, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3]
MASKED = [0, 1, 2, 3, 4, 8]                                  # 0x11f, in ACN order = the file's column order
OG = {"lf": [1.0, 1.0, 1.0, 0.0], "hf": [1.0, 0.5, 0.25, 0.0]}
# AmbDec speaker order S1..S6 -> the layout index its direction lands on
WANT_MAP = {0: 4, 1: 3, 2: 0, 3: 5, 4: 2, 5: 1}             # up, right(-90 = room -x), front, down, back, left


def main():
    if not CONV.exists():
        print("SKIP: %s not found" % CONV)
        return 77

    rc, log, block = run(FIX)
    check(rc == 0, "the fixture converts (exit %d):\n%s" % (rc, log))
    if rc != 0:
        return 1
    check(block["source"] == "dec.ambdec", "source names the input file")
    check(len(block["decoders"]) == 1, "one decoder")
    dec = block["decoders"][0]
    check(dec["channels"] == "2H1V", "chan_mask 0x11f names the 2H1V set (got %r)" % dec["channels"])
    check(dec["xover_hz"] == 400.0, "two-band crossover carried")
    # /opt/nfeff_comp input: a note on stdout, and the value kept in a field the engine ignores
    check(dec.get("nfeff_comp") == "input", "nfeff_comp recorded in the decoder (got %r)" % dec.get("nfeff_comp"))
    check("near-field compensation" in log and "NOT" in log, "the converter notes that NFC is not applied:\n" + log)
    rc, log2, b2 = run(FIX.replace("/opt/nfeff_comp   input", "/opt/nfeff_comp   none"))
    check(rc == 0 and b2 and "nfeff_comp" not in b2["decoders"][0] and "near-field" not in log2,
          "nfeff_comp none: no field, no note:\n" + log2)
    rc, log2, _ = run(FIX.replace("/opt/nfeff_comp   input", "/opt/nfeff_comp   sideways"))
    check(rc != 0 and "nfeff_comp must be" in log2, "an unknown nfeff_comp value is refused:\n" + log2)
    for band in ("hf", "lf"):
        rows = rows_by_index(dec, band)
        check(sorted(rows) == list(range(6)), "%s: one row per layout index" % band)
        for amb_row, idx in WANT_MAP.items():
            g = rows[idx]
            check(len(g) == 16, "%s row %d has 16 gains" % (band, idx))
            for k in range(16):
                if k in MASKED:
                    col = MASKED.index(k)
                    want = value(amb_row, col, band) * OG[band][ORDER[k]] * math.sqrt(2 * ORDER[k] + 1)
                else:
                    want = 0.0
                check(abs(g[k] - want) < 1e-6 * max(1.0, abs(want)),
                      "%s index %d ACN %d: got %.9g, want %.9g (map S%d -> %d, n3d scaling, order gain)"
                      % (band, idx, k, g[k], want, amb_row + 1, idx))

    # SN3D input: no rescale. FuMa input: D_sn3d = D_fuma / f, f = the engine's FuMa->SN3D signal factor.
    rc, log, b = run(FIX.replace("/dec/coeff_scale n3d", "/dec/coeff_scale sn3d"))
    check(rc == 0, "sn3d variant converts:\n" + log)
    if b:
        g = rows_by_index(b["decoders"][0], "hf")[4]                 # S1 (row 0) -> index 4
        check(abs(g[3] - value(0, 3, "hf") * 0.5) < 1e-6, "sn3d: X = value * order gain, no rescale")
    rc, log, b = run(FIX.replace("/dec/coeff_scale n3d", "/dec/coeff_scale fuma"))
    check(rc == 0, "fuma variant converts:\n" + log)
    if b:
        g = rows_by_index(b["decoders"][0], "lf")[4]
        check(abs(g[0] - value(0, 0, "lf") / math.sqrt(2.0)) < 1e-6, "fuma: W / sqrt(2) (FuMa's -3 dB W)")
        check(abs(g[4] - value(0, 4, "lf") / (math.sqrt(3.0) / 2)) < 1e-6, "fuma: V / (sqrt(3)/2)")
        check(abs(g[1] - value(0, 1, "lf")) < 1e-6, "fuma: first order unchanged")

    # xover_ratio +6 dB: LF down 3 dB, HF up 3 dB
    rc, log, b = run(FIX.replace("/opt/xover_ratio  0.000000", "/opt/xover_ratio  6.0"))
    check(rc == 0, "xover_ratio variant converts:\n" + log)
    if b:
        lf = rows_by_index(b["decoders"][0], "lf")[4][0]
        hf = rows_by_index(b["decoders"][0], "hf")[4][0]
        check(abs(lf - value(0, 0, "lf") * 10 ** (-6 / 40)) < 1e-6, "xover_ratio lowers LF by half")
        check(abs(hf - value(0, 0, "hf") * 10 ** (6 / 40)) < 1e-6, "xover_ratio raises HF by half")

    # a single-band full first-order decoder: "full1" (a full mask names its order), no lf, no xover_hz
    single = (FIX.replace("/dec/chan_mask   11f", "/dec/chan_mask   f")
                 .replace("/dec/freq_bands  2", "/dec/freq_bands  1"))
    single = single[:single.index("/lfmatrix/{")] + "/matrix/{\norder_gain 1 1 0 0\n" + \
        "".join("add_row 0.1 0.2 0.3 0.4\n" for _ in range(6)) + "/}\n/end\n"
    rc, log, b = run(single)
    check(rc == 0, "single-band first-order converts:\n" + log)
    if b:
        d0 = b["decoders"][0]
        check(d0["channels"] == "full1" and "lf" not in d0 and "xover_hz" not in d0, "single band: full1, hf only")
    # a single-band full THIRD-order decoder: mask ffff names full3
    full3 = (single.replace("/dec/chan_mask   f", "/dec/chan_mask   ffff")
                   .replace("add_row 0.1 0.2 0.3 0.4\n", "add_row " + " ".join("0.%02d" % (k + 1) for k in range(16)) + "\n")
                   .replace("order_gain 1 1 0 0", "order_gain 1 0.86 0.61 0.3"))
    rc, log, b = run(full3)
    check(rc == 0, "single-band third-order converts:\n" + log)
    if b:
        check(b["decoders"][0]["channels"] == "full3", "mask ffff names full3 (got %r)" % b["decoders"][0]["channels"])

    # --map pins a speaker the direction match would refuse
    off = FIX.replace("add_spkr S3 2.0 0 0", "add_spkr S3 2.0 10 0")
    rc, log, _ = run(off)
    check(rc != 0 and "past --tol-deg" in log, "a speaker 10 deg off is refused:\n" + log)
    rc, log, b = run(off, "--map", "S3=0")
    check(rc == 0, "--map S3=0 pins it:\n" + log)
    if b:
        check(rows_by_index(b["decoders"][0], "hf")[0][0] == rows_by_index(block["decoders"][0], "hf")[0][0],
              "--map lands S3 on index 0")
    rc, log, _ = run(off, "--tol-deg", "12")
    check(rc == 0, "a wider --tol-deg accepts it:\n" + log)

    # refusals
    def refuses(text, needle, what, *extra):
        rc, log, _ = run(text, *extra)
        check(rc != 0 and needle in log, "%s is refused with %r (exit %d):\n%s" % (what, needle, rc, log))

    refuses(FIX.replace("/dec/chan_mask   11f", "/dec/chan_mask   7"), "neither a full set", "an unknown mask")
    refuses(FIX.replace("/dec/chan_mask   11f", "/dec/chan_mask   1ffff"), "above 3rd order", "a 4th-order mask")
    refuses(FIX.replace("/version         3", "/version         2"), "version 3", "AmbDec version 2")
    refuses(FIX.replace("add_row 1.1 1.2 1.3 1.4 1.5 1.6", "add_row 1.1 1.2 1.3 1.4 1.5"), "coefficients",
            "a short row")
    refuses(FIX.replace("/opt/xover_freq   400.000000", "/opt/xover_freq   50"), "xover_freq", "a 50 Hz crossover")
    refuses(FIX.replace("add_spkr S6 2.0 90 0", "add_spkr S6 2.0 -91 0"), "matched by both",
            "two AmbDec speakers on one layout speaker", "--tol-deg", "3")
    refuses(FIX.replace("/end", ""), "no /end", "a truncated file")
    refuses(FIX.replace("add_row 1.1 1.2 1.3 1.4 1.5 1.6", "add_row 1.1 1.2 1.3 1.4 1.5 1e9"), "bound",
            "an absurd gain")
    refuses(FIX, "not an AmbDec speaker id", "--map with an unknown id", "--map", "Q9=0")

    # --into: the decoder merges into an existing layout; a second run for another set keeps both
    with tempfile.TemporaryDirectory() as td:
        lay = pathlib.Path(td) / "layout.json"
        lay.write_text(LAYOUT.read_text())
        src = pathlib.Path(td) / "full.ambdec"
        src.write_text(single)
        mix = pathlib.Path(td) / "mixed.ambdec"
        mix.write_text(FIX)
        f3 = pathlib.Path(td) / "full3.ambdec"
        f3.write_text(full3)
        # ADT's per-order files, one at a time: each ADDS its set; the repeat REPLACES 2H1V
        for f in (mix, f3, src, mix):
            p = subprocess.run([sys.executable, str(CONV), str(f), "--into", str(lay)], capture_output=True, text=True)
            check(p.returncode == 0, "--into %s:\n%s" % (f.name, p.stdout + p.stderr))
        merged = json.loads(lay.read_text())
        sets = [d["channels"] for d in merged["bed_decode"]["decoders"]]
        check(sets == ["full1", "full3", "2H1V"],
              "--into adds per-order decoders beside each other, one per set, full orders first (got %r)" % sets)
        check(merged["bed_decode"]["source"] == "full3.ambdec; full.ambdec; mixed.ambdec",
              "--into records every source once (got %r)" % merged["bed_decode"]["source"])
        check(len(merged["speakers"]) == 6 and merged["listening_point_m"] == [0, 1.5, 0],
              "--into leaves the rest of the layout alone")

    print("ambdec_convert: %s (%d failure%s)" % ("PASS" if not fails else "FAIL", fails, "" if fails == 1 else "s"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
