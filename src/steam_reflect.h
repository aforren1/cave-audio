/*
 * steam_reflect.h — reflection bed (materials, the diffuse reverb layer). PHONON-FREE interface
 * (like steam_decode.h / steam_scene.h), so engine.c includes it unconditionally; steam_reflect.c
 * links phonon and compiles ONLY with BWA_WITH_STEAMAUDIO. Callers gate on BWA_HAVE_STEAMAUDIO.
 *
 * A single shared listener-centric reverb bed. It owns its OWN reflections IPLSimulator + a dedicated
 * sim thread + an IMMORTAL listener-centric bed IPLSource (the IR aliases interior memory of that
 * source, so it must outlive every audio-thread apply), and the persistent audio-thread effects
 * (one HYBRID IPLReflectionEffect + one custom 26-direction IPLAmbisonicsDecodeEffect + scratch). It
 * BORROWS the IPLContext + IPLScene from SteamScene (shared geometry); it does not own them and must
 * be destroyed before the scene. The sim thread ray-traces reflections at a low rate and publishes a
 * small POD (the IPLReflectionEffectParams) through a seqlock; the audio thread (steam_reflect_tap)
 * convolves the mono aux send through that IR, decodes the ambisonic result to the 26 speakers
 * (world-locked, identity orientation — matching the sim's world-frame listener), and sums it onto
 * the bus. The bus tap is registered with rt via rt_set_bus_tap (a plain C function pointer).
 *
 * RT-safety: every effect + all scratch is created at bwa_start (off the audio thread, after the
 * device block is known); the per-block apply is allocation/lock/syscall-free (verified against the
 * phonon source — the IR handoff is a lock-free SPSC triple buffer). The bed is silent until the
 * first RunReflections publishes.
 */
#ifndef BWA_STEAM_REFLECT_H
#define BWA_STEAM_REFLECT_H

#include "steam_scene.h"   /* SteamScene* (opaque) + the phonon-object accessors */
#include "rt.h"            /* RtCore* (for the listener pose) + the RtBusTap signature */
#include "layout.h"        /* Layout — the 26 speaker directions for the custom-layout decode */
#include <stdint.h>

typedef struct SteamReflect SteamReflect;

/* Create the bed. `scene` supplies the borrowed IPLContext + IPLScene; `rt` supplies the listener
 * pose (rt_read_pose) on the sim thread; `L` supplies the 26 speaker directions. `block` MUST equal
 * the device block (phonon frameSize is fixed at create). `order` is 1 or 2; `ir_seconds` sizes the
 * convolution IR. NULL on failure (then no tap is registered and the engine runs dry). Call at
 * bwa_start, off the audio thread, AFTER the sink's true block size is known. */
SteamReflect* steam_reflect_create(SteamScene* scene, RtCore* rt, const Layout* L,
                                   uint32_t sample_rate, uint32_t block, uint32_t order,
                                   float ir_seconds, uint32_t num_rays, uint32_t num_bounces,
                                   float wet_gain, int bake);   /* bake!=0: precompute reverb at probes (bwa_reflections_desc.bake) */

/* The rt bus tap (matches RtBusTap): AUDIO thread, after the voice loop, before align_process.
 * ud == the SteamReflect*. Convolves `aux` and sums the decoded 26-ch reverb onto `bus`. */
void steam_reflect_tap(void* ud, float* bus, uint32_t n, const float* lp, const float* lq, const float* aux);

/* Set the wet level applied to the reverb before it sums onto the bus (linear; 1 = unity). Control
 * thread, per-frame-safe (a single relaxed atomic the audio-thread tap reads). */
void steam_reflect_set_gain(SteamReflect* r, float linear);

/* Stop the sim thread + release the owned phonon objects. Call AFTER the audio thread is joined and
 * the tap is unregistered (the IR aliases the bed source), and BEFORE steam_scene_destroy. */
void steam_reflect_destroy(SteamReflect* r);

#endif /* BWA_STEAM_REFLECT_H */
