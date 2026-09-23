/**
 * Scene 2: Channel walk.
 *
 * The rig check, on a desk. `bwa_set_test_signal` drives ONE bus channel directly, past every
 * spatial stage, and `bwa_get_bus_levels` reads back what each channel actually received at the
 * very end of the render - after align, after the test signal, after the limiter. So the gizmo
 * that lights is the channel that was driven, and if it is not, something between the panner and
 * the device is wrong. On the rig you walk the room and point at the speaker you hear. Here the
 * CAVE_SIM decode puts that one virtual speaker on your headphones, at its surveyed direction.
 *
 * The stimulus source is muted while this scene runs, because the point is one channel at a time.
 */
const KINDS = ["sine", "noise"];

export const channelWalk = {
  id: "channel-walk",
  name: "2. Channel walk",
  blurb: "One speaker at a time, straight onto its bus channel. The lit cone is the channel the engine reports.",
  needsBus: true,
  state: { channel: 0, kind: 0, auto: true, timer: 0, gain: 0.25, applied: -1, appliedKind: -1 },

  async enter(ctx) {
    ctx.world.setTrail(false);
    ctx.world.dragEnabled = false;
    ctx.world.source.visible = false;
    ctx.world.link.visible = false;
    ctx.set("source_set_gain", ctx.srcHandle, 0);     /* only the test tone sounds here */
    this.state.applied = -1;
    this.state.appliedKind = -1;
  },

  async exit(ctx) {
    const s = this.state;
    if (s.applied >= 0) ctx.set("set_test_signal", s.applied, 0, 0);
    s.applied = -1;
    ctx.world.source.visible = true;
    ctx.world.link.visible = true;
    ctx.world.dragEnabled = true;
    ctx.set("source_set_gain", ctx.srcHandle, 0.9);
  },

  update(ctx, dt) {
    const s = this.state;
    const n = Math.max(1, ctx.rig.speakerCount);
    if (s.auto) {
      s.timer += dt;
      if (s.timer >= 0.9) { s.timer = 0; s.channel = (s.channel + 1) % n; }
    }
    if (s.channel >= n) s.channel = 0;
    if (s.channel !== s.applied || s.kind !== s.appliedKind) {
      /* Off THEN on, always in that order: two channels driven at once is the one state this
       * scene must never show, because it makes a wiring fault unreadable. */
      if (s.applied >= 0) ctx.set("set_test_signal", s.applied, 0, 0);
      ctx.set("set_test_signal", s.channel, s.kind === 0 ? 1 : 2, s.gain);
      s.applied = s.channel;
      s.appliedKind = s.kind;
    }
    ctx.highlight = s.channel;
    const p = ctx.rig.speakers;
    ctx.sourceRoom = [p[s.channel * 3], p[s.channel * 3 + 1], p[s.channel * 3 + 2]];
  },

  controls(ctx) {
    const s = this.state;
    const n = Math.max(1, ctx.rig.speakerCount);
    return [
      {
        kind: "toggle", label: "walk the array automatically",
        get: () => s.auto,
        set: (v) => { s.auto = v; s.timer = 0; },
        hint: "One channel every 0.9 s, in index order.",
      },
      {
        kind: "slider", label: "channel", min: 0, max: n - 1, step: 1,
        get: () => s.channel,
        set: (v) => { s.channel = v; s.auto = false; ctx.refresh(); },
        format: (v) => `${v} of ${n - 1}`,
      },
      {
        kind: "select", label: "test signal", options: KINDS,
        get: () => s.kind,
        set: (v) => { s.kind = v; },
        hint: "A sine finds a dead channel. Noise finds a channel wired to the wrong place, because you can localize it.",
      },
      {
        kind: "slider", label: "level", min: 0, max: 0.5, step: 0.01,
        get: () => s.gain,
        set: (v) => { s.gain = v; s.appliedKind = -1; },
        format: (v) => `${(20 * Math.log10(Math.max(v, 1e-6))).toFixed(1)} dBFS`,
      },
      {
        kind: "note",
        text: "This scene needs the array bus, so it runs in the CAVE_SIM profile whatever the " +
              "page is set to. The binaural profile has no speakers to walk.",
      },
    ];
  },

  readout(ctx) {
    const s = this.state;
    const { channel, peak } = ctx.rig.loudestChannel();
    const p = ctx.rig.speakers;
    const pos = `${p[s.channel * 3]?.toFixed(2)}, ${p[s.channel * 3 + 1]?.toFixed(2)}, ${p[s.channel * 3 + 2]?.toFixed(2)}`;
    return [
      ["driven channel", `${s.channel}`],
      ["its position (room)", `${pos} m`],
      ["loudest bus channel", channel < 0 ? "-" : `${channel}`],
      ["agrees", channel === s.channel ? "yes" : "NO"],
      ["peak", `${(20 * Math.log10(Math.max(peak, 1e-6))).toFixed(1)} dBFS`],
      ["channels in the layout", `${ctx.rig.speakerCount}`],
    ];
  },
};
