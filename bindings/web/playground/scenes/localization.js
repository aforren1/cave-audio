/**
 * Scene 1: Localization.
 *
 * One click, one listener, and the panner that puts the click where the click is. Drag the red
 * marker, or let it orbit. The knobs are the ones that change WHERE it sounds rather than what it
 * sounds like: the panner itself, the source's angular size, dual-band panning, and the two
 * distance effects.
 *
 * The speaker gizmos light from `bwa_get_bus_levels` in the CAVE_SIM profile, so the bright cones
 * are the channels the panner really solved for. In BINAURAL there is no bus and they stay dark,
 * which is correct: point voices bypass the panner there.
 */
const PANNERS = ["DBAP", "SPCAP", "VBAP"];

export const localization = {
  id: "localization",
  name: "1. Localization",
  blurb: "A click orbiting your head. Drag it, or let it fly. The array solves the pan every block.",
  state: {
    auto: true,
    flyby: false,
    t: 0,
    spread: 0,
    panner: 0,
    focus: 0,
    density: 0,
    dualBand: false,
    doppler: false,
    air: false,
  },

  async enter(ctx) {
    const s = this.state;
    ctx.world.setTrail(true);
    ctx.world.dragEnabled = true;
    ctx.sourceRoom = [0, ctx.headRoom[1], 2.0];
    await this.apply(ctx);
  },

  async apply(ctx) {
    const s = this.state;
    ctx.set("set_panner", s.panner);
    ctx.set("set_spcap_focus", s.focus, s.density);
    ctx.set("set_dual_band", s.dualBand);
    ctx.set("source_set_spread", ctx.srcHandle, s.spread);
    ctx.set("source_set_doppler", ctx.srcHandle, s.doppler);
    ctx.set("source_set_air_absorption", ctx.srcHandle, s.air);
  },

  async exit(ctx) {
    ctx.world.setTrail(false);
    /* Leave the engine at its defaults so the next scene starts from a known place. */
    ctx.set("set_panner", 0);
    ctx.set("set_dual_band", false);
    ctx.set("source_set_spread", ctx.srcHandle, 0);
    ctx.set("source_set_doppler", ctx.srcHandle, false);
    ctx.set("source_set_air_absorption", ctx.srcHandle, false);
  },

  update(ctx, dt) {
    const s = this.state;
    if (s.flyby) {
      s.t += dt;
      const period = 3.6;
      const u = (s.t % period) / period;                 /* there and back, about 7.8 m/s */
      const x = u < 0.5 ? -7 + 28 * u : 7 - 28 * (u - 0.5);
      ctx.sourceRoom = [x, ctx.headRoom[1], 0.8];
    } else if (s.auto) {
      s.t += dt;
      /* Three incommensurate periods, so the source eventually visits the whole space rather than
       * retracing one circle: azimuth ~10 s, radius ~7 s, height ~5.5 s. */
      const az = 0.62 * s.t;
      const r = 2.0 + 1.2 * Math.sin(0.9 * s.t);
      const y = 1.2 * Math.sin(1.14 * s.t);
      ctx.sourceRoom = [r * Math.cos(az), ctx.headRoom[1] + y, r * Math.sin(az)];
    }
    if (s.auto || s.flyby) ctx.world.pushTrail();
  },

  controls(ctx) {
    const s = this.state;
    const re = () => ctx.refresh();
    return [
      {
        kind: "buttons", label: "motion", items: [
          { label: "orbit", onClick: () => { s.auto = true; s.flyby = false; s.t = 0; ctx.world.setTrail(true); re(); } },
          { label: "fly-by", onClick: () => { s.flyby = true; s.auto = false; s.t = 0; ctx.world.setTrail(true); re(); } },
          { label: "hold", onClick: () => { s.auto = false; s.flyby = false; ctx.world.setTrail(false); ctx.world.setTrail(true); re(); } },
        ],
        hint: "Or drag the red marker in the view.",
      },
      {
        kind: "select", label: "panner", options: PANNERS,
        get: () => s.panner,
        set: (v) => { s.panner = v; ctx.set("set_panner", v); re(); },
        hint: "DBAP is the listener-relative default. SPCAP is the smooth all-speaker solve; VBAP hands the source to one hull triangle.",
      },
      {
        kind: "slider", label: "SPCAP focus", min: 0, max: 40, step: 0.5,
        get: () => s.focus,
        set: (v) => { s.focus = v; ctx.set("set_spcap_focus", v, s.density); },
        format: (v) => (v <= 0 ? "geometry default" : v.toFixed(1)),
        hint: "Inert under DBAP and VBAP: neither has a lobe to sharpen.",
      },
      {
        kind: "slider", label: "spread", min: 0, max: 1, step: 0.01,
        get: () => s.spread,
        set: (v) => { s.spread = v; ctx.set("source_set_spread", ctx.srcHandle, v); },
        format: (v) => `${Math.round(v * 100)}%`,
        hint: "0 is a point. Above that the source grows an angular size and the solve widens.",
      },
      {
        kind: "toggle", label: "dual-band panning",
        get: () => s.dualBand,
        set: (v) => { s.dualBand = v; ctx.set("set_dual_band", v); },
        hint: "Amplitude panning below the crossover, power above it.",
      },
      {
        kind: "toggle", label: "Doppler",
        get: () => s.doppler,
        set: (v) => { s.doppler = v; ctx.set("source_set_doppler", ctx.srcHandle, v); },
        hint: "Hear it on the fly-by, not on the orbit: an orbit has almost no radial speed.",
      },
      {
        kind: "toggle", label: "air absorption",
        get: () => s.air,
        set: (v) => { s.air = v; ctx.set("source_set_air_absorption", ctx.srcHandle, v); },
        hint: "Distance eats treble, so far reads as far and not only as quiet.",
      },
    ];
  },

  readout(ctx) {
    const [x, y, z] = ctx.sourceRoom;
    const d = Math.hypot(x - ctx.headRoom[0], y - ctx.headRoom[1], z - ctx.headRoom[2]);
    const { channel, peak } = ctx.rig.loudestChannel();
    return [
      ["source (room x,y,z)", `${x.toFixed(2)}, ${y.toFixed(2)}, ${z.toFixed(2)} m`],
      ["side", x > 0.05 ? "left of the listener" : x < -0.05 ? "right of the listener" : "centered"],
      ["distance", `${d.toFixed(2)} m`],
      ["panner", PANNERS[this.state.panner]],
      ["loudest bus channel", channel < 0 ? "-" : `${channel} (${(20 * Math.log10(Math.max(peak, 1e-6))).toFixed(1)} dBFS)`],
    ];
  },
};
