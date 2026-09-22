/**
 * Scene 6: Blind A/B/X.
 *
 * A and B are two settings of ONE engine knob. X is randomly one of them. Listen to all three as
 * long as you like, then say which X was. The one-sided binomial tail over your trials turns
 * "sounds different to me" into a measurement: p is the chance that guessing alone would have
 * scored at least this well, so p below 0.05 means you can actually hear it.
 *
 * These are exactly the judgments the automated suite cannot make. A test can prove the DBAP and
 * SPCAP solves differ numerically; only an ear can say whether the difference is a difference.
 *
 * Every switch is live and click-free (ramped gains, atomic toggles), and `applyListen` resets
 * every knob in the table to baseline before setting the tested one - so changing comparison
 * cannot leave the previous knob stuck on its B setting, which would silently confound the next
 * tally.
 *
 * The orbit is on by default and is IDENTICAL for A, B and X, so motion exposes the panners
 * without ever cueing which one is playing.
 */
const COMPARISONS = [
  {
    name: "dual-band panning", a: "single band (power)", b: "dual band (LF amplitude)",
    apply: (ctx, v) => ctx.set("set_dual_band", !!v),
  },
  {
    name: "panner: DBAP vs SPCAP", a: "DBAP", b: "SPCAP",
    apply: (ctx, v) => ctx.set("set_panner", v ? 1 : 0),
  },
  {
    name: "panner: DBAP vs VBAP", a: "DBAP", b: "VBAP",
    apply: (ctx, v) => ctx.set("set_panner", v ? 2 : 0),
  },
  {
    name: "source spread", a: "point source", b: "spread 60%",
    apply: (ctx, v) => ctx.set("source_set_spread", ctx.srcHandle, v ? 0.6 : 0.0),
  },
  {
    name: "spread render: MDAP", a: "LOBE (reshape)", b: "MDAP (virtual ring)",
    apply: (ctx, v) => {
      ctx.set("source_set_spread", ctx.srcHandle, 0.6);
      ctx.set("set_spread_mode", v ? 1 : 0);
    },
  },
  {
    name: "spread render: SPECTRAL", a: "LOBE (reshape)", b: "SPECTRAL (frequency-dependent pan)",
    apply: (ctx, v) => {
      ctx.set("source_set_spread", ctx.srcHandle, 0.6);
      ctx.set("set_spread_mode", v ? 2 : 0);
    },
  },
  {
    name: "decorrelation (wide source)", a: "coherent copies", b: "velvet-noise decorrelated",
    apply: (ctx, v) => {
      ctx.set("source_set_spread", ctx.srcHandle, 0.6);
      ctx.set("set_decorrelation", !!v);
    },
  },
  {
    name: "air absorption", a: "off", b: "on (distance low-pass)",
    apply: (ctx, v) => ctx.set("source_set_air_absorption", ctx.srcHandle, !!v),
  },
];

/** One-sided binomial tail: P(at least k correct in n, by guessing). */
export function pValue(n, k) {
  if (n <= 0) return 1;
  const logFact = new Float64Array(n + 1);
  for (let i = 2; i <= n; ++i) logFact[i] = logFact[i - 1] + Math.log(i);
  let p = 0;
  for (let i = k; i <= n; ++i) {
    p += Math.exp(logFact[n] - logFact[i] - logFact[n - i] - n * Math.LN2);
  }
  return Math.min(1, p);
}

export const abx = {
  id: "abx",
  name: "6. Blind A/B/X",
  blurb: "Two settings of one knob, one hidden X, and a p-value that says whether you heard it.",
  state: { cmp: 0, listen: 2, x: 0, trials: 0, correct: 0, lastX: -1, orbit: true, t: 0, flash: 0 },

  async enter(ctx) {
    const s = this.state;
    ctx.world.dragEnabled = true;
    ctx.world.setTrail(false);
    this.newTrial(ctx);
  },

  async exit(ctx) {
    this.baseline(ctx);
  },

  baseline(ctx) {
    ctx.set("set_dual_band", false);
    ctx.set("set_panner", 0);
    ctx.set("source_set_spread", ctx.srcHandle, 0.0);
    ctx.set("set_spread_mode", 0);
    ctx.set("set_decorrelation", false);
    ctx.set("source_set_air_absorption", ctx.srcHandle, false);
  },

  applyListen(ctx) {
    const s = this.state;
    this.baseline(ctx);
    COMPARISONS[s.cmp].apply(ctx, s.listen === 2 ? s.x : s.listen);
  },

  newTrial(ctx) {
    const s = this.state;
    s.x = Math.random() < 0.5 ? 0 : 1;
    s.listen = 2;
    this.applyListen(ctx);
  },

  reset(ctx) {
    const s = this.state;
    s.trials = 0;
    s.correct = 0;
    s.lastX = -1;
    this.newTrial(ctx);
  },

  answer(ctx, guess) {
    const s = this.state;
    s.lastX = s.x;
    s.flash = guess === s.x ? 1 : -1;
    if (guess === s.x) s.correct++;
    s.trials++;
    this.newTrial(ctx);
  },

  update(ctx, dt) {
    const s = this.state;
    if (s.orbit) {
      s.t += dt;
      const az = 0.45 * s.t;
      ctx.sourceRoom = [
        2.2 * Math.cos(az),
        ctx.headRoom[1] + 0.5 * Math.sin(0.31 * s.t),
        2.2 * Math.sin(az),
      ];
    }
  },

  controls(ctx) {
    const s = this.state;
    const c = COMPARISONS[s.cmp];
    const re = () => ctx.refresh();
    return [
      {
        kind: "select", label: "comparison", options: COMPARISONS.map((x) => x.name),
        get: () => s.cmp,
        set: (v) => { s.cmp = v; this.reset(ctx); re(); },
        hint: "A new knob starts a fresh tally: a p-value pooled over two different questions means nothing.",
      },
      {
        kind: "buttons", label: "listen to", items: [
          { label: `A: ${c.a}`, onClick: () => { s.listen = 0; this.applyListen(ctx); re(); } },
          { label: `B: ${c.b}`, onClick: () => { s.listen = 1; this.applyListen(ctx); re(); } },
          { label: "X (hidden)", onClick: () => { s.listen = 2; this.applyListen(ctx); re(); } },
        ],
      },
      {
        kind: "buttons", label: "X was", items: [
          { label: "A", onClick: () => { this.answer(ctx, 0); re(); } },
          { label: "B", onClick: () => { this.answer(ctx, 1); re(); } },
        ],
        hint: "Answering reveals the last X and immediately deals the next one.",
      },
      {
        kind: "buttons", label: "", items: [
          { label: "reset the tally", onClick: () => { this.reset(ctx); re(); } },
        ],
      },
      {
        kind: "toggle", label: "orbit the source",
        get: () => s.orbit,
        set: (v) => { s.orbit = v; },
        hint: "The same orbit for A, B and X, so it exposes the knob and never cues it.",
      },
    ];
  },

  readout(ctx) {
    const s = this.state;
    const p = pValue(s.trials, s.correct);
    return [
      ["now playing", s.listen === 2 ? "X (hidden)" : s.listen === 0 ? "A" : "B"],
      ["last X was", s.lastX < 0 ? "-" : s.lastX === 0 ? "A" : "B"],
      ["last answer", s.flash === 0 ? "-" : s.flash > 0 ? "correct" : "wrong"],
      ["score", `${s.correct} of ${s.trials}`],
      ["p (one sided binomial)", s.trials ? p.toFixed(4) : "-"],
      ["verdict", s.trials < 5 ? "not enough trials" : p < 0.05 ? "you can hear it (p < 0.05)" : "consistent with guessing"],
    ];
  },
};
