/* dsp.h: the audio conditioning chain between the MP3 decoder and the RF
 * modulator, plus the arcsine duty table the modulator drives.
 *
 * Deliberately free of ESP-IDF headers: this file and dsp.c compile and run
 * on a host, and test/host/ exercises them there. It is the only part of the
 * firmware that can be verified without a board, so keep it that way.
 *
 * Signal path, in order:
 *
 *   int16 mono  ->  DC block  ->  low-pass  ->  resample  ->  gain
 *               ->  limiter   ->  soft clip ->  int16 to the modulator
 *
 * Everything here runs in the decode task on core 1, never in an ISR, so
 * float is fine and the ESP32-S3 FPU makes it cheap. The ISR side is integer
 * and table-driven; see rf/carrier.c.
 */
#ifndef AMTX_DSP_H
#define AMTX_DSP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_MAX_SECTIONS 4

/* ------------------------------------------------------------------ biquad */

/* Transposed direct form II: two state words, and well behaved in float at
 * the low cutoff-to-rate ratios this project uses (4.5 kHz of 44.1 kHz). */
typedef struct {
    float b0, b1, b2, a1, a2;
    float z1, z2;
} dsp_biquad_t;

/* Cascade of Butterworth low-pass sections. `sections` of 3 is 6th order. */
typedef struct {
    dsp_biquad_t s[DSP_MAX_SECTIONS];
    int sections;
} dsp_lpf_t;

/* Design a Butterworth low-pass of 2*sections order at fc, for sample rate fs.
 * Clamps fc to below 0.45*fs. Resets state. */
void dsp_lpf_init(dsp_lpf_t *f, int sections, float fc_hz, float fs_hz);
float dsp_lpf_process(dsp_lpf_t *f, float x);

/* -------------------------------------------------------------- DC blocker */

typedef struct {
    float r;        /* pole radius */
    float x1, y1;
} dsp_dcblock_t;

void dsp_dcblock_init(dsp_dcblock_t *d, float fc_hz, float fs_hz);
float dsp_dcblock_process(dsp_dcblock_t *d, float x);

/* ---------------------------------------------------------------- limiter */

/* Feed-forward peak limiter with a cubic soft knee on the way out. The hard
 * ceiling matters here in a way it does not for a loudspeaker: an overshoot
 * past the top of the duty table is a clipped carrier peak, which is
 * splatter, not a click. */
typedef struct {
    float ceiling;
    float attack, release;   /* one-pole coefficients */
    float env;
    float gain;
} dsp_limiter_t;

void dsp_limiter_init(dsp_limiter_t *l, float ceiling, float attack_ms,
                      float release_ms, float fs_hz);
float dsp_limiter_process(dsp_limiter_t *l, float x);

/* -------------------------------------------------------------- resampler */

#define DSP_PHASE_ONE 65536u     /* Q16 */

/* Linear-interpolating fractional resampler, push model: feed it input
 * samples one at a time and it hands back however many output samples fall
 * due (usually zero or one when downsampling).
 *
 * `trim_ppm` skews the step so the consumer's FIFO stays near its target
 * fill. The station's clock and this board's crystal are independent; without
 * the trim the FIFO walks to one end and clicks. */
typedef struct {
    uint32_t step_nominal;   /* Q16 input samples per output sample */
    uint32_t step;           /* step_nominal after trim */
    uint32_t pos;            /* Q16 position inside the current segment */
    float prev, cur;
    int primed;
} dsp_resampler_t;

void dsp_resampler_init(dsp_resampler_t *r, float fs_in_hz, float fs_out_hz);
void dsp_resampler_set_trim(dsp_resampler_t *r, int32_t ppm);
/* Returns the number of output samples written to out (0..out_max). */
int dsp_resampler_push(dsp_resampler_t *r, float x, float *out, int out_max);

/* ------------------------------------------------------------------- AGC */

/* Slow loudness leveller, between the fixed gain and the limiter.
 *
 * Stations differ by more than 10 dB in average level: a heavily compressed
 * music service runs near full scale, while a jazz service with real dynamic
 * range, or speech with pauses in it, averages far lower. The peak limiter
 * cannot help, because a limiter only pushes peaks down and never lifts quiet
 * material up, so average modulation depth follows whatever the station sends
 * and that is what the ear hears as loudness.
 *
 * This tracks programme RMS and drives gain towards a target. It is what
 * broadcast AM processing does, and why AM stations all sound equally loud.
 *
 * Three details matter:
 *
 *  - The slew is asymmetric. Gain comes DOWN quickly, so a sudden loud
 *    passage does not slam the limiter, and goes UP slowly, so the level
 *    rides the programme instead of pumping on every phrase.
 *  - There is a gate. Below it the gain freezes rather than winding up, or
 *    every pause in speech would bring the noise floor up with it. This is
 *    the difference between usable and unusable on a talk station.
 *  - The limiter still follows, as the safety net. The AGC targets an
 *    average; peaks are not its job. */
typedef struct {
    float target;        /* wanted RMS, 0..1 */
    float gain;          /* current gain, linear */
    float min_gain, max_gain;
    float rms2;          /* smoothed mean square */
    float a_rms;         /* one-pole coefficient for the detector */
    float a_up, a_down;  /* gain slew, per sample */
    float gate2;         /* mean-square below which gain freezes */
    int enabled;
} dsp_agc_t;

/* attack_ms brings gain down, release_ms brings it up. `range_db` bounds the
 * gain either side of unity: {-min, +max}. */
void dsp_agc_init(dsp_agc_t *g, float fs_hz, float target,
                  float min_db, float max_db,
                  float attack_ms, float release_ms, float gate_db);
void dsp_agc_set_target(dsp_agc_t *g, float target);
void dsp_agc_set_enabled(dsp_agc_t *g, int on);
float dsp_agc_process(dsp_agc_t *g, float x);

/* ------------------------------------------------------------- whole chain */

typedef struct {
    dsp_dcblock_t dc;
    dsp_lpf_t lpf;
    dsp_resampler_t rs;
    dsp_agc_t agc;
    dsp_limiter_t lim;
    float gain;
    float fs_in, fs_out, lpf_hz;
    int sections;
    /* Peak of the last block, 0..1, for the web UI's level meter. */
    float peak;
} dsp_chain_t;

/* (Re)configure the whole chain. Safe to call when the stream's sample rate
 * changes mid-play; filter state is reset, which is a click, but the
 * alternative is an unstable filter. */
void dsp_chain_init(dsp_chain_t *c, float fs_in_hz, float fs_out_hz,
                    float lpf_hz, int sections, float ceiling,
                    float attack_ms, float release_ms);

void dsp_chain_set_gain(dsp_chain_t *c, float gain);
void dsp_chain_set_trim(dsp_chain_t *c, int32_t ppm);
void dsp_chain_set_agc(dsp_chain_t *c, int on, float target);

/* What the AGC is currently doing, in dB. Worth showing: it explains why a
 * quiet station is not quiet any more. */
float dsp_chain_agc_gain_db(const dsp_chain_t *c);

/* Push n_in mono samples; returns how many output samples were written.
 * `out_max` must be at least n_in * fs_out / fs_in + 2. */
int dsp_chain_process(dsp_chain_t *c, const int16_t *in, int n_in,
                      int16_t *out, int out_max);

/* Downmix interleaved stereo to mono in place-safe fashion.
 * `frames` is sample pairs; channels of 1 is a straight copy. */
void dsp_downmix(const int16_t *in, int frames, int channels, int16_t *out);

/* ------------------------------------------------------------- duty table */

/* Fill `tbl` with LEDC duty values in Q8 (duty << 8), indexed by the audio
 * sample mapped onto 0..entries-1.
 *
 * This is the heart of the transmitter and the reason it works at all. The
 * amplitude of the fundamental of a square wave of duty D goes as sin(pi*D),
 * whose slope is ZERO at D = 0.5. Modulating about 50% duty, as the original
 * brief specified, produces almost no AM (LL-02, KI-01).
 *
 * So: place the positive modulation peak at D = 0.5, where the radiated
 * amplitude is greatest, put the unmodulated carrier at 1/(1+depth) of that,
 * and invert the sine to get the duty for a wanted amplitude:
 *
 *     a(s) = (1 + depth*s) / (1 + depth)      s in [-1, +1]
 *     D(s) = asin(a(s)) / pi
 *
 * The radiated envelope is then linear in s, which is what an envelope
 * detector in a receiver expects. */
/* `level` scales the whole radiated envelope, 0..1. It is the carrier level
 * control: 1.0 puts the modulation peak at 50% duty, where a square wave
 * radiates most; lower values shrink the entire envelope towards zero and so
 * reduce the field. It costs resolution, because the envelope then occupies
 * fewer duty steps, which is the price of turning the power down. */
void dsp_build_duty_table(uint16_t *tbl, int entries, int res_bits,
                          float depth, float level);

/* The normalised fundamental amplitude a square wave of this Q8 duty
 * radiates, 0..1. Exposed so the host tests can prove the table is linear. */
float dsp_duty_q8_to_amplitude(uint16_t duty_q8, int res_bits);

#ifdef __cplusplus
}
#endif
#endif /* AMTX_DSP_H */
