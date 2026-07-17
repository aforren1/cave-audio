# Signal flow (rendered, with the code map)

The full render path as a Mermaid graph — structurally the same as the ASCII diagram in
[`architecture.md`](architecture.md) ("The full render path"), which stays the canonical,
raw-readable version; **edit that one first and keep this in sync**. On top of the structure,
this page carries the *code map*: each stage names the function(s) that implement it (plain
text, mostly `rt.c` and the module files) and the `bwa_*` calls that configure it (italics).
The tap-order rationale (why the sends branch where they do) lives with the ASCII diagram.

```mermaid
flowchart TD
  subgraph CTRL["control thread (engine.c forwards the bwa_* ABI to rt.c)"]
    WAV["wav / flac / mp3 → mono asset<br/>sound_load — decode + resample<br/><i>bwa_load_sound</i>"]
    BEDA["AmbiX 4/9/16 ch → bed asset<br/>sound_load_ambix<br/><i>bwa_load_ambix</i>"]
    FUMA["FuMa → reorder + rescale → the same bed asset<br/>sound_load_fuma<br/><i>bwa_load_fuma</i>"]
    STRM["disk file → streaming thread → per-stream ring<br/>stream_open; the stream thread fills the ring<br/><i>bwa_load_sound_streaming</i>"]
    PUSH["caller PCM → push ring (caller is the producer)<br/>stream_open_push · stream_push<br/><i>bwa_source_create_push · bwa_source_push / _space / _end</i>"]
    CMD["every per-frame call → command ring (cmd_push)<br/>commit promotes pending → active + drains events<br/><i>bwa_source_* · bwa_set_listener_pose · bwa_commit</i>"]
  end

  subgraph SIM["off-thread producers"]
    POSE["OptiTrack → pose seqlock<br/>natnet.c → pose_write_t<br/><i>bwa_tracker_connect · bwa_set_pose_prediction</i>"]
    SCENE["scene sim 30 Hz: occlusion · transmission tilt ·<br/>directivity — steam_scene.c → rt_set_direct<br/><i>bwa_scene_set_mesh_mat/_box · bwa_source_set_occlusion ·<br/>_set_directivity · _set_occlusion_manual (no-SDK path)</i>"]
    PATHS["path sim 10 Hz: per-voice shCoeffs + bending tilt<br/>steam_path.c → rt_set_pathing<br/><i>bwa_desc.enable_pathing · bwa_source_set_pathing</i>"]
  end

  SOLVE["gain solve — compute_gains (block rate, dirty-gated)<br/>panner_gains → dbap_gains / spcap_gains / vbap_gains →<br/>per-source atten override (by ratio, atten_curve) →<br/>spread: spread_gains (LOBE) / mdap_gains / fs_solve (SPECTRAL);<br/>anisotropic w×h extent (up-anchored frame + affine squash)<br/><i>bwa_set_panner · _dual_band · _spread_mode · _near_spread ·<br/>_extra_listeners · bwa_source_set_spread / _extent / _size / _gain /<br/>_attenuation_override · bwa_group_set_gain · bwa_source_fade_to</i>"]

  subgraph VOICE["mono voice — mix_voice, per sample (inside rt_render)"]
    READ["read: pcm cursor · pitch resample · stream_pull<br/><i>bwa_source_play / _play_at · bwa_source_set_pitch</i>"]
    GATE["× pause / seek gate — pause_gate<br/><i>bwa_source_set_paused · _seek · bwa_set_paused · stops/steals</i>"]
    TEQ["transmission EQ (3 biquads — occlusion's tilt)<br/>coeffs from the scene sim's publish"]
    OCC["× occlusion level × directivity<br/>gated on the voice's own generation"]
    PROP["air-absorption LP → loudness shelf → Doppler ring<br/><i>bwa_source_set_air_absorption · _loudness_comp · _doppler</i>"]
    PAN["pan output — gains ramped toward compute_gains' targets<br/>single · dual-band 700 Hz · spectral 6-band"]
    READ --> GATE --> TEQ --> OCC --> PROP --> PAN
  end

  WAV --> READ
  STRM --> READ
  PUSH --> READ
  CMD -.-> SOLVE
  POSE -.-> SOLVE
  SCENE -.-> OCC
  SOLVE -.-> PAN

  BUS[("N-ch MASTER BUS")]
  DECOR[("DECOR bus")]
  AUX[("AUX — mono reverb send")]
  PACC[("PATH ACCUM — ambisonic")]

  GATE -. "s_raw → bending-loss EQ → × shCoeffs<br/><i>bwa_source_set_pathing</i>" .-> PACC
  PATHS -.-> PACC
  OCC -. "× wet send (× distance)<br/><i>bwa_source_set_reverb · _reflection_send · _reflection_distance</i>" .-> AUX
  OCC -. "ISM: 6 shoebox mirror images — ism_images;<br/>frac delay · HF damp · per-image panner_gains<br/><i>bwa_source_set_early_reflections · bwa_scene_set_box ·<br/>bwa_early_reflections_set_gain</i>" .-> BUS
  PROP -. "decor split × √spread<br/><i>bwa_set_decorrelation</i>" .-> DECOR
  PAN --> BUS

  subgraph BEDV["bed voice — mix_bed, per sample (inside rt_render)"]
    SH["SH frames (world-locked soundfield)<br/><i>bwa_bed_play · bwa_bed_set_gain</i>"]
    ROT["rotate: yaw phasor (bed_rotate_z) ·<br/>full 3-axis ambi_rot_matrix / ambi_rot_apply (glided)<br/><i>bwa_bed_set_rotation · bwa_bed_set_orientation</i>"]
    MTX["matrix render: × max-rE taper (ambi_max_re_weights,<br/>crossfaded; opt. band-split: taper > 700 Hz only) →<br/>bed decode — build_bed_decode: allrad_build_decode /<br/>epad_build_decode (SAD = internal fallback)<br/><i>bwa_set_max_re · _max_re_split · bwa_desc.bed_decoder</i>"]
    PARA["parametric render (crossfaded): FOA bands →<br/>DirAC direction + diffuseness ψ<br/><i>bwa_set_bed_renderer</i>"]
    SH --> ROT
    ROT --> MTX
    ROT --> PARA
  end

  BEDA --> SH
  FUMA --> SH
  MTX --> BUS
  PARA -- "direct √(1−ψ)·W → listener-relative<br/>panner_gains at the array shell" --> BUS
  PARA -- "diffuse √ψ·FOA → bed decode (raw)" --> DECOR

  DECOR --> VN["per-channel sparse velvet-noise filters<br/>(mutually incoherent copies; built in rt_create)"] --> BUS
  AUX --> RTAP["the ONE reverb tap (rt_set_bus_tap):<br/>steam_reflect_tap — convolve → ambisonic IR → phonon decode<br/>·or· fdn_tap — 16 lines · 2-band decay · direction-scaled ·<br/>plane waves through the bed decode + max-rE pair<br/><i>bwa_reflections_config · bwa_fdn_config · bwa_reverb_set_gain</i>"] --> BUS
  PACC --> PTAP["path tap (rt_set_path_tap):<br/>steam_path_tap — phonon's own ambisonics decode"] --> BUS

  BUS --> MG["× master gain (ramped)<br/><i>bwa_set_master_gain</i>"]
  MG --> ALIGN["align_process + room_eq_track:<br/>per-speaker correction FIR · room-EQ biquads<br/>(re-aimed at the tracked pose) · gain trim · delay<br/><i>trims/EQ written by bwa_calibrate · bwa_set_tracked_room_eq</i>"]
  ALIGN --> TSIG["+ test signal (raw channel, post-align)<br/><i>bwa_set_test_signal</i>"]
  TSIG --> LIM["linked limiter → per-channel peak meters<br/><i>bwa_set_limiter · _limiter_ceiling · bwa_get_bus_levels</i>"]

  LIM --> CAVE["cave: asio_sink.cpp bufferSwitch →<br/>26-ch ASIO ► DVS ► the array"]
  LIM --> BIN["binaural: each channel = a virtual speaker →<br/>3rd-order SH encode (ambi_encode_sn3d) →<br/>phonon HRTF decode (steam_decode.c;<br/>binaural.c simple-pan fallback) → 2-ch device"]
  LIM --> BOTH["both: array sink + monitor on a second device<br/>(double-buffered handoff, engine.c)"]
  LIM --> NUL["null sink (null_sink.c): no device —<br/>keeps rendering in real time, silent"]
```

Reading the map: plain function names are where the DSP *runs* (almost all on the audio
thread, inside one `rt_render` block); italic `bwa_*` names are the control-thread calls
that *configure* that stage — every one of them crosses over via the command ring or an
atomic, never by touching DSP state directly (`concurrency.md` is the contract).
