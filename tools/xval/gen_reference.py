#!/usr/bin/env python3
"""Cross-validation reference generator (test/xval_data.h) — independent implementations
of the engine's core spatial-audio math, computed with numpy/scipy and emitted as golden
vectors for test/xval_test.c. The point is INDEPENDENCE: every reference below reaches the
same specified result through a different algorithm/library than the C code, so a shared
bug is implausible:

  SH encode  — scipy.special.lpmv associated Legendre (Condon-Shortley stripped) vs the
               engine's hardcoded polynomial encode (ambisonics.c).
  VBAP       — the l1-optimal linear program of Franck/Wang/Fazi 2017 (IEEE TASLP), solved
               with scipy.optimize.linprog/HiGHS: with non-negative gains its unique
               solution IS VBAP on the Delaunay triangulation — vs hull.c's geometric
               brute-force hull walk. A simplex solver validating a convex-hull search.
  AllRAD     — the full decode matrix rebuilt on scipy.spatial.ConvexHull (qhull) +
               numpy.linalg triangle solves + the scipy SH above (including the
               imaginary-pole-speaker rule) vs allrad.c + hull.c.
  RBJ biquad — the cookbook's ANALOG prototypes bilinear-transformed with pre-warping
               (scipy.signal.bilinear) vs biquad.h's cookbook difference-equation forms.
  align EQ   — scipy.signal.lfilter (float64) cascade golden vs align.c's DF-I float32
               room_eq rendering.
  EPAD       — the polar-factor decode D = c*Y^T(YY^T)^(-1/2) built with numpy.linalg.svd
               (Zotter/Pomberger/Noisternig 2012: D = c*VU^T) vs epad.c's Jacobi eigensolve
               of YY^T — two different factorizations of the same unique polar factor.
  SH rotation — the Ivanic-Ruedenberg recursion (ambisonics.c) vs a matrix RECOVERED from
               the defining property alone: encode(R*d) = M*encode(d) least-squares-solved
               over the scipy harmonics (numpy.linalg.lstsq) — no recursion in the loop, so
               a self-consistent sign/index slip in the recursion cannot hide (the engine's
               own ambi property test uses the engine's encode on both sides; this one
               never touches engine code).

No reference exists for DBAP/SPCAP (house designs: listener-relative direction weighting,
placement correction) or the MDAP ring parametrization (ring layout is an implementation
choice; its panner cores are validated here, its contract by the rt/dsp property tests).

The generator SELF-CHECKS against third-party constants (phonon's hardcoded orthonormal SH
table, via test/ambi_test.c's interop values) before writing anything.

Run:  python tools/xval/gen_reference.py   (writes test/xval_data.h; commit the result)
Deps: numpy, scipy. Deterministic (fixed seed) — reruns are byte-identical.
"""
import math
import os
import numpy as np
from scipy.optimize import linprog
from scipy.spatial import ConvexHull
from scipy.special import lpmv
from scipy.signal import bilinear, lfilter

OUT = os.path.join(os.path.dirname(__file__), "..", "..", "test", "xval_data.h")
AMBI_CH = 16          # 3rd order, (N+1)^2
CH = 26               # BWA_CHANNELS
RNG = np.random.default_rng(20260712)

# ---------------------------------------------------------------- real SN3D/ACN SH (AmbiX)
def sh_sn3d(dir_ambi):
    """Real SN3D/ACN spherical harmonics, AmbiX convention (no Condon-Shortley), 3rd order.
    dir_ambi: unit vector in AMBISONIC axes (x front, y left, z up)."""
    x, y, z = dir_ambi
    az = math.atan2(y, x)
    out = np.zeros(AMBI_CH)
    for n in range(AMBI_CH):
        l = int(math.floor(math.sqrt(n)))
        m = n - l * l - l
        am = abs(m)
        # scipy lpmv includes the Condon-Shortley (-1)^m phase; AmbiX SH do not — strip it.
        P = lpmv(am, l, z) * ((-1.0) ** am)
        N = math.sqrt((2.0 if am else 1.0) * math.factorial(l - am) / math.factorial(l + am))
        trig = math.cos(m * az) if m >= 0 else math.sin(am * az)
        out[n] = N * P * trig
    return out

def check_sh():
    """Assert against test/ambi_test.c's pinned values — including phonon's hardcoded
    orthonormal-SH constants (a third, independent source) via the N3D/sqrt(4pi) scale."""
    SQ3_2, S58, S38 = 0.8660254, 0.7905694, 0.6123724
    y = sh_sn3d((1, 0, 0))
    assert abs(y[0] - 1) < 1e-6 and abs(y[3] - 1) < 1e-6 and abs(y[1]) < 1e-9
    assert abs(y[6] + 0.5) < 1e-6 and abs(y[8] - SQ3_2) < 1e-6
    assert abs(y[13] + S38) < 1e-6 and abs(y[15] - S58) < 1e-6
    y = sh_sn3d((0, 1, 0))
    assert abs(y[1] - 1) < 1e-6 and abs(y[8] + SQ3_2) < 1e-6 and abs(y[9] + S58) < 1e-6
    y = sh_sn3d((0, 0, 1))
    assert abs(y[2] - 1) < 1e-6 and abs(y[6] - 1) < 1e-6 and abs(y[12] - 1) < 1e-6
    y = sh_sn3d((0.6666667, 0.3333333, 0.6666667))   # the m<0 sign-bug canary direction
    for acn, want in ((4, 0.3849002), (5, 0.3849002), (7, 0.7698004), (8, 0.2886751),
                      (10, 0.5737753), (11, 0.2494813), (14, 0.4303315)):
        assert abs(y[acn] - want) < 1e-5, (acn, y[acn], want)
    # phonon interop: SN3D * sqrt(2l+1)/sqrt(4pi) must hit phonon's table (ambi_test.c)
    y = sh_sn3d((1, 0, 0))
    s = lambda l: math.sqrt(2 * l + 1) / math.sqrt(4 * math.pi)
    for acn, l, want in ((0, 0, 0.282095), (3, 1, 0.488603), (6, 2, -0.315392), (8, 2, 0.546274)):
        assert abs(y[acn] * s(l) - want) < 1e-5, (acn, y[acn] * s(l), want)

# ---------------------------------------------------------------- layouts (mirror layout.c)
def default_grid():
    """layout_default(): 3x3x3 boundary grid minus the center, floor origin."""
    ax, ay = (-1.5, 0.0, 1.5), (0.0, 1.5, 3.0)
    pos = []
    for yi in range(3):
        for xi in range(3):
            for zi in range(3):
                if ax[xi] == 0.0 and ay[yi] == 1.5 and ax[zi] == 0.0:
                    continue
                pos.append((ax[xi], ay[yi], ax[zi]))
    assert len(pos) == CH
    return np.array(pos)

def unit_dirs(pos):
    ref = pos.mean(axis=0)
    d = pos - ref
    return d / np.linalg.norm(d, axis=1, keepdims=True), ref

def room_to_ambi(d):
    return np.array([d[2], d[0], d[1]])   # (z, x, y): matches build_bed_decode / allrad.c

# ---------------------------------------------------------------- VBAP via the l1 LP
def vbap_lp(spk_dirs, p):
    """min sum(g) s.t. sum g_i u_i = p, g >= 0 — Franck/Wang/Fazi 2017: the unique solution
    is the VBAP gain vector on the Delaunay triangulation. Returns the L2-normalized gains."""
    res = linprog(c=np.ones(len(spk_dirs)), A_eq=spk_dirs.T, b_eq=p,
                  bounds=(0, None), method="highs")
    assert res.status == 0, res.message
    g = res.x
    n = np.linalg.norm(g)
    assert n > 1e-9
    return g / n

def gen_vbap(spk_dirs, count=64):
    dirs, gains = [], []
    while len(dirs) < count:
        d = RNG.standard_normal(3)
        d /= np.linalg.norm(d)
        g = vbap_lp(spk_dirs, d)
        nz = g[g > 1e-7]
        if len(nz) > 3:                      # LP degeneracy (shouldn't happen): resample
            continue
        if np.any((g > 1e-7) & (g < 5e-3)):  # grazing a triangle edge: the active set is
            continue                         # tie-prone there — resample for a stable golden
        dirs.append(d)
        gains.append(g)
    return np.array(dirs), np.array(gains)

# ---------------------------------------------------------------- AllRAD via qhull + LP-VBAP
def fib_dirs_f32(M=240):
    """allrad.c's Fibonacci sphere, replicated in float32 so the virtual directions match."""
    i = np.arange(M, dtype=np.float32)
    y = np.float32(1.0) - np.float32(2.0) * (i + np.float32(0.5)) / np.float32(M)
    r = np.sqrt(np.maximum(np.float32(0.0), np.float32(1.0) - y * y))
    th = i * np.float32(2.39996323)
    return np.stack([r * np.cos(th), y, r * np.sin(th)], axis=1).astype(np.float64)

def hull_vbap_qhull(dirs_all, d):
    """VBAP d onto the array via qhull's triangulation + numpy solves: pick the simplex with
    the largest min gain (hull.c's selection rule), clamp, L2-normalize."""
    hull = ConvexHull(dirs_all)
    best, bg, bs = -1e30, None, None
    for simplex in hull.simplices:
        M = dirs_all[simplex].T              # columns = the triangle's speaker dirs
        try:
            g = np.linalg.solve(M, d)
        except np.linalg.LinAlgError:
            continue
        if g.min() > best:
            best, bg, bs = g.min(), g, simplex
    g = np.maximum(bg, 0.0)
    n = np.linalg.norm(g)
    return (g / n if n > 1e-12 else g), bs

def allrad_decode(pos):
    """allrad_build_decode replicated on qhull + scipy SH, including the imaginary-pole rule."""
    dirs, _ = unit_dirs(pos)
    N = len(dirs)
    dirs_all = dirs.copy()
    for pole in (-1.0, 1.0):                 # nadir then zenith (room +y up), cos(60) gap rule
        if np.max(pole * dirs[:, 1]) < 0.5:
            dirs_all = np.vstack([dirs_all, [0.0, pole, 0.0]])
    hull = ConvexHull(dirs_all)              # one hull for all virtual solves
    decode = np.zeros((N, AMBI_CH))
    l_of = np.array([int(math.floor(math.sqrt(k))) for k in range(AMBI_CH)])
    for d in fib_dirs_f32():
        best, bg, bs = -1e30, None, None
        for simplex in hull.simplices:
            M = dirs_all[simplex].T
            try:
                g = np.linalg.solve(M, d)
            except np.linalg.LinAlgError:
                continue
            if g.min() > best:
                best, bg, bs = g.min(), g, simplex
        g = np.maximum(bg, 0.0)
        n = np.linalg.norm(g)
        if n < 1e-12:
            continue
        g /= n
        row = (2 * l_of + 1) * sh_sn3d(room_to_ambi(d)) / 240.0
        for q in range(3):
            if bs[q] < N:                    # imaginary pole speaker: its share is discarded
                decode[bs[q]] += g[q] * row
    e_all = np.sum(decode**2 / (2 * l_of + 1))
    decode *= math.sqrt((AMBI_CH / N) / e_all)   # energy-normalize to the sampling decode
    return decode, len(dirs_all) - N

# ---------------------------------------------------------------- EPAD via numpy SVD
def epad_decode(pos):
    """epad.c rebuilt on numpy.linalg.svd: D = c*V.U^T over the kept singular values (the SVD
    form of the polar factor Y^T(YY^T)^(-1/2); epad.c reaches it through a Jacobi eigensolve of
    YY^T instead). N3D design basis, SN3D input rescale, sampling-decode diffuse normalization."""
    dirs, _ = unit_dirs(pos)
    N = len(dirs)
    l_of = np.array([int(math.floor(math.sqrt(k))) for k in range(AMBI_CH)])
    n3d = np.sqrt(2 * l_of + 1)
    Y = np.array([sh_sn3d(room_to_ambi(d)) for d in dirs]).T * n3d[:, None]   # (16, N), N3D
    U, S, Vh = np.linalg.svd(Y, full_matrices=False)                          # Y = U S Vh
    keep = S * S > 1e-6 * (S * S).max()          # epad.c's eigenvalue truncation, in sigma^2
    D = Vh[keep].T @ U[:, keep].T                # (N, 16): the polar factor on the kept subspace
    D = D * n3d[None, :]                         # accept SN3D signals
    e = np.sum(D**2 / (2 * l_of + 1))
    D *= math.sqrt((AMBI_CH / N) / e)            # energy-normalize to the sampling decode
    return D

# ---------------------------------------------------------------- SH rotation via lstsq
def rodrigues(axis, angle):
    a = np.asarray(axis, dtype=np.float64)
    a = a / np.linalg.norm(a)
    K = np.array([[0, -a[2], a[1]], [a[2], 0, -a[0]], [-a[1], a[0], 0]])
    return np.eye(3) + math.sin(angle) * K + (1.0 - math.cos(angle)) * (K @ K)

def rot_golden(R, dirs):
    """Recover the real-SH rotation matrix from its DEFINING property: encode(R*d) = M*encode(d)
    for all d, least-squares over well-spread directions — algorithm-independent of the
    Ivanic-Ruedenberg recursion. Returns ambisonics.c's 83-float packing (l=1..3 blocks,
    row-major); asserts block-diagonality and an exact fit before packing."""
    Y = np.array([sh_sn3d(d) for d in dirs])           # (n, 16)
    Yr = np.array([sh_sn3d(R @ d) for d in dirs])      # (n, 16)
    Mt, res, rank, _ = np.linalg.lstsq(Y, Yr, rcond=None)
    assert rank == AMBI_CH, "direction set does not span the SH space"
    M = Mt.T                                           # Yr[j] = M @ Y[j]
    assert np.max(np.abs(Y @ M.T - Yr)) < 1e-9, "rotation fit is not exact"
    for l in (0, 1, 2, 3):                             # rotation mixes only within one degree
        b, w = l * l, 2 * l + 1
        off = M[b:b + w].copy(); off[:, b:b + w] = 0.0
        assert np.max(np.abs(off)) < 1e-9, "rotation matrix is not block-diagonal"
    assert abs(M[0, 0] - 1.0) < 1e-9
    out = []
    for l in (1, 2, 3):
        b, w = l * l, 2 * l + 1
        out.extend(M[b + i, b + j] for i in range(w) for j in range(w))
    return np.array(out)                               # 9 + 25 + 49 = 83

def gen_rotations(dirs):
    """Deterministic rotation cases: identity, the two pure-axis conventions the engine's bed
    orientation exposes (yaw about ambi z = room up, tilt about ambi y), and 5 random axis-angle
    rotations. RNG draws happen AFTER every other consumer so the older goldens stay byte-stable."""
    cases = [np.eye(3), rodrigues((0, 0, 1), math.pi / 2), rodrigues((0, 1, 0), 0.4)]
    for _ in range(5):
        ax = RNG.standard_normal(3)
        ang = RNG.uniform(0.1, math.pi)
        cases.append(rodrigues(ax, ang))
    Rs = np.array([c.ravel() for c in cases])          # row-major 3x3, ambi axes
    Ms = np.array([rot_golden(c, dirs) for c in cases])
    return Rs, Ms

# ---------------------------------------------------------------- RBJ biquads via bilinear
def rbj_bilinear(kind, fc, q, gain_db, fs):
    """The cookbook's analog prototypes, bilinear-transformed with pre-warping so fc maps
    exactly (s_norm = (z-1)/(z+1) / tan(w0/2)) — an independent derivation of biquad.h."""
    A = 10.0 ** (gain_db / 40.0)
    if kind == 1:      # peak: H(s) = (s^2 + s A/Q + 1) / (s^2 + s/(A Q) + 1)
        b = [1.0, A / q, 1.0]
        a = [1.0, 1.0 / (A * q), 1.0]
    elif kind == 0:    # low shelf: A (s^2 + sqrt(A)/Q s + A) / (A s^2 + sqrt(A)/Q s + 1)
        b = [A, A * math.sqrt(A) / q, A * A]
        a = [A, math.sqrt(A) / q, 1.0]
    else:              # high shelf: A (A s^2 + sqrt(A)/Q s + 1) / (s^2 + sqrt(A)/Q s + A)
        b = [A * A, A * math.sqrt(A) / q, A]
        a = [1.0, math.sqrt(A) / q, A]
    w0 = 2.0 * math.pi * fc / fs
    bz, az = bilinear(b, a, fs=0.5 / math.tan(w0 / 2.0))
    bz, az = bz / az[0], az / az[0]
    return np.array([bz[0], bz[1], bz[2], az[1], az[2]])

def gen_biquads():
    cases, coefs = [], []
    for fs in (48000.0, 96000.0):
        for kind, fcs in ((1, (45.0, 100.0, 200.0, 3160.0)),   # peak: room_eq + occ mid band
                          (0, (800.0,)), (2, (8000.0,))):       # shelves: the occlusion EQ bands
            for q in (0.707, 2.0, 6.0):
                for g in (-12.0, -3.0, 6.0):
                    for fc in fcs:
                        cases.append((kind, fc, q, g, fs))
                        coefs.append(rbj_bilinear(kind, fc, q, g, fs))
    return np.array(cases), np.array(coefs)

# ---------------------------------------------------------------- align room_eq lfilter golden
LF_N = 4096
LF_SECTIONS = ((45.0, -8.0, 6.0), (120.0, -5.0, 2.0))   # the calib-test modal-cut shapes

def lcg_noise(n):
    """rt.c's test-signal LCG, so the C side regenerates the identical float32 input."""
    s = 1
    out = np.empty(n, dtype=np.float32)
    for i in range(n):
        s = (s * 1664525 + 1013904223) & 0xFFFFFFFF
        out[i] = np.float32((s >> 9) / 4194304.0 - 1.0)
    return out

def gen_lfilter():
    x = lcg_noise(LF_N).astype(np.float64)
    y = x
    for fc, g, q in LF_SECTIONS:
        c = rbj_bilinear(1, fc, q, g, 48000.0)
        y = lfilter([c[0], c[1], c[2]], [1.0, c[3], c[4]], y)
    return y

# ---------------------------------------------------------------- emit the header
def flit(v):
    s = f"{v:.9g}"
    if "." not in s and "e" not in s and "inf" not in s and "nan" not in s:
        s += ".0"                        # 0f / -1f are invalid C literals — force a decimal point
    return s + "f"

def farr(name, a, per=8):
    a = np.asarray(a, dtype=np.float32).ravel()
    rows = []
    for i in range(0, len(a), per):
        rows.append("    " + ", ".join(flit(v) for v in a[i:i + per]) + ",")
    return f"static const float {name}[{len(a)}] = {{\n" + "\n".join(rows) + "\n};\n"

def main():
    check_sh()

    pos26 = default_grid()
    dirs26, ref26 = unit_dirs(pos26)
    assert np.allclose(ref26, (0.0, 1.5, 0.0))

    # SH: quasi-random unit directions in AMBI axes
    sh_dirs = RNG.standard_normal((64, 3))
    sh_dirs /= np.linalg.norm(sh_dirs, axis=1, keepdims=True)
    sh_vals = np.array([sh_sn3d(d) for d in sh_dirs])

    # VBAP: LP golden on the default grid's listener-relative dirs; spot-check the LP against
    # the independent qhull VBAP too (two references must agree before we pin either)
    vb_dirs, vb_gains = gen_vbap(dirs26)
    for d, g in zip(vb_dirs[:16], vb_gains[:16]):
        gq3, spk = hull_vbap_qhull(dirs26, d)
        gq = np.zeros(CH)
        gq[spk] = gq3
        assert np.allclose(g, gq, atol=1e-6), "LP and qhull VBAP disagree"

    # AllRAD: full grid (no imaginary) + floor-less grid (nadir imaginary)
    dec26, nimag26 = allrad_decode(pos26)
    assert nimag26 == 0, "default grid should need no imaginary speaker"
    pos17 = pos26[pos26[:, 1] > 0.1]
    assert len(pos17) == 17
    dec17, nimag17 = allrad_decode(pos17)
    assert nimag17 == 1, "floor-less grid should add exactly the nadir imaginary speaker"

    # EPAD: same two grids as AllRAD (no RNG)
    epad26 = epad_decode(pos26)
    epad17 = epad_decode(pos17)

    # SH rotations: recovered from the defining property (RNG draws AFTER gen_vbap's,
    # so every older golden stays byte-identical)
    rot_R, rot_M = gen_rotations(sh_dirs)

    bq_cases, bq_coefs = gen_biquads()
    lf_out = gen_lfilter()

    with open(OUT, "w", newline="\n") as f:
        f.write("/* GENERATED by tools/xval/gen_reference.py — DO NOT EDIT.\n"
                " * Cross-validation golden vectors from independent implementations (scipy\n"
                " * lpmv spherical harmonics, linprog/HiGHS l1-panning, qhull AllRAD, bilinear\n"
                " * RBJ prototypes, lfilter). See the generator header for what validates what.\n"
                " * Regenerate: python tools/xval/gen_reference.py  (deterministic). */\n"
                "#ifndef BWA_XVAL_DATA_H\n#define BWA_XVAL_DATA_H\n\n")
        f.write(f"#define XVAL_SH_N {len(sh_dirs)}\n")
        f.write(farr("xval_sh_dir", sh_dirs))
        f.write(farr("xval_sh_val", sh_vals))
        f.write(f"\n#define XVAL_VBAP_N {len(vb_dirs)}\n")
        f.write(farr("xval_vbap_dir", vb_dirs))
        f.write(farr("xval_vbap_gain", vb_gains))
        f.write("\n/* AllRAD decode matrices, [speaker][ACN] row-major (SN3D) */\n")
        f.write(farr("xval_allrad_grid26", dec26))
        f.write(farr("xval_allrad_floorless17", dec17))
        f.write("\n/* EPAD decode matrices, [speaker][ACN] row-major (SN3D) — numpy SVD polar factor */\n")
        f.write(farr("xval_epad_grid26", epad26))
        f.write(farr("xval_epad_floorless17", epad17))
        f.write(f"\n#define XVAL_ROT_N {len(rot_R)}\n")
        f.write("/* SH rotations: R row-major 3x3 (ambi axes) + ambisonics.c's 83-float packed M\n"
                " * (l = 1..3 blocks, row-major), recovered from encode(R*d) = M*encode(d) by lstsq */\n")
        f.write(farr("xval_rot_R", rot_R))
        f.write(farr("xval_rot_M", rot_M))
        f.write(f"\n#define XVAL_BQ_N {len(bq_cases)}\n")
        f.write("/* per case: type(0 lowshelf/1 peak/2 highshelf), fc, Q, gain_db, fs */\n")
        f.write(farr("xval_bq_case", bq_cases))
        f.write("/* per case: b0 b1 b2 a1 a2 (a0-normalized) */\n")
        f.write(farr("xval_bq_coef", bq_coefs))
        f.write(f"\n#define XVAL_LF_N {LF_N}\n")
        f.write("/* align room_eq golden: rt.c's LCG noise (seed 1) through the two sections\n"
                " * {45 Hz -8 dB Q6, 120 Hz -5 dB Q2} at 48 kHz, rendered with scipy lfilter */\n")
        f.write(farr("xval_lf_out", lf_out))
        f.write("\n#endif /* BWA_XVAL_DATA_H */\n")
    print(f"wrote {os.path.normpath(OUT)}: {len(sh_dirs)} SH dirs, {len(vb_dirs)} VBAP dirs, "
          f"2 AllRAD + 2 EPAD matrices, {len(rot_R)} SH rotations, {len(bq_cases)} biquads, "
          f"{LF_N} lfilter samples")

if __name__ == "__main__":
    main()
