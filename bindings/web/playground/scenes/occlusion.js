/**
 * Scene 3: Occlusion and materials.
 *
 * A wall between you and the source, registered with the engine as a DYNAMIC mesh with an acoustic
 * material, and the engine's ray-traced occlusion turned on for the source. Slide the wall through
 * the line of sight and the source does not just get quieter: the material's per-band transmission
 * is rendered as a 3-biquad EQ, so concrete muffles and glass mostly does not.
 *
 * A dynamic mesh and not a static one on purpose. A static mesh rebuilds the whole scene BVH on
 * every move; a dynamic one is a rigid instance whose transform is a cheap refit, which is what a
 * wall that slides every frame needs.
 *
 * `bwa_source_get_occlusion` reads the factor the simulation published (1 = clear, 0 = blocked),
 * so the readout is the engine's own answer rather than a geometric guess drawn beside it. Without
 * the Steam Audio backend every call here is a documented no-op and the factor stays at 1; the
 * readout says so.
 */
const MATERIALS = [
  { name: "concrete", preset: 2, color: 0x8892a0 },
  { name: "glass", preset: 6, color: 0x7fc4e0 },
  { name: "carpet", preset: 5, color: 0xc08a6a },
  { name: "wood", preset: 8, color: 0xb98a4e },
  { name: "metal", preset: 9, color: 0xa8b2bd },
];
const HW = 2.0;
const HH = 1.2;

export const occlusion = {
  id: "occlusion",
  name: "3. Occlusion and materials",
  blurb: "A wall you can slide through the line of sight, with a material the ray tracer reads.",
  state: { z: -1.4, sweep: true, t: 0, material: 0, on: true, mesh: -1, factor: 1, token: 0 },

  async enter(ctx) {
    const s = this.state;
    s.mesh = -1;                        /* a handle from a previous engine build is stale */
    s.factor = 1;
    ctx.world.dragEnabled = true;
    ctx.sourceRoom = [0, ctx.headRoom[1], -3.0];
    this.panel = ctx.world.addPanel(HW * 2, HH * 2, MATERIALS[s.material].color, 0.28);
    await this.install(ctx);
    ctx.set("source_set_occlusion", ctx.srcHandle, s.on);
  },

  /** (Re)mint the material and (re)add the wall as one dynamic instance. */
  async install(ctx) {
    const s = this.state;
    const e = ctx.rig.engine;
    if (!e) return;
    if (s.mesh >= 0) { await e.invoke("scene_remove_dynamic_mesh", s.mesh); s.mesh = -1; }
    s.token = await e.invoke("material_preset", MATERIALS[s.material].preset);
    /* LOCAL space, so a pure translation moves the wall: a quad in the local xy plane, normal +z,
     * wound counter-clockwise. */
    const verts = [-HW, -HH, 0, HW, -HH, 0, HW, HH, 0, -HW, HH, 0];
    const tris = [0, 1, 2, 0, 2, 3];
    const r = await e.invokeBuf("scene_add_dynamic_mesh", { f32: verts }, 4, { i32: tris }, 2, s.token);
    s.mesh = typeof r === "number" ? r : -1;
    this.place(ctx);
  },

  place(ctx) {
    const s = this.state;
    if (s.mesh >= 0) ctx.set("scene_set_dynamic_transform", s.mesh, 0, 1.5, s.z, 0, 0, 0, 1);
    if (this.panel) this.panel.position.set(0, 1.5, s.z);
  },

  async exit(ctx) {
    const s = this.state;
    if (s.mesh >= 0 && ctx.rig.engine) {
      await ctx.rig.engine.invoke("scene_remove_dynamic_mesh", s.mesh);
      s.mesh = -1;
    }
    ctx.set("source_set_occlusion", ctx.srcHandle, false);
    this.panel = null;
  },

  update(ctx, dt) {
    const s = this.state;
    if (s.sweep) {
      s.t += dt;
      s.z = -1.4 + 1.5 * Math.sin(s.t * 0.6);
    }
    this.place(ctx);
    /* The engine's own occlusion factor, polled on the frame: it is a control-thread read of a
     * value the simulation thread publishes at its own 10-30 Hz, not a per-block quantity. */
    ctx.rig.engine?.invoke("source_get_occlusion", ctx.srcHandle)
       .then((v) => { s.factor = v; })
       .catch(() => {});
    ctx.world.setLinkColor(s.factor < 0.85 ? 0xe05252 : 0x5bd67a);
  },

  controls(ctx) {
    const s = this.state;
    return [
      {
        kind: "toggle", label: "occlusion on",
        get: () => s.on,
        set: (v) => { s.on = v; ctx.set("source_set_occlusion", ctx.srcHandle, v); },
        hint: "Off leaves the wall in the scene and stops the source listening to it: the A/B.",
      },
      {
        kind: "toggle", label: "slide the wall",
        get: () => s.sweep,
        set: (v) => { s.sweep = v; },
      },
      {
        kind: "slider", label: "wall position (room z)", min: -3, max: 0.5, step: 0.01,
        get: () => s.z,
        set: (v) => { s.z = v; s.sweep = false; this.place(ctx); },
        format: (v) => `${v.toFixed(2)} m`,
      },
      {
        kind: "select", label: "material", options: MATERIALS.map((m) => m.name),
        get: () => s.material,
        set: async (v) => {
          s.material = v;
          if (this.panel) this.panel.children.forEach((c) => c.material.color.setHex(MATERIALS[v].color));
          await this.install(ctx);
        },
        hint: "Transmission is per band, so what gets through is duller as well as quieter.",
      },
    ];
  },

  readout(ctx) {
    const s = this.state;
    return [
      ["occlusion factor", `${s.factor.toFixed(3)} (1 = clear)`],
      ["state", s.factor < 0.85 ? "occluded" : "clear"],
      ["wall z", `${s.z.toFixed(2)} m`],
      ["material", MATERIALS[s.material].name],
      ["material token", `${s.token}`],
      ["dynamic mesh handle", s.mesh < 0 ? "none (no Steam Audio backend?)" : `${s.mesh}`],
    ];
  },
};
