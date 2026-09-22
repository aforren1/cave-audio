/**
 * Scene 5: The medium boundary.
 *
 * Dive, and two different things happen at once.
 *
 * THE PER-SOURCE HALF. A path that CROSSES the surface pays the interface loss and the water
 * muffle: `bwa_source_set_occlusion_manual` with a broadband transmittance of 0.03 and a low/mid/
 * high tilt of 1 / 0.2 / 0.033. The tilt is relative to the level (the engine multiplies them), so
 * the low band is pinned at 1 and the interface loss is charged once. Net about -30 / -44 / -60 dB,
 * which is why the thump survives and the detail does not. Localization collapses too, so the
 * source gets a wide spread.
 *
 * THE MEDIUM HALF. `bwa_set_speed_of_sound` glides every delay to 1480 m/s, which is 4.3 times
 * air, so Doppler and path delays shrink with it. Turn Doppler on and drag the source past
 * yourself to hear the difference.
 *
 * THE SURFACE BOUNCE. With both ends under it, the surface is a PRESSURE-RELEASE mirror plane
 * (`bwa_scene_set_ground` with pressure_release true) and the image-source reflection comes back
 * INVERTED. Push the source up toward the surface and the direct sound and its inverted image
 * cancel: the Lloyd's-mirror comb, and the reason a near-surface source sounds thin. Broadband
 * stimuli show it best, so pick the pink bursts.
 *
 * WHAT IS MISSING, and it is missing rather than faked: the native playground also retunes the FDN
 * late tail on the dive (a long low band, a dead high band). Enabling the FDN is `bwa_fdn_config`,
 * which takes a struct, and this binding marshals arrays but deliberately not structs
 * (bindings/web/README.md). So this scene has the boundary and the medium, and no room tail.
 */
const WATER_Y = 2.4;
const WATER_BANDS = [1.0, 0.2, 0.033];

export const underwater = {
  id: "underwater",
  name: "5. Medium boundary",
  blurb: "Dive. The interface loss, the muffle, the speed of sound, and the surface's inverted bounce.",
  state: { under: false, lloyd: true, doppler: true, crossed: -1, er: -1, sweep: true, t: 0 },

  async enter(ctx) {
    const s = this.state;
    s.crossed = -1;
    s.er = -1;
    ctx.world.dragEnabled = true;
    ctx.sourceRoom = [0, WATER_Y + 0.8, -2.2];
    this.surface = ctx.world.addPanel(9, 9, 0x5bb0e0, 0.18);
    this.surface.rotation.x = -Math.PI / 2;
    this.surface.position.y = WATER_Y;
    this.image = ctx.world.addSphere(0.14, 0x78bef0, 0.45);
    this.image.visible = false;
    this.imageLine = ctx.world.addLine(0x78bef0, 2);
    ctx.set("source_set_doppler", ctx.srcHandle, s.doppler);
    this.applyMedium(ctx);
  },

  async exit(ctx) {
    const s = this.state;
    ctx.set("set_speed_of_sound", 343);
    ctx.set("source_set_early_reflections", ctx.srcHandle, false);
    ctx.set("source_set_doppler", ctx.srcHandle, false);
    ctx.set("source_set_spread", ctx.srcHandle, 0);
    ctx.setBuf("source_set_occlusion_manual", ctx.srcHandle, 1.0, { f32: [1, 1, 1] });
    s.crossed = -1;
    s.er = -1;
    this.surface = null;
    this.image = null;
    this.imageLine = null;
  },

  applyMedium(ctx) {
    const s = this.state;
    ctx.set("set_speed_of_sound", s.under ? 1480 : 343);
    /* The surface as a mirror plane. Pressure release only while the listener is under it: seen
     * from above, water is the HARDER medium and the bounce keeps its polarity. */
    ctx.set("scene_set_ground", WATER_Y, 0, s.under);
    s.crossed = -1;                      /* force the per-source half to re-derive */
    s.er = -1;
  },

  update(ctx, dt) {
    const s = this.state;
    if (s.sweep) {
      s.t += dt;
      /* Ride the source through the surface, which is the transition worth hearing. */
      const [x, , z] = ctx.sourceRoom;
      ctx.sourceRoom = [x, WATER_Y + 1.1 * Math.sin(s.t * 0.45), z];
    }
    const above = ctx.sourceRoom[1] > WATER_Y;
    const crossed = above === s.under;   /* one end above, the other below */
    if (crossed !== s.crossed) {
      s.crossed = crossed;
      if (crossed) {
        ctx.setBuf("source_set_occlusion_manual", ctx.srcHandle, 0.03, { f32: WATER_BANDS });
        ctx.set("source_set_spread", ctx.srcHandle, 0.8);
      } else {
        ctx.setBuf("source_set_occlusion_manual", ctx.srcHandle, 1.0, { f32: [1, 1, 1] });
        ctx.set("source_set_spread", ctx.srcHandle, 0.0);
      }
    }
    /* The bounce renders only with BOTH ends under the surface: a cross-boundary image has no
     * physical path, and the crossing source is muffled to nothing anyway. */
    const er = s.lloyd && s.under && !above ? 1 : 0;
    if (er !== s.er) {
      s.er = er;
      ctx.set("source_set_early_reflections", ctx.srcHandle, er === 1);
    }

    ctx.world.setSourceColor(above ? 0xe6c24a : 0x5bb0e0);
    ctx.world.setLinkColor(crossed ? 0x60829e : 0x5bd67a);
    this.surface.children.forEach((c) => { c.material.opacity = s.under ? 0.32 : 0.14; });
    this.image.visible = er === 1;
    if (er === 1) {
      const [x, y, z] = ctx.sourceRoom;
      const iy = 2 * WATER_Y - y;
      this.image.position.set(x, iy, z);
      ctx.world.setLinePoints(this.imageLine, [[x, iy, z], ctx.headRoom]);
      this.imageLine.visible = true;
    } else if (this.imageLine) {
      this.imageLine.visible = false;
    }
  },

  controls(ctx) {
    const s = this.state;
    return [
      {
        kind: "toggle", label: "the listener is under water",
        get: () => s.under,
        set: (v) => { s.under = v; this.applyMedium(ctx); ctx.refresh(); },
        hint: "The head stays at 1.5 m and the surface is at 2.4 m, so under means the room fills.",
      },
      {
        kind: "toggle", label: "ride the source through the surface",
        get: () => s.sweep,
        set: (v) => { s.sweep = v; },
      },
      {
        kind: "toggle", label: "surface bounce (Lloyd's mirror)",
        get: () => s.lloyd,
        set: (v) => { s.lloyd = v; s.er = -1; },
        hint: "Both ends under. The inverted image cancels the direct sound near the surface.",
      },
      {
        kind: "toggle", label: "Doppler",
        get: () => s.doppler,
        set: (v) => { s.doppler = v; ctx.set("source_set_doppler", ctx.srcHandle, v); },
        hint: "What makes the speed of sound audible at all.",
      },
      {
        kind: "note",
        text: "No room tail here. Enabling the FDN late reverb is a struct-taking call this " +
              "binding does not marshal, so the scene ships with the boundary and the medium only.",
      },
    ];
  },

  readout(ctx) {
    const s = this.state;
    const above = ctx.sourceRoom[1] > WATER_Y;
    return [
      ["listener", s.under ? "under water" : "in air"],
      ["source", above ? "above the surface" : "under water"],
      ["path crosses the surface", s.crossed === 1 ? "yes" : "no"],
      ["speed of sound", `${s.under ? 1480 : 343} m/s`],
      ["surface bounce", s.er === 1 ? "on (pressure release)" : "off"],
      ["source height", `${ctx.sourceRoom[1].toFixed(2)} m (surface ${WATER_Y})`],
    ];
  },
};
