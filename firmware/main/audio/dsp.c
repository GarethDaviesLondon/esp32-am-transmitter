/* dsp.c: see dsp.h. Host-buildable; no ESP-IDF headers here. */

#include "dsp.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define INT16_SCALE 32768.0f

static float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

/* ------------------------------------------------------------------ biquad */

/* RBJ low-pass, normalised. q is the section's Butterworth Q. */
static void biquad_lowpass(dsp_biquad_t *b, float fc_hz, float fs_hz, float q)
{
    const float w0 = 2.0f * (float)M_PI * fc_hz / fs_hz;
    const float cw = cosf(w0);
    const float sw = sinf(w0);
    const float alpha = sw / (2.0f * q);

    const float a0 = 1.0f + alpha;
    b->b0 = ((1.0f - cw) * 0.5f) / a0;
    b->b1 = (1.0f - cw) / a0;
    b->b2 = b->b0;
    b->a1 = (-2.0f * cw) / a0;
    b->a2 = (1.0f - alpha) / a0;
    b->z1 = b->z2 = 0.0f;
}

/* Butterworth pole Q values, by cascade position, for orders 2, 4, 6 and 8.
 * A cascade of `n` sections is order 2n; q_table[n-1][k] is section k's Q. */
static const float q_table[4][4] = {
    { 0.70710678f, 0.0f,        0.0f,        0.0f        },  /* order 2 */
    { 0.54119610f, 1.30656296f, 0.0f,        0.0f        },  /* order 4 */
    { 0.51763809f, 0.70710678f, 1.93185165f, 0.0f        },  /* order 6 */
    { 0.50979558f, 0.60134489f, 0.89997622f, 2.56291545f },  /* order 8 */
};

void dsp_lpf_init(dsp_lpf_t *f, int sections, float fc_hz, float fs_hz)
{
    if (sections < 1) sections = 1;
    if (sections > DSP_MAX_SECTIONS) sections = DSP_MAX_SECTIONS;
    f->sections = sections;

    /* Above 0.45*fs the bilinear warp makes the design meaningless, and a
     * cutoff that high defeats the point of the filter anyway. */
    fc_hz = clampf(fc_hz, 20.0f, 0.45f * fs_hz);

    for (int i = 0; i < sections; i++) {
        biquad_lowpass(&f->s[i], fc_hz, fs_hz, q_table[sections - 1][i]);
    }
}

float dsp_lpf_process(dsp_lpf_t *f, float x)
{
    for (int i = 0; i < f->sections; i++) {
        dsp_biquad_t *b = &f->s[i];
        const float y = b->b0 * x + b->z1;
        b->z1 = b->b1 * x - b->a1 * y + b->z2;
        b->z2 = b->b2 * x - b->a2 * y;
        x = y;
    }
    return x;
}

/* -------------------------------------------------------------- DC blocker */

void dsp_dcblock_init(dsp_dcblock_t *d, float fc_hz, float fs_hz)
{
    d->r = 1.0f - (2.0f * (float)M_PI * fc_hz / fs_hz);
    d->r = clampf(d->r, 0.0f, 0.9999f);
    d->x1 = d->y1 = 0.0f;
}

float dsp_dcblock_process(dsp_dcblock_t *d, float x)
{
    const float y = x - d->x1 + d->r * d->y1;
    d->x1 = x;
    d->y1 = y;
    return y;
}

/* ---------------------------------------------------------------- limiter */

static float one_pole_coef(float ms, float fs_hz)
{
    if (ms <= 0.0f) return 1.0f;
    const float y = 1.0f - expf(-1.0f / (fs_hz * ms * 0.001f));
    return clampf(y, 0.0f, 1.0f);
}

void dsp_limiter_init(dsp_limiter_t *l, float ceiling, float attack_ms,
                      float release_ms, float fs_hz)
{
    l->ceiling = clampf(ceiling, 0.05f, 1.0f);
    l->attack = one_pole_coef(attack_ms, fs_hz);
    l->release = one_pole_coef(release_ms, fs_hz);
    l->env = 0.0f;
    l->gain = 1.0f;
}

/* Soft knee above `knee`, asymptotic to `ceiling`. Transparent below the
 * knee: a limiter that colours quiet passages is a compressor, and on AM a
 * permanently reduced gain is a permanently under-modulated carrier.
 *
 * A single cubic cannot have both unity small-signal slope and saturation at
 * the ceiling, so the knee is explicit and tanh does the bending. tanh is
 * only evaluated on the samples that actually need it. */
static float soft_knee(float x, float knee, float ceiling)
{
    const float a = fabsf(x);
    if (a <= knee) return x;

    const float span = ceiling - knee;
    if (span <= 0.0f) return x < 0.0f ? -ceiling : ceiling;

    const float y = knee + span * tanhf((a - knee) / span);
    return x < 0.0f ? -y : y;
}

float dsp_limiter_process(dsp_limiter_t *l, float x)
{
    const float a = fabsf(x);

    /* Attack fast on the way up, release slowly on the way down. */
    l->env += (a > l->env ? l->attack : l->release) * (a - l->env);

    const float want = (l->env > l->ceiling) ? (l->ceiling / l->env) : 1.0f;

    /* Gain follows the same asymmetry: clamp down at once, recover gently. */
    if (want < l->gain) {
        l->gain = want;
    } else {
        l->gain += l->release * (want - l->gain);
    }

    /* The gain stage already holds the signal at the ceiling in the steady
     * state; the knee only catches what gets past it during an attack. */
    return soft_knee(x * l->gain, 0.70f * l->ceiling, l->ceiling);
}

/* -------------------------------------------------------------- resampler */

void dsp_resampler_init(dsp_resampler_t *r, float fs_in_hz, float fs_out_hz)
{
    double step = (double)fs_in_hz / (double)fs_out_hz * (double)DSP_PHASE_ONE;
    if (step < 1.0) step = 1.0;
    r->step_nominal = (uint32_t)(step + 0.5);
    r->step = r->step_nominal;
    r->pos = 0;
    r->prev = r->cur = 0.0f;
    r->primed = 0;
}

void dsp_resampler_set_trim(dsp_resampler_t *r, int32_t ppm)
{
    const int64_t delta = ((int64_t)r->step_nominal * ppm) / 1000000;
    int64_t step = (int64_t)r->step_nominal + delta;
    if (step < 1) step = 1;
    r->step = (uint32_t)step;
}

int dsp_resampler_push(dsp_resampler_t *r, float x, float *out, int out_max)
{
    r->prev = r->cur;
    r->cur = x;

    /* The first sample only establishes prev/cur; there is no segment yet. */
    if (!r->primed) {
        r->primed = 1;
        return 0;
    }

    int n = 0;
    while (r->pos < DSP_PHASE_ONE && n < out_max) {
        const float frac = (float)r->pos * (1.0f / (float)DSP_PHASE_ONE);
        out[n++] = r->prev + (r->cur - r->prev) * frac;
        r->pos += r->step;
    }

    /* If out_max ran out mid-segment we would emit the rest next call with a
     * stale segment, so only retire the segment once it is fully consumed.
     * Callers size `out` from the rate ratio, so this is belt and braces. */
    if (r->pos >= DSP_PHASE_ONE) {
        r->pos -= DSP_PHASE_ONE;
    }
    return n;
}

/* ------------------------------------------------------------- whole chain */

void dsp_chain_init(dsp_chain_t *c, float fs_in_hz, float fs_out_hz,
                    float lpf_hz, int sections, float ceiling,
                    float attack_ms, float release_ms)
{
    memset(c, 0, sizeof(*c));
    c->fs_in = fs_in_hz;
    c->fs_out = fs_out_hz;
    c->lpf_hz = lpf_hz;
    c->sections = sections;
    c->gain = 1.0f;

    dsp_dcblock_init(&c->dc, 25.0f, fs_in_hz);
    dsp_lpf_init(&c->lpf, sections, lpf_hz, fs_in_hz);
    dsp_resampler_init(&c->rs, fs_in_hz, fs_out_hz);
    dsp_limiter_init(&c->lim, ceiling, attack_ms, release_ms, fs_out_hz);

    /* Defaults chosen for broadcast-ish behaviour rather than transparency:
     * +18 dB of lift is enough to bring a quiet jazz service up to a
     * compressed pop one, -6 dB of cut keeps a hot station off the limiter,
     * and the -50 dB gate stops speech pauses lifting the noise floor. The
     * enable state and target are set by the caller afterwards, so a
     * mid-stream sample-rate change does not silently turn the AGC off. */
    {
        const int was_on = c->agc.enabled;
        const float target = c->agc.target > 0.0f ? c->agc.target : 0.25f;
        dsp_agc_init(&c->agc, fs_out_hz, target, -6.0f, 18.0f,
                     200.0f, 3000.0f, -50.0f);
        dsp_agc_set_enabled(&c->agc, was_on);
    }
}

void dsp_chain_set_gain(dsp_chain_t *c, float gain)
{
    c->gain = clampf(gain, 0.0f, 16.0f);
}

void dsp_chain_set_agc(dsp_chain_t *c, int on, float target)
{
    dsp_agc_set_target(&c->agc, target);
    dsp_agc_set_enabled(&c->agc, on);
}

float dsp_chain_agc_gain_db(const dsp_chain_t *c)
{
    if (!c->agc.enabled) return 0.0f;
    const float g = c->agc.gain > 1e-6f ? c->agc.gain : 1e-6f;
    return 20.0f * log10f(g);
}

void dsp_chain_set_trim(dsp_chain_t *c, int32_t ppm)
{
    dsp_resampler_set_trim(&c->rs, ppm);
}

int dsp_chain_process(dsp_chain_t *c, const int16_t *in, int n_in,
                      int16_t *out, int out_max)
{
    int n_out = 0;
    float peak = 0.0f;

    for (int i = 0; i < n_in; i++) {
        float x = (float)in[i] * (1.0f / INT16_SCALE);

        /* Filter at the input rate: everything above the cutoff must be gone
         * before decimation folds it back into the audio band. */
        x = dsp_dcblock_process(&c->dc, x);
        x = dsp_lpf_process(&c->lpf, x);

        float res[8];
        const int nr = dsp_resampler_push(&c->rs, x, res,
                                          (out_max - n_out) < 8
                                              ? (out_max - n_out) : 8);
        for (int k = 0; k < nr; k++) {
            /* Gain, then limit, at the output rate: the limiter's time
             * constants are designed against fs_out, and interpolation can
             * overshoot slightly, so it must see the interpolated samples. */
            /* Fixed gain, then the AGC rides the average, then the
             * limiter catches what is left. Order matters: the limiter must
             * be last or the AGC would undo its work. */
            float y = dsp_agc_process(&c->agc, res[k] * c->gain);
            y = dsp_limiter_process(&c->lim, y);

            const float ay = fabsf(y);
            if (ay > peak) peak = ay;

            y = clampf(y, -1.0f, 1.0f);
            int v = (int)lrintf(y * 32767.0f);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            out[n_out++] = (int16_t)v;
        }
        if (n_out >= out_max) break;
    }

    c->peak = peak;
    return n_out;
}

void dsp_downmix(const int16_t *in, int frames, int channels, int16_t *out)
{
    if (channels <= 1) {
        memmove(out, in, (size_t)frames * sizeof(int16_t));
        return;
    }
    for (int i = 0; i < frames; i++) {
        /* Sum then halve in 32-bit: (l + r) >> 1 on int16 would overflow on
         * correlated full-scale material, which is most of it. */
        const int32_t l = in[(size_t)i * channels];
        const int32_t r = in[(size_t)i * channels + 1];
        out[i] = (int16_t)((l + r) >> 1);
    }
}

/* ------------------------------------------------------------- duty table */

/* ------------------------------------------------------------------- AGC */

/* One-pole coefficient for a time constant in milliseconds. */
static float pole_for(float fs_hz, float ms)
{
    if (ms <= 0.0f) return 1.0f;
    return 1.0f - expf(-1.0f / (fs_hz * ms * 0.001f));
}

void dsp_agc_init(dsp_agc_t *g, float fs_hz, float target,
                  float min_db, float max_db,
                  float attack_ms, float release_ms, float gate_db)
{
    memset(g, 0, sizeof *g);
    g->target = clampf(target, 0.01f, 0.9f);
    g->gain = 1.0f;
    g->min_gain = powf(10.0f, min_db / 20.0f);
    g->max_gain = powf(10.0f, max_db / 20.0f);
    g->a_rms = pole_for(fs_hz, 50.0f);      /* 50 ms detector window */
    g->a_down = pole_for(fs_hz, attack_ms);
    g->a_up = pole_for(fs_hz, release_ms);
    const float gate = powf(10.0f, gate_db / 20.0f);
    g->gate2 = gate * gate;
    g->rms2 = g->target * g->target;        /* start settled, not winding up */
    g->enabled = 0;
}

void dsp_agc_set_target(dsp_agc_t *g, float target)
{
    g->target = clampf(target, 0.01f, 0.9f);
}

void dsp_agc_set_enabled(dsp_agc_t *g, int on)
{
    if (!on && g->enabled) g->gain = 1.0f;   /* leave nothing applied */
    g->enabled = on ? 1 : 0;
}

float dsp_agc_process(dsp_agc_t *g, float x)
{
    if (!g->enabled) return x;

    /* Detector runs on the input to the AGC, not its output, so the loop
     * cannot chase its own tail. */
    g->rms2 += g->a_rms * (x * x - g->rms2);

    /* Below the gate the programme has stopped: a pause between words, or a
     * track change. Freeze rather than lift the noise floor. */
    if (g->rms2 > g->gate2) {
        const float rms = sqrtf(g->rms2);
        float want = g->target / (rms > 1e-6f ? rms : 1e-6f);
        want = clampf(want, g->min_gain, g->max_gain);

        /* Down fast, up slow. */
        const float a = (want < g->gain) ? g->a_down : g->a_up;
        g->gain += a * (want - g->gain);
    }

    return x * g->gain;
}

void dsp_build_duty_table(uint16_t *tbl, int entries, int res_bits,
                          float depth, float level)
{
    level = clampf(level, 0.02f, 1.0f);
    const int duty_full = 1 << res_bits;          /* 256 at 8 bits */
    const float a0 = 1.0f / (1.0f + depth);       /* unmodulated amplitude */

    /* Never let the pulse vanish or fill the period: either is a DC level,
     * not a carrier, and LEDC treats 0 and duty_full as static output. */
    const int32_t q_min = 1 * 256;
    const int32_t q_max = (int32_t)(duty_full - 1) * 256;

    for (int i = 0; i < entries; i++) {
        const float s = (entries > 1)
                            ? (2.0f * (float)i / (float)(entries - 1) - 1.0f)
                            : 0.0f;
        const float a = clampf(level * a0 * (1.0f + depth * s), 0.0f, 1.0f);
        const float d = asinf(a) / (float)M_PI;   /* 0 .. 0.5 */

        int32_t q = (int32_t)lrintf(d * (float)duty_full * 256.0f);
        if (q < q_min) q = q_min;
        if (q > q_max) q = q_max;
        tbl[i] = (uint16_t)q;
    }
}

float dsp_duty_q8_to_amplitude(uint16_t duty_q8, int res_bits)
{
    const float duty_full = (float)(1 << res_bits);
    const float d = ((float)duty_q8 / 256.0f) / duty_full;
    return sinf((float)M_PI * d);
}
