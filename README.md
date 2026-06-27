# bwaudio

Self-hosted spatial audio engine for a 26-speaker CAVE installation. Drives the
array over **ASIO → Dante Virtual Soundcard**, with a **binaural HRTF monitor** for
desk-side debugging. Unity and Unreal connect as thin control clients over a C ABI.

> **Status: design / planning.** This repo currently holds specs only. No
> implementation yet. `include/bwaudio.h` is the authoritative contract.

## Why self-hosted

Going direct (no FMOD/Wwise) gives sample-accurate access to ASIO timing hooks and
keeps the core engine-agnostic, so the same library serves both engines with only a
thin per-engine glue layer. The spatializer, mixer, output, and tracking all live
in one process behind one audio callback.

## Shape

```
 engine (Unity/Unreal) ──control──┐
 OptiTrack (NatNet) ─────pose─────┤
                                  ▼
                     ┌─ voice playback ─ DBAP pan ─► 26-ch master bus ─┐
                     │  (one audio callback)                           │
                     └─────────────────────────────────────────────────┘
                                  │                         │
                          ASIO ► DVS ► array        binaural monitor ► stereo
                          (production)              (debug)
```

## Read next

Start with [`CLAUDE.md`](./CLAUDE.md), then [`docs/architecture.md`](./docs/architecture.md).
Full doc index is in `CLAUDE.md`.

## Playground

An interactive [raylib](https://www.raylib.com/) scene to audition the binaural monitor by
ear — move a sound source around the 26-speaker array and turn your head, hear it on
headphones in real time:

```
cmake -S . -B build -DBWAUDIO_BUILD_PLAYGROUND=ON
cmake --build build --config RelWithDebInfo --target bw_playground
./build/RelWithDebInfo/bw_playground          # WASD move source, R/F up/down, Q/E turn head
```

It drives the `binaural` profile straight to headphones through an auto-picked 2-ch ASIO
driver (ASIO4ALL / FlexASIO / the Steinberg built-in); without one it runs silently (visual
only). Opt-in so the default build stays lean. Source: [`examples/playground.c`](./examples/playground.c).

## Platform & licensing

Windows-only (ASIO). Links the Steinberg ASIO SDK under its GPLv3 option and Steam
Audio; see [`docs/build.md`](./docs/build.md) for the copyleft/distribution
implications before you ship anything.

Licensed under **GPLv3** ([`LICENSE`](./LICENSE)), consistent with the ASIO SDK's GPLv3 option.
Third-party components keep their own licenses (see [`docs/build.md`](./docs/build.md)).
