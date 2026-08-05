/*
 * sos.h — speed of sound for the MEASUREMENT tools (bwa_calibrate, bwa_layout_tool, calib_view).
 * Header-only; control/tool side, never the audio thread.
 *
 * Why this is a variable and not a constant: c varies with air temperature by about 0.6 m/s per
 * degree C, so a room at 15 C and one at 25 C differ by ~2%. That is irrelevant to what anything
 * SOUNDS like (2% of a 3 ms alignment delay is 0.06 ms), but it is the dominant systematic in the
 * acoustic SURVEY, where range = c * delay: 2% of a 4 m range is 8 cm, an order of magnitude larger
 * than the 2-3 mm capsule-geometry term the Zylia solve already corrects for. A CAVE held at 73 F
 * (22.8 C) is really at 345.1 m/s, so assuming 343.0 reads every range ~0.6% short.
 *
 * NOT the engine's speed of sound. rt.c owns BWA_SPEED_OF_SOUND (Doppler, live via
 * bwa_set_speed_of_sound, deliberately driven to 1480 for the playground's underwater demo). That
 * one is a medium/creative control; this one is a property of the room you are measuring in.
 *
 * The default is the textbook 20 C figure, NOT any one install's number: the tools seed themselves
 * from the layout file's reference.speed_of_sound_mps and fall back here only when the file is
 * silent. Keeping the fallback neutral also keeps every synthetic-capture golden bit-identical
 * (calib_capture.c and the analyzers it feeds are a matched pair at 343.0 — move one side only and
 * the recovered positions bias).
 */
#ifndef BWA_SOS_H
#define BWA_SOS_H

#include <ctype.h>
#include <stdlib.h>

#define BWA_SOS_REF_MPS 343.0   /* dry air at 20 C / 68 F — the documented fallback */

/* Plausible-room guard. Wide enough for any real install (306 m/s is about -42 C, 380 about +80 C),
 * tight enough that a fat-fingered "--temp 730" or a meters/feet mixup is rejected rather than
 * silently surveyed. */
#define BWA_SOS_MIN_MPS 306.0
#define BWA_SOS_MAX_MPS 380.0

/* The standard linear fit, good to well under 0.1 m/s across any habitable room temperature.
 * Humidity adds at most ~0.3 m/s at high RH, which is inside the noise of everything above. */
static inline double sos_from_temp_c(double t_c) { return 331.3 + 0.606 * t_c; }
static inline double sos_from_temp_f(double t_f) { return sos_from_temp_c((t_f - 32.0) * 5.0 / 9.0); }

/* Parse a temperature token: "22.8" or "22.8C" (Celsius, the default) or "73F". Returns 1 and
 * writes the resulting speed of sound on success, 0 on a malformed token or an implausible result.
 * Celsius is the default unit because the formula is Celsius; the F suffix exists because plenty of
 * rooms are specified in Fahrenheit and converting by hand is a silent-error opportunity. */
static inline int sos_parse_temp(const char* s, double* out_mps) {
    if (!s || !*s || !out_mps) return 0;
    char* end = NULL;
    double t = strtod(s, &end);
    if (end == s) return 0;                                  /* no number at all */
    while (*end && isspace((unsigned char)*end)) ++end;
    double c;
    if      (*end == 'f' || *end == 'F') { c = sos_from_temp_f(t); ++end; }
    else if (*end == 'c' || *end == 'C') { c = sos_from_temp_c(t); ++end; }
    else                                   c = sos_from_temp_c(t); /* bare number = Celsius */
    if (*end) return 0;                                      /* trailing junk: reject, don't guess */
    if (!(c >= BWA_SOS_MIN_MPS && c <= BWA_SOS_MAX_MPS)) return 0;
    *out_mps = c;
    return 1;
}

/* Parse a direct speed-of-sound token ("345.1"), for someone who measured c rather than temperature. */
static inline int sos_parse_mps(const char* s, double* out_mps) {
    if (!s || !*s || !out_mps) return 0;
    char* end = NULL;
    double c = strtod(s, &end);
    if (end == s || *end) return 0;
    if (!(c >= BWA_SOS_MIN_MPS && c <= BWA_SOS_MAX_MPS)) return 0;
    *out_mps = c;
    return 1;
}

#endif /* BWA_SOS_H */
