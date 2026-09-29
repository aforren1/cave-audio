#!/usr/bin/env python3
"""Turn a loudspeaker's CLF simulation file (.cf2) into the layout's `directivity` block.

Speaker vendors publish directivity balloons for room-acoustics simulators in the Common
Loudspeaker Format (CLF, clfgroup.org). Genelec ships one for the 4410A
(https://www.genelec.com/4410a, "4410A Simulation File (CLF)"). The engine reads a plain
JSON table instead, so this script does the decode once, offline, and the layout carries the
result. Stdlib only, deterministic.

    uv run tools/directivity/clf_to_json.py Genelec_Oy-4410A.CF2 -o genelec_4410a_directivity.json
    uv run tools/directivity/clf_to_json.py Genelec_Oy-4410A.CF2 --into cave_layout.json

The first form writes a standalone table (its top level IS the block). The second merges the
block into a layout file in place, under `directivity`, leaving every other field alone.

WHAT A CF2 HOLDS, as decoded from Genelec's v2.1a file (the CLF group publishes the format's
definition on request and a free viewer; the binary itself is undocumented, so every rule
below was established from the file's own consistency and is checked at run time):

  - a text header (maker, model, notes) followed by binary tables;
  - 30 third-octave bands, 20 Hz .. 16 kHz, each a full balloon of 72 meridians (5 deg steps
    of rotation about the acoustic axis) x 37 polar steps (0 .. 180 deg off the axis, 5 deg
    apart), float32 dB relative to on-axis. Polar 0 is on-axis and reads within a few
    hundredths of 0 dB in every band; the 37th polar entry (180 deg) is a zero placeholder,
    not a measurement, so it is dropped and the rear pole takes the 175 deg value. A band
    the maker did not measure is all zeros (the 4410A file's first three, below 40 Hz) and
    is left out of the output;
  - in the numeric header, four 27-entry per-band lists (one per measured band, 40 Hz up):
    the -6 dB coverage half-angle in three planes, 180 where the band never drops 6 dB,
    plus the maker's own directivity index (DI, dB).

The coverage lists are the self-check: the script recomputes the -6 dB half-angle of the
balloon it decoded, in two planes, and refuses to write unless it agrees with the file's
own lists (either plane within 12 deg of one of the three lists) in at least 12 of the 15
bands that have one, and unless the measured-band count equals the lists' length. Those
two catch a wrong band assignment, a wrong angular stride, a wrong axis, and a balloon
found one band early in a zero region. The DI is printed beside the balloon's own integral
for a reader; the maker's figure runs 1 to 2 dB under a plain solid-angle integral of the
balloon, so it is not used as a gate. `--no-check` bypasses the agreement gate only.

WHAT THE ENGINE TAKES from it: an AXISYMMETRIC loss curve per band, the power mean over the
72 meridians. The layout knows each speaker's aim but not its roll about that axis, so a
mean is the only table it can apply. Its cost: the 4410A's two principal planes agree within
2 dB over most of the range but differ by up to 6 dB around 2 to 2.5 kHz, where the woofer
beams just under the 2.9 kHz crossover, so a box rolled 90 deg on its mount is up to 3 dB
off the mean in that band. The two polars are written alongside for a reader, under
`planes`; the engine ignores them.
"""
import argparse
import json
import math
import pathlib
import struct
import sys

# The file's balloon layout. Established from the Genelec 4410A file (see the module docstring)
# and asserted against the file's own coverage lists before anything is written.
BANDS_HZ = [20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000,
            1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000]
N_MERID = 72          # 5 deg steps of rotation about the axis
N_POLAR = 37          # 0..180 deg off the axis in 5 deg steps; the last entry is a placeholder
POLAR_STEP_DEG = 5
COVERAGE_TOL_DEG = 12
COVERAGE_MIN_AGREE = 12


def read_floats(data, offset, count):
    return list(struct.unpack_from("<%df" % count, data, offset))


def find_balloon(data):
    """Yield every offset where a (30 x 72 x 37) float32 balloon could start: a zero at every
    37th slot, a near-0 dB on-axis entry per meridian, and relative-dB values elsewhere. An
    unmeasured band is all zeros and passes trivially, so a zero region ahead of the real
    balloon matches too; decode() walks the candidates and keeps the one whose measured
    bands are exactly the file's own per-band lists. Scans 4-byte aligned offsets."""
    need = len(BANDS_HZ) * N_MERID * N_POLAR
    for off in range(0, len(data) - 4 * need + 1, 4):
        ok = True
        for b in range(len(BANDS_HZ)):
            base = off + 4 * b * N_MERID * N_POLAR
            for m in (0, 17, 35, 53):
                row = base + 4 * m * N_POLAR
                if abs(struct.unpack_from("<f", data, row)[0]) > 1.0:               # on-axis ~ 0 dB
                    ok = False
                    break
                if struct.unpack_from("<f", data, row + 4 * (N_POLAR - 1))[0] != 0.0:  # placeholder
                    ok = False
                    break
                v = struct.unpack_from("<f", data, row + 4 * 6)[0]                    # 30 deg
                if not (-60.0 <= v <= 6.0):
                    ok = False
                    break
            if not ok:
                break
        if ok:
            yield off


def find_coverage_lists(data, balloon_off):
    """The maker's -6 dB coverage half-angle lists: 27 floats starting with twelve 180.0 (the
    omnidirectional LF bands) followed by 15 angles in (0, 180]. Returns every distinct list."""
    pat = struct.pack("<12f", *([180.0] * 12))
    out = []
    i = 0
    while True:
        j = data.find(pat, i)
        if j < 0 or j >= balloon_off:
            break
        vals = read_floats(data, j, 27)
        if all(0.0 < v <= 180.0 for v in vals[12:]) and vals not in out:
            out.append(vals)
        i = j + 4
    return out


def find_di_table(data, balloon_off):
    """The per-band DI list in the numeric header: 27 consecutive float32 values that start
    below 3 dB, end above 6 dB, and never leave [-3, 30]. Informational only."""
    for off in range(0, balloon_off - 4 * 27, 4):
        v = read_floats(data, off, 27)
        if 0.0 < v[0] <= 3.0 and v[-1] > 6.0 and all(-3.0 <= x <= 30.0 for x in v) \
                and all(x != 0.0 for x in v):
            return v
    return None


def decode_at(data, off):
    """The balloon at `off` as (bands, balloon), or None if it is not a balloon we trust: the
    unmeasured (all-zero) bands must be LEADING, and at least one band must be measured."""
    balloon = []                       # [band][merid][polar] dB, 36 polar entries kept
    bands = []
    for b in range(len(BANDS_HZ)):
        base = off + 4 * b * N_MERID * N_POLAR
        band = []
        for m in range(N_MERID):
            row = read_floats(data, base + 4 * m * N_POLAR, N_POLAR)
            band.append(row[:N_POLAR - 1])
        if all(v == 0.0 for r in band for v in r):
            if bands:
                return None            # a hole after measured bands: this is not the balloon
            continue                   # unmeasured LF band: nothing to compensate, leave it out
        balloon.append(band)
        bands.append(BANDS_HZ[b])
    return (bands, balloon) if bands else None


def decode(data):
    """Walk the candidate offsets and keep the first whose measured-band count equals the length
    of the file's own per-band lists (27 on the 4410A). A candidate one band early (a zero region
    ahead of the balloon) decodes to a different count, or to a hole, and is skipped. With no
    lists in the file the first plausible candidate is used and the self-check reports that."""
    lists_len = None
    first = None
    for off in find_balloon(data):
        got = decode_at(data, off)
        if got is None:
            continue
        bands, balloon = got
        if lists_len is None:
            lists = find_coverage_lists(data, off)
            lists_len = len(lists[0]) if lists else 0
        if lists_len == 0 or len(bands) == lists_len:
            return off, bands, balloon
        if first is None:
            first = (off, bands, balloon)
    if first is not None:
        off, bands, balloon = first
        raise SystemExit("clf_to_json: the balloon at byte %d has %d measured bands but the file's "
                         "per-band lists have %d entries; the band assignment is not trusted"
                         % (off, len(bands), lists_len))
    raise SystemExit("clf_to_json: no balloon block found (not a CF2 of the expected shape)")


def half_angle_6db(band, m):
    """Degrees off axis where meridian m of a band first drops to -6 dB (linear between the
    5 deg samples); 180 when it never does."""
    prev = band[m][0]
    for p in range(1, N_POLAR - 1):
        v = band[m][p]
        if v <= -6.0:
            f = (-6.0 - prev) / (v - prev) if v != prev else 0.0
            return POLAR_STEP_DEG * (p - 1 + f)
        prev = v
    return 180.0


def power_mean_db(values):
    return 10.0 * math.log10(sum(10.0 ** (v / 10.0) for v in values) / len(values))


def directivity_index_db(band):
    """DI = 10 log10(4 pi / integral of the normalized power over the sphere), the balloon
    sampled on a polar grid: each cell weighted by the solid angle of its ring segment. The
    rear pole is not stored, so it takes the 175 deg ring's mean."""
    num = 0.0
    den = 0.0
    step = math.radians(POLAR_STEP_DEG)
    for p in range(N_POLAR - 1):
        theta = math.radians(p * POLAR_STEP_DEG)
        lo = max(theta - step / 2, 0.0)
        hi = min(theta + step / 2, math.pi)
        w = (math.cos(lo) - math.cos(hi)) / N_MERID
        for m in range(N_MERID):
            num += w
            den += w * 10.0 ** (band[m][p] / 10.0)
    rear = power_mean_db([band[m][N_POLAR - 2] for m in range(N_MERID)])
    w = math.cos(math.pi - step / 2) - math.cos(math.pi)
    num += w
    den += w * 10.0 ** (rear / 10.0)
    return 10.0 * math.log10(num / den)


def build_block(bands, balloon, model, source, split_hz):
    angles = list(range(0, 181, POLAR_STEP_DEG))
    loss = []
    for band in balloon:
        row = [power_mean_db([band[m][p] for m in range(N_MERID)]) for p in range(N_POLAR - 1)]
        row.append(row[-1])           # 180 deg: the placeholder is dropped, the 175 ring stands in
        on_axis = row[0]              # re-reference to on-axis exactly (the file is within 0.05 dB)
        loss.append([round(v - on_axis, 2) for v in row])

    def polar(m0, m1):
        # one plane through the axis: meridian m0 for 0..175, and its opposite m1 supplies 180
        out = []
        for band in balloon:
            row = [round(band[m0][p], 2) for p in range(N_POLAR - 1)]
            row.append(round(band[m1][N_POLAR - 2], 2))
            out.append(row)
        return out

    return {
        "model": model,
        "source": source,
        "convention": "loss_db[band][angle]: dB relative to on-axis, angle = degrees off the "
                      "acoustic axis, power mean over rotation about the axis",
        "bands_hz": bands,
        "angles_deg": angles,
        "split_hz": split_hz,
        "loss_db": loss,
        "planes": {
            "note": "the two principal polars of the balloon (meridians 0/180 and 90/270), same "
                    "shape as loss_db; informational, the engine reads loss_db only",
            "a_db": polar(0, 36),
            "b_db": polar(18, 54),
        },
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("cf2", help="the vendor's .cf2 file")
    ap.add_argument("-o", "--out", help="write a standalone directivity JSON here")
    ap.add_argument("--into", help="merge the block into this layout JSON, in place")
    ap.add_argument("--model", help="model name recorded in the block (default: from the file)")
    ap.add_argument("--split-hz", type=float, default=1000.0,
                    help="the two-band split the engine compensates with (default 1000)")
    ap.add_argument("--no-check", action="store_true",
                    help="write even if the decoded balloon disagrees with the file's coverage lists")
    args = ap.parse_args()
    if not args.out and not args.into:
        ap.error("give -o <file> and/or --into <layout.json>")

    data = pathlib.Path(args.cf2).read_bytes()
    off, bands, balloon = decode(data)
    header = data[:off].decode("latin-1")
    model = args.model
    if not model:
        for cand in ("4410A", "4010A", "8331A", "8010A", "8020D", "8030C", "8040B", "8050B"):
            if cand in header:
                model = "Genelec " + cand
                break
        model = model or pathlib.Path(args.cf2).stem
    source = "%s (CLF v2 balloon, 1/3 octave, 5 deg), decoded by tools/directivity/clf_to_json.py" \
             % pathlib.Path(args.cf2).name
    print("clf_to_json: %s, balloon at byte %d, %d measured bands (%g .. %g Hz)"
          % (model, off, len(bands), bands[0], bands[-1]))

    # The self-check: the maker's -6 dB half-angle lists against the decoded balloon's, per band.
    lists = find_coverage_lists(data, off)
    di_file = find_di_table(data, off)
    print("  band_hz   half-angle A/B (decoded)   maker's lists      DI(balloon)  DI(maker)")
    agree = 0
    with_list = 0
    for i, hz in enumerate(bands):
        band = balloon[i]
        ha = half_angle_6db(band, 0)
        hb = half_angle_6db(band, 18)
        di = directivity_index_db(band)
        maker = [lst[i] for lst in lists] if lists and i < 27 else []
        real = [v for v in maker if v < 180.0]
        if real and (ha < 180.0 or hb < 180.0):
            with_list += 1
            if min(abs(ha - v) for v in real) <= COVERAGE_TOL_DEG \
                    or min(abs(hb - v) for v in real) <= COVERAGE_TOL_DEG:
                agree += 1
        dif = di_file[i] if di_file and i < len(di_file) else float("nan")
        print("  %6g       %5.1f / %5.1f            %-18s  %5.2f       %5.2f"
              % (hz, ha, hb, " ".join("%.0f" % v for v in maker) if maker else "-", di, dif))
    if not lists:
        print("  WARNING: no coverage lists found in the header; the band/axis assignment is unverified")
        if not args.no_check:
            raise SystemExit("clf_to_json: refusing to write without the self-check (--no-check overrides)")
    elif agree < COVERAGE_MIN_AGREE:
        print("  coverage self-check: %d of %d bands agree within %d deg (need %d)"
              % (agree, with_list, COVERAGE_TOL_DEG, COVERAGE_MIN_AGREE))
        if not args.no_check:
            raise SystemExit("clf_to_json: decoded balloon does not reproduce the file's coverage lists; "
                             "the band or angle assignment is wrong for this file (--no-check overrides)")
    else:
        print("  coverage self-check passed: %d of %d bands agree within %d deg"
              % (agree, with_list, COVERAGE_TOL_DEG))

    block = build_block(bands, balloon, model, source, args.split_hz)
    print("  -6 dB half-angle (axisymmetric mean):")
    for hz in (500, 1000, 2000, 4000, 8000, 16000):
        if hz not in bands:
            continue
        row = block["loss_db"][bands.index(hz)]
        ang = next((block["angles_deg"][p] for p, v in enumerate(row) if v <= -6.0), None)
        print("    %5d Hz: %s" % (hz, ("%d deg" % ang) if ang is not None else "> 180 deg"))

    if args.out:
        pathlib.Path(args.out).write_text(json.dumps(block, indent=1) + "\n", newline="\n")
        print("clf_to_json: wrote %s" % args.out)
    if args.into:
        p = pathlib.Path(args.into)
        layout = json.loads(p.read_text())
        layout["directivity"] = block
        p.write_text(json.dumps(layout, indent=2) + "\n", newline="\n")
        print("clf_to_json: merged into %s (directivity block; everything else untouched)" % args.into)


if __name__ == "__main__":
    sys.exit(main())
