/**
 * Scene 4: Directivity.
 *
 * A source that does not radiate equally in every direction. Turn it away from you and the level
 * drops by the weighted-dipole law, not by distance. The orange outline is that law drawn at the
 * source's current aim; `bwa_source_get_directivity` under the readout is the gain the engine is
 * really applying, which is the number the outline is a picture of.
 *
 * The aim is a quaternion about the room's up axis, and a source's forward is +z in its own frame,
 * the same convention the listener has (BWA_ROOM_AHEAD).
 */
const PRESETS = ["omni", "cardioid", "figure-8"];
const SEGMENTS = 64;

export const directivity = {
  id: "directivity",
  name: "4. Directivity",
  blurb: "Aim the source. Off-axis is quieter by the radiation pattern, not by distance.",
  state: { preset: 1, yaw: 0, spin: true, gain: 1, applied: -1 },

  async enter(ctx) {
    const s = this.state;
    ctx.world.dragEnabled = true;
    ctx.sourceRoom = [1.2, ctx.headRoom[1], 1.6];
    this.lobe = ctx.world.addLine(0xffb050, SEGMENTS + 1);
    this.aim = ctx.world.addLine(0xffb050, 2);
    s.applied = -1;
  },

  async exit(ctx) {
    ctx.set("source_set_directivity_preset", ctx.srcHandle, 0);
    this.lobe = null;
    this.aim = null;
  },

  update(ctx, dt) {
    const s = this.state;
    /* The pattern is pushed from UPDATE and not from the control, so the scene CONVERGES to its
     * state rather than to the history of which setter was called. The headless check drives this
     * state directly, and a scene that only reacted to a DOM event would leave it testing a stale
     * engine while reading a fresh number. */
    if (s.preset !== s.applied) {
      ctx.set("source_set_directivity_preset", ctx.srcHandle, s.preset);
      s.applied = s.preset;
    }
    if (s.spin) s.yaw += dt * 0.7;
    const half = s.yaw * 0.5;
    /* Rotation about the room's up axis: q = (0, sin(yaw/2), 0, cos(yaw/2)). */
    ctx.set("source_set_orientation", ctx.srcHandle, 0, Math.sin(half), 0, Math.cos(half));
    ctx.rig.engine?.invoke("source_get_directivity", ctx.srcHandle)
       .then((v) => { s.gain = v; })
       .catch(() => {});

    /* The weighted dipole the presets name: |(1-w) + w cos(theta)|, w = 0 omni, 0.5 cardioid,
     * 1 figure-8. Drawn in the horizontal plane through the source. */
    const w = s.preset === 1 ? 0.5 : s.preset === 2 ? 1.0 : 0.0;
    const [sx, sy, sz] = ctx.sourceRoom;
    const pts = [];
    for (let i = 0; i <= SEGMENTS; ++i) {
      const a = (i / SEGMENTS) * Math.PI * 2;
      const g = Math.abs((1 - w) + w * Math.cos(a));
      const r = 0.15 + 0.85 * g;
      const wa = s.yaw + a;
      pts.push([sx + r * Math.sin(wa), sy, sz + r * Math.cos(wa)]);
    }
    ctx.world.setLinePoints(this.lobe, pts);
    ctx.world.setLinePoints(this.aim, [
      [sx, sy, sz],
      [sx + 1.1 * Math.sin(s.yaw), sy, sz + 1.1 * Math.cos(s.yaw)],
    ]);
  },

  controls(ctx) {
    const s = this.state;
    return [
      {
        kind: "select", label: "pattern", options: PRESETS,
        get: () => s.preset,
        set: (v) => { s.preset = v; },
      },
      {
        kind: "toggle", label: "spin the source",
        get: () => s.spin,
        set: (v) => { s.spin = v; },
      },
      {
        kind: "slider", label: "aim (yaw)", min: -Math.PI, max: Math.PI, step: 0.01,
        get: () => Math.atan2(Math.sin(s.yaw), Math.cos(s.yaw)),
        set: (v) => { s.yaw = v; s.spin = false; },
        format: (v) => `${((v * 180) / Math.PI).toFixed(0)} deg`,
      },
    ];
  },

  readout(ctx) {
    const s = this.state;
    return [
      ["pattern", PRESETS[s.preset]],
      ["directivity gain", `${s.gain.toFixed(3)} (1 = on axis)`],
      ["that in dB", `${(20 * Math.log10(Math.max(s.gain, 1e-4))).toFixed(1)} dB`],
      ["aim (yaw)", `${((s.yaw * 180) / Math.PI % 360).toFixed(0)} deg`],
    ];
  },
};
