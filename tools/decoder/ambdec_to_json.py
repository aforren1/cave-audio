#!/usr/bin/env python3
"""Turn an AmbDec decoder file (.ambdec) into the layout's `bed_decode` block.

The Ambisonic Decoder Toolbox (ADT, Heller/Benjamin/Lopez-Lezcano) designs decoders the engine
does not build itself: two-band decoders whose low-frequency matrix matches rV to rE, and
mixed-order decoders for arrays with sparse vertical coverage ("Optimized Decoders for
Mixed-Order Ambisonics", AES 150, arXiv 2210.00378). It writes them as AmbDec presets. The engine
reads a plain JSON block instead, so this script converts once, offline, and the layout carries
the result. Stdlib only, deterministic.

    uv run tools/decoder/ambdec_to_json.py dec.ambdec --layout cave_layout.json -o bed_decode.json
    uv run tools/decoder/ambdec_to_json.py dec.ambdec --into cave_layout.json

The first form writes a standalone block (its top level IS the block). The second merges it into
a layout file in place, under `bed_decode`: a decoder for the same channel set is replaced, other
sets are kept, and every other field is left alone. Run it once per .ambdec: ADT writes one file
per order and per mixed set, and each lands beside the others (full1, full2, full3, 2H1V, ...).

THE AMBDEC FORMAT, version 3 (no official specification exists; this follows the reader in
OpenAL Soft, core/ambdec.cpp, and its docs/ambdec.txt, plus the files ADT version 8 writes):

  /dec/chan_mask     hex ACN bit mask of the input channels the matrices use (bit k = ACN k)
  /dec/freq_bands    1 (a /matrix/ block) or 2 (/lfmatrix/ and /hfmatrix/)
  /dec/speakers      speaker count
  /dec/coeff_scale   n3d, sn3d or fuma: the normalization the matrix expects its INPUT in
  /opt/xover_freq    two-band crossover, Hz
  /opt/xover_ratio   dB; positive lowers the LF band and raises the HF band by half of it each
  /opt/nfeff_comp    none, input or output: AmbDec's near-field compensation. NOT applied by the
                     engine (docs/spatialization.md, "Near-field compensation: deliberately
                     omitted"); the converter prints a note and records the value in the decoder's
                     `nfeff_comp` field, which the engine ignores
  /speakers/{ add_spkr <id> <dist m> <azimuth deg> <elevation deg> [<port>] /}
                     azimuth 0 = ahead, positive = LEFT; elevation positive = up
  /matrix/{ | /lfmatrix/{ | /hfmatrix/{
     order_gain <g0> <g1> <g2> <g3> [<g4>]   per-order gains, multiplied into every row
     add_row <c> ...  one row per speaker, in /speakers/ order, one coefficient per SET BIT of
                      chan_mask, in ascending ACN order
  /}
  /end

WHAT IT WRITES: per decoder a final 16-column matrix in the engine's basis (ACN/SN3D, x front /
y left / z up), the masked columns filled and the rest zero, the order gains and the xover ratio
folded in, rows keyed by the layout's speaker `index`. The engine applies it as it is: no max-rE
taper (the designer chose the order gains). The channel set comes from the mask: a full mask names
its order (0xf "full1", 0x1ff "full2", 0xffff "full3": a decoder's order gains fit one order, so
the engine picks a full decoder by the bed's order), and the six .amb mixed sets (1H 0xb, 2H 0x11b,
2H1V 0x11f, 3H 0x831b, 3H1V 0x831f, 3H2V 0x83ff) are named. Anything else is refused: the engine
has no bed of that shape to feed it.

SPEAKER MATCHING: an AmbDec file names its speakers by free-text id and places them by azimuth and
elevation around the array center; the layout keys them by `index`. Each AmbDec speaker is matched
to the layout speaker whose direction from the layout's listening point (`listening_point_m`, else
the speaker centroid, exactly as the engine computes it) is nearest, within --tol-deg (default
5 deg), and only when no other layout speaker is also inside the tolerance. Every layout speaker
must be matched exactly once. `--map ID=INDEX,...` pins a speaker by its AmbDec id (the angle is
still reported). AmbDec's distance, delay and level compensation are NOT used: the layout's own
trims do that job.
"""
import argparse
import json
import math
import pathlib
import sys

# ACN k -> spherical-harmonic order l
ORDER_OF = [0, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3]
# FuMa -> SN3D per ACN channel, for a SIGNAL (s_sn3d = s_fuma * f): the engine's own table
# (src/core/sound.c fuma_sn3d, by ACN here). A matrix that expects FuMa INPUT therefore becomes
# D_sn3d = D_fuma / f. W carries FuMa's -3 dB; 3rd order is not plain MaxN (the L..Q factors).
FUMA_TO_SN3D = [math.sqrt(2.0),                          # W
                1.0, 1.0, 1.0,                           # Y Z X
                math.sqrt(3.0) / 2, math.sqrt(3.0) / 2,  # V T
                1.0,                                     # R
                math.sqrt(3.0) / 2, math.sqrt(3.0) / 2,  # S U
                math.sqrt(5.0 / 8),                      # Q
                math.sqrt(5.0) / 3,                      # O
                math.sqrt(32.0 / 45),                    # M
                1.0,                                     # K
                math.sqrt(32.0 / 45),                    # L
                math.sqrt(5.0) / 3,                      # N
                math.sqrt(5.0 / 8)]                      # P
SETS = {0x000B: "1H", 0x011B: "2H", 0x011F: "2H1V", 0x831B: "3H", 0x831F: "3H1V", 0x83FF: "3H2V"}
FULL_MASKS = {0x000F: "full1", 0x01FF: "full2", 0xFFFF: "full3"}
MAX_DECODERS = 9                    # BWA_BED_DEC_MAX: one per channel set (3 full orders + 6 mixed)
XOVER_RANGE = (100.0, 4000.0)       # the layout schema's own bound
MAX_GAIN = 1.0e4                    # BWA_MAX_GAIN: the layout loader refuses past it


def fail(msg):
    raise SystemExit("ambdec_to_json: " + msg)


def parse_ambdec(text):
    """The decoder as a dict; refuses anything the format reader in OpenAL Soft would refuse."""
    d = {"version": None, "mask": None, "bands": None, "nspk": None, "scale": None,
         "xover": None, "xratio": 0.0, "nfeff": "none", "desc": "", "speakers": [], "mat": {}}
    scope = None
    for ln, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        tok = line.split()
        cmd = tok[0]
        where = "line %d: " % ln
        if cmd == "/}":
            if scope is None:
                fail(where + "unexpected /} in global scope")
            scope = None
            continue
        if scope == "speakers":
            if cmd != "add_spkr" or len(tok) < 5:
                fail(where + "expected add_spkr <id> <dist> <azimuth> <elevation> [<port>]")
            try:
                dist, az, el = float(tok[2]), float(tok[3]), float(tok[4])
            except ValueError:
                fail(where + "add_spkr distance/azimuth/elevation must be numbers")
            if not all(math.isfinite(v) for v in (dist, az, el)) or dist < 0:
                fail(where + "add_spkr values must be finite (distance >= 0)")
            d["speakers"].append({"id": tok[1], "dist": dist, "az": az, "el": el})
            continue
        if scope in ("matrix", "lf", "hf"):
            m = d["mat"].setdefault(scope, {"gain": None, "rows": []})
            try:
                vals = [float(v) for v in tok[1:]]
            except ValueError:
                fail(where + "%s values must be numbers" % cmd)
            if not all(math.isfinite(v) for v in vals):
                fail(where + "%s values must be finite" % cmd)
            if cmd == "order_gain":
                if len(vals) < 4:
                    fail(where + "order_gain needs 4 values (orders 0..3)")
                m["gain"] = vals[:4]
            elif cmd == "add_row":
                m["rows"].append(vals)
            else:
                fail(where + "unexpected matrix command %r" % cmd)
            continue
        if cmd == "/description":
            d["desc"] = line[len(cmd):].strip()
        elif cmd == "/version":
            d["version"] = int(tok[1])
        elif cmd == "/dec/chan_mask":
            d["mask"] = int(tok[1], 16)
        elif cmd == "/dec/freq_bands":
            d["bands"] = int(tok[1])
        elif cmd == "/dec/speakers":
            d["nspk"] = int(tok[1])
        elif cmd == "/dec/coeff_scale":
            d["scale"] = tok[1]
        elif cmd == "/opt/xover_freq":
            d["xover"] = float(tok[1])
        elif cmd == "/opt/xover_ratio":
            d["xratio"] = float(tok[1])
        elif cmd == "/opt/nfeff_comp":
            if len(tok) < 2 or tok[1] not in ("none", "input", "output"):
                fail(where + "/opt/nfeff_comp must be none, input or output")
            d["nfeff"] = tok[1]
        elif cmd in ("/opt/input_scale", "/opt/delay_comp", "/opt/level_comp"):
            pass                       # AmbDec runtime options: the engine has its own (see the docstring)
        elif cmd == "/speakers/{":
            scope = "speakers"
        elif cmd in ("/matrix/{", "/lfmatrix/{", "/hfmatrix/{"):
            scope = {"/matrix/{": "matrix", "/lfmatrix/{": "lf", "/hfmatrix/{": "hf"}[cmd]
        elif cmd == "/end":
            break
        else:
            fail(where + "unknown command %r" % cmd)
    else:
        fail("no /end line (truncated file?)")

    if d["version"] != 3:
        fail("only AmbDec version 3 files are supported (got %r)" % d["version"])
    if d["mask"] is None or d["bands"] is None or d["nspk"] is None or d["scale"] is None:
        fail("missing /dec/chan_mask, /dec/freq_bands, /dec/speakers or /dec/coeff_scale")
    if d["mask"] <= 0:
        fail("chan_mask must be nonzero")
    if d["mask"] > 0xFFFF:
        fail("chan_mask 0x%x uses channels above 3rd order; the engine renders beds to 3rd order" % d["mask"])
    channel_set(d["mask"])                            # refuse a mask the engine has no bed for, first
    if d["scale"] not in ("n3d", "sn3d", "fuma"):
        fail("coeff_scale must be n3d, sn3d or fuma (got %r)" % d["scale"])
    if d["bands"] not in (1, 2):
        fail("freq_bands must be 1 or 2 (got %d)" % d["bands"])
    if len(d["speakers"]) != d["nspk"]:
        fail("/dec/speakers says %d but /speakers/ lists %d" % (d["nspk"], len(d["speakers"])))
    want = ("matrix",) if d["bands"] == 1 else ("lf", "hf")
    if sorted(d["mat"]) != sorted(want):
        fail("a %d-band decoder needs %s" % (d["bands"], " and ".join("/%s/" % w.replace("lf", "lfmatrix")
                                                                      .replace("hf", "hfmatrix") for w in want)))
    ncol = bin(d["mask"]).count("1")
    for name, m in d["mat"].items():
        if len(m["rows"]) != d["nspk"]:
            fail("matrix %s has %d rows for %d speakers" % (name, len(m["rows"]), d["nspk"]))
        for r, row in enumerate(m["rows"]):
            if len(row) != ncol:
                fail("matrix %s row %d has %d coefficients; chan_mask 0x%x selects %d"
                     % (name, r, len(row), d["mask"], ncol))
        if m["gain"] is None:
            m["gain"] = [1.0, 1.0, 1.0, 1.0]
    if d["bands"] == 2:
        if d["xover"] is None:
            fail("a two-band decoder needs /opt/xover_freq")
        if not (XOVER_RANGE[0] <= d["xover"] <= XOVER_RANGE[1]):
            fail("xover_freq %g Hz is outside the engine's [%g, %g]" % (d["xover"], *XOVER_RANGE))
    return d


def channel_set(mask):
    if mask in FULL_MASKS:
        return FULL_MASKS[mask]
    if mask in SETS:
        return SETS[mask]
    fail("chan_mask 0x%x is neither a full set (0xf, 0x1ff, 0xffff) nor one of the .amb mixed sets "
         "(%s); the engine has no bed of that shape" % (mask, ", ".join("%s 0x%x" % (v, k) for k, v in SETS.items())))


def to_engine_matrix(d, name, gain_scale):
    """[speaker][16] in ACN/SN3D: mask expansion, order gains, input-scaling conversion."""
    acns = [k for k in range(16) if d["mask"] >> k & 1]
    m = d["mat"][name]
    out = []
    for row in m["rows"]:
        full = [0.0] * 16
        for c, k in zip(row, acns):
            v = c * m["gain"][ORDER_OF[k]] * gain_scale
            if d["scale"] == "n3d":
                v *= math.sqrt(2 * ORDER_OF[k] + 1)       # the matrix wants N3D input = SN3D * sqrt(2l+1)
            elif d["scale"] == "fuma":
                v /= FUMA_TO_SN3D[k]
            if not math.isfinite(v) or abs(v) > MAX_GAIN:
                fail("a converted gain (%g) is past the layout's +/-%g bound" % (v, MAX_GAIN))
            full[k] = v
        out.append(full)
    return out


def ambdec_dir_room(az_deg, el_deg):
    """AmbDec azimuth (0 ahead, + left) / elevation (+ up) -> a unit room vector. The room frame:
    right-handed, +y up, +z ahead, so room LEFT is +x (room-right is -x)."""
    az, el = math.radians(az_deg), math.radians(el_deg)
    return (math.cos(el) * math.sin(az), math.sin(el), math.cos(el) * math.cos(az))


def layout_dirs(layout):
    spk = layout.get("speakers")
    if not isinstance(spk, list) or len(spk) < 4:
        fail("the layout has no 'speakers' array of 4 or more")
    pos = {}
    for s in spk:
        pos[int(s["index"])] = [float(v) for v in s["position"]]
    if sorted(pos) != list(range(len(spk))):
        fail("the layout's speaker indices are not 0..N-1")
    lp = layout.get("listening_point_m")
    if lp is None:                                    # the engine's own default: the centroid
        lp = [sum(p[i] for p in pos.values()) / len(pos) for i in range(3)]
    dirs = {}
    for i, p in pos.items():
        v = [p[k] - lp[k] for k in range(3)]
        n = math.sqrt(sum(c * c for c in v))
        if n < 1e-6:
            fail("layout speaker %d sits on the listening point; it has no direction" % i)
        dirs[i] = [c / n for c in v]
    return dirs


def angle_deg(a, b):
    c = max(-1.0, min(1.0, sum(x * y for x, y in zip(a, b))))
    return math.degrees(math.acos(c))


def match_speakers(d, dirs, tol, pins):
    """AmbDec speaker order -> layout index, by direction (see the docstring)."""
    ids = [s["id"] for s in d["speakers"]]
    if len(set(ids)) != len(ids):
        fail("AmbDec speaker ids are not unique, so --map cannot name them")
    for pid in pins:
        if pid not in ids:
            fail("--map names %r, which is not an AmbDec speaker id (have: %s)" % (pid, " ".join(ids)))
    if len(d["speakers"]) != len(dirs):
        fail("the AmbDec file has %d speakers and the layout %d; the block needs one row per layout speaker"
             % (len(d["speakers"]), len(dirs)))
    used, out, report = {}, [], []
    for s in d["speakers"]:
        u = ambdec_dir_room(s["az"], s["el"])
        ranked = sorted((angle_deg(u, v), i) for i, v in dirs.items())
        if s["id"] in pins:
            idx = pins[s["id"]]
            if idx not in dirs:
                fail("--map %s=%d: the layout has no speaker index %d" % (s["id"], idx, idx))
            ang = angle_deg(u, dirs[idx])
            how = "pinned"
        else:
            ang, idx = ranked[0]
            if ang > tol:
                fail("AmbDec speaker %s (az %g, el %g) is %.1f deg from the nearest layout speaker (%d), "
                     "past --tol-deg %g; check the frames or pin it with --map %s=<index>"
                     % (s["id"], s["az"], s["el"], ang, idx, tol, s["id"]))
            if len(ranked) > 1 and ranked[1][0] <= tol:
                fail("AmbDec speaker %s is within %g deg of layout speakers %d and %d; ambiguous, "
                     "pin it with --map %s=<index>" % (s["id"], tol, ranked[0][1], ranked[1][1], s["id"]))
            how = "nearest"
        if idx in used:
            fail("layout speaker %d is matched by both %s and %s" % (idx, used[idx], s["id"]))
        used[idx] = s["id"]
        out.append(idx)
        report.append((s["id"], idx, ang, how))
    return out, report


def parse_map(text):
    pins = {}
    if not text:
        return pins
    for item in text.split(","):
        if "=" not in item:
            fail("--map takes ID=INDEX[,ID=INDEX...] (got %r)" % item)
        k, v = item.split("=", 1)
        try:
            pins[k.strip()] = int(v)
        except ValueError:
            fail("--map %s: index %r is not an integer" % (k, v))
    return pins


def build_decoder(d, order_idx, nlayout):
    cset = channel_set(d["mask"])
    # xover_ratio (dB): + lowers LF and raises HF by half of it each, as an AmbDec runtime would
    r = d["xratio"] if d["bands"] == 2 else 0.0
    lf_scale, hf_scale = 10.0 ** (-r / 40.0), 10.0 ** (r / 40.0)
    hf_name = "matrix" if d["bands"] == 1 else "hf"
    rows = {"hf": to_engine_matrix(d, hf_name, hf_scale)}
    if d["bands"] == 2:
        rows["lf"] = to_engine_matrix(d, "lf", lf_scale)
    dec = {"channels": cset}
    if d["bands"] == 2:
        dec["xover_hz"] = d["xover"]
    if d["nfeff"] != "none":
        dec["nfeff_comp"] = d["nfeff"]   # informational: the engine ignores this field
    for band in ("hf", "lf"):
        if band not in rows:
            continue
        by_index = [None] * nlayout
        for r_i, idx in enumerate(order_idx):
            by_index[idx] = rows[band][r_i]
        dec[band] = [{"index": i, "gains": [float("%.9g" % g) for g in by_index[i]]} for i in range(nlayout)]
    return dec


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("ambdec", help="the AmbDec (.ambdec) decoder file, version 3")
    ap.add_argument("--layout", help="the layout the speakers are matched against (default: --into)")
    ap.add_argument("-o", "--out", help="write a standalone bed_decode JSON here")
    ap.add_argument("--into", help="merge the decoder into this layout JSON's bed_decode, in place")
    ap.add_argument("--tol-deg", type=float, default=5.0, help="direction-match tolerance (default 5)")
    ap.add_argument("--map", help="pin speakers by AmbDec id: ID=INDEX[,ID=INDEX...]")
    args = ap.parse_args()
    if not args.out and not args.into:
        ap.error("give -o <file> and/or --into <layout.json>")
    lay_path = args.layout or args.into
    if not lay_path:
        ap.error("give --layout <layout.json> (the speakers are matched against it)")

    src = pathlib.Path(args.ambdec)
    d = parse_ambdec(src.read_text(encoding="utf-8", errors="replace"))
    layout = json.loads(pathlib.Path(lay_path).read_text(encoding="utf-8"))
    dirs = layout_dirs(layout)
    order_idx, report = match_speakers(d, dirs, args.tol_deg, parse_map(args.map))
    dec = build_decoder(d, order_idx, len(dirs))

    print("ambdec_to_json: %s" % src.name)
    if d["desc"]:
        print("  description: %s" % d["desc"])
    print("  channels: %s (chan_mask 0x%x), %d band%s, %s input scaling%s"
          % (dec["channels"], d["mask"], d["bands"], "s" if d["bands"] == 2 else "", d["scale"],
             (", crossover %g Hz, ratio %g dB" % (d["xover"], d["xratio"])) if d["bands"] == 2 else ""))
    for sid, idx, ang, how in report:
        print("    %-8s -> index %2d  (%.1f deg, %s)" % (sid, idx, ang, how))
    if d["nfeff"] != "none":
        print("  note: the file asks for near-field compensation (/opt/nfeff_comp %s). The engine does NOT "
              "apply it (plane-wave decode; docs/spatialization.md, \"Near-field compensation: deliberately "
              "omitted\"); the value is kept in the decoder's nfeff_comp field, which the engine ignores."
              % d["nfeff"])

    if args.out:
        block = {"source": src.name, "decoders": [dec]}
        pathlib.Path(args.out).write_text(json.dumps(block, indent=1) + "\n", newline="\n")
        print("ambdec_to_json: wrote %s" % args.out)
    if args.into:
        p = pathlib.Path(args.into)
        lay = json.loads(p.read_text(encoding="utf-8"))
        bd = lay.get("bed_decode")
        if not isinstance(bd, dict):
            bd = {"source": src.name, "decoders": []}
        decs = [x for x in bd.get("decoders", []) if x.get("channels") != dec["channels"]]
        decs.append(dec)
        # full orders first (full1, full2, full3), then the mixed sets by name
        decs.sort(key=lambda x: (not str(x.get("channels", "")).startswith("full"), str(x.get("channels", ""))))
        if len(decs) > MAX_DECODERS:
            fail("the layout's bed_decode would hold %d decoders; the engine takes at most %d"
                 % (len(decs), MAX_DECODERS))
        srcs = [s for s in str(bd.get("source", "")).split("; ") if s and s != src.name]
        bd["source"] = "; ".join(srcs + [src.name])
        bd["decoders"] = decs
        lay["bed_decode"] = bd
        p.write_text(json.dumps(lay, indent=2) + "\n", newline="\n")
        print("ambdec_to_json: merged the %s decoder into %s (bed_decode; everything else untouched)"
              % (dec["channels"], args.into))
    return 0


if __name__ == "__main__":
    sys.exit(main())
