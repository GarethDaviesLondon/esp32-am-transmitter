/* test_dsp.c: host-side unit tests for the audio chain and the duty table.
 *
 * These are the only tests in this project that can run without an ESP32-S3,
 * so they carry the weight of proving that the filter really is sharp enough,
 * that the resampler really does track its trim, and above all that the
 * arcsine duty table really does make the radiated envelope linear in the
 * audio sample (LL-02 / KI-01).
 *
 *   make -C test/host test
 */
#include "dsp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int failures = 0;
static int checks = 0;

static void check(int cond, const char *what, const char *detail)
{
    checks++;
    if (cond) {
        printf("  ok    %s\n", what);
    } else {
        failures++;
        printf("  FAIL  %s  (%s)\n", what, detail ? detail : "");
    }
}

static void check_near(double got, double want, double tol, const char *what)
{
    char buf[160];
    snprintf(buf, sizeof buf, "got %.6f want %.6f +/- %.6f", got, want, tol);
    check(fabs(got - want) <= tol, what, buf);
}

/* Steady-state amplitude of the filter at frequency f, in dB. */
static double lpf_response_db(int sections, double fc, double fs, double f)
{
    dsp_lpf_t lpf;
    dsp_lpf_init(&lpf, sections, (float)fc, (float)fs);

    const int settle = (int)(fs * 0.5);
    const int measure = (int)(fs * 0.2);
    double peak = 0.0;

    for (int n = 0; n < settle + measure; n++) {
        const double x = sin(2.0 * M_PI * f * n / fs);
        const double y = dsp_lpf_process(&lpf, (float)x);
        if (n >= settle && fabs(y) > peak) peak = fabs(y);
    }
    return 20.0 * log10(peak < 1e-12 ? 1e-12 : peak);
}

static void test_lpf(void)
{
    puts("low-pass filter (6th-order Butterworth, 4.5 kHz of 44.1 kHz)");

    check_near(lpf_response_db(3, 4500, 44100, 1000), 0.0, 0.2,
               "passband flat at 1 kHz");
    check_near(lpf_response_db(3, 4500, 44100, 4500), -3.0, 0.6,
               "-3 dB at the cutoff");

    const double at10k = lpf_response_db(3, 4500, 44100, 10000);
    char buf[80];
    snprintf(buf, sizeof buf, "%.1f dB", at10k);
    /* 10 kHz is the Nyquist of the 20 kHz modulation rate: anything left here
     * folds straight back into the audio band and out onto the carrier. */
    check(at10k < -35.0, "at least 35 dB down at the decimation Nyquist", buf);

    /* The 2nd-order design the brief specified, for the record. */
    const double order2 = lpf_response_db(1, 4500, 44100, 10000);
    snprintf(buf, sizeof buf, "2nd order manages only %.1f dB", order2);
    check(order2 > at10k + 20.0, "6th order beats 2nd order by 20 dB+", buf);
}

static void test_dcblock(void)
{
    puts("DC blocker");

    dsp_dcblock_t d;
    dsp_dcblock_init(&d, 25.0f, 44100.0f);

    double y = 0.0;
    for (int n = 0; n < 44100; n++) y = dsp_dcblock_process(&d, 0.5f);
    check_near(y, 0.0, 0.01, "removes a steady 0.5 offset within a second");

    /* A 1 kHz tone must survive untouched. */
    dsp_dcblock_init(&d, 25.0f, 44100.0f);
    double peak = 0.0;
    for (int n = 0; n < 44100; n++) {
        const double x = sin(2.0 * M_PI * 1000.0 * n / 44100.0);
        const double v = dsp_dcblock_process(&d, (float)x);
        if (n > 4410 && fabs(v) > peak) peak = fabs(v);
    }
    check_near(peak, 1.0, 0.01, "passes 1 kHz unchanged");
}

static void test_resampler(void)
{
    puts("resampler");

    dsp_resampler_t r;
    dsp_resampler_init(&r, 44100.0f, 20000.0f);

    float out[8];
    int n_out = 0;
    const int n_in = 44100;
    for (int i = 0; i < n_in; i++) n_out += dsp_resampler_push(&r, 0.0f, out, 8);

    const double want = n_in * 20000.0 / 44100.0;
    char buf[96];
    snprintf(buf, sizeof buf, "got %d want ~%.0f", n_out, want);
    check(fabs(n_out - want) < 4.0, "44.1 kHz in gives 20 kHz out", buf);

    /* Trim direction: a positive trim lengthens the step, so each output
     * consumes more input, so fewer outputs come back. That is what the
     * decode task needs when the FIFO is running full. */
    dsp_resampler_init(&r, 44100.0f, 20000.0f);
    dsp_resampler_set_trim(&r, 5000);
    int n_fast = 0;
    for (int i = 0; i < n_in; i++) n_fast += dsp_resampler_push(&r, 0.0f, out, 8);
    snprintf(buf, sizeof buf, "%d vs %d", n_fast, n_out);
    check(n_fast < n_out, "a positive trim produces fewer output samples", buf);

    const double ratio = (double)(n_out - n_fast) / n_out;
    check_near(ratio, 0.005, 0.0006, "5000 ppm of trim moves the rate by 0.5%");

    /* Interpolation accuracy: a slow ramp must come back as the same ramp. */
    dsp_resampler_init(&r, 44100.0f, 20000.0f);
    double worst = 0.0;
    for (int i = 0; i < 2000; i++) {
        const double x = i / 2000.0;
        const int n = dsp_resampler_push(&r, (float)x, out, 8);
        for (int k = 0; k < n; k++) {
            /* The emitted sample lags the input by up to one input period. */
            const double err = fabs(out[k] - x);
            if (i > 10 && err > worst) worst = err;
        }
    }
    check(worst < 1.5 / 2000.0, "linear interpolation tracks a ramp", NULL);
}

static void test_limiter(void)
{
    puts("limiter");

    dsp_limiter_t l;
    dsp_limiter_init(&l, 0.95f, 1.0f, 250.0f, 20000.0f);

    double peak = 0.0;
    for (int n = 0; n < 20000; n++) {
        /* 4x over the ceiling: the worst a hot stream plus 140% gain does. */
        const double x = 4.0 * sin(2.0 * M_PI * 1000.0 * n / 20000.0);
        const double y = dsp_limiter_process(&l, (float)x);
        if (n > 2000 && fabs(y) > peak) peak = fabs(y);
    }
    char buf[64];
    snprintf(buf, sizeof buf, "peak %.4f", peak);
    check(peak <= 0.96, "holds a 4x overdrive at or below the ceiling", buf);

    /* Quiet material must come through untouched: a limiter that always acts
     * is a compressor, and on AM that means a permanently pinched carrier. */
    dsp_limiter_init(&l, 0.95f, 1.0f, 250.0f, 20000.0f);
    peak = 0.0;
    for (int n = 0; n < 20000; n++) {
        const double x = 0.3 * sin(2.0 * M_PI * 1000.0 * n / 20000.0);
        const double y = dsp_limiter_process(&l, (float)x);
        if (n > 2000 && fabs(y) > peak) peak = fabs(y);
    }
    check_near(peak, 0.3, 0.01, "leaves a 0.3 signal alone");
}

/* Run a steady sine of amplitude `amp` through an AGC for `secs` and report
 * the RMS of the last tenth of it, by which time the loop has settled. */
static double agc_settled_rms(dsp_agc_t *g, double amp, double secs)
{
    const double fs = 20000.0;
    const int n = (int)(fs * secs);
    const int from = n - n / 10;
    double sum = 0.0;
    int count = 0;

    for (int i = 0; i < n; i++) {
        const double x = amp * sin(2.0 * M_PI * 700.0 * i / fs);
        const double y = dsp_agc_process(g, (float)x);
        if (i >= from) { sum += y * y; count++; }
    }
    return sqrt(sum / (count > 0 ? count : 1));
}

static void test_agc(void)
{
    puts("loudness AGC");

    const double target = 0.25;
    dsp_agc_t g;

    /* A quiet station gets lifted towards the target. This is the whole
     * point: Radio Swiss Jazz averaged 10.8 dB below Jazz Blues on the same
     * settings, and a peak limiter cannot close that gap. */
    dsp_agc_init(&g, 20000.0f, (float)target, -6.0f, 18.0f, 200.0f, 3000.0f, -50.0f);
    dsp_agc_set_enabled(&g, 1);
    double rms = agc_settled_rms(&g, 0.05, 30.0);
    check(rms > 0.15, "lifts a quiet source towards the target", NULL);

    /* A loud one gets pulled down rather than left to slam the limiter. */
    dsp_agc_init(&g, 20000.0f, (float)target, -6.0f, 18.0f, 200.0f, 3000.0f, -50.0f);
    dsp_agc_set_enabled(&g, 1);
    rms = agc_settled_rms(&g, 0.9, 30.0);
    check(rms < 0.75, "pulls a loud source down", NULL);

    /* Two very different sources must end up close to each other. That is the
     * measurable form of "every station sounds equally loud". */
    dsp_agc_init(&g, 20000.0f, (float)target, -6.0f, 18.0f, 200.0f, 3000.0f, -50.0f);
    dsp_agc_set_enabled(&g, 1);
    const double quiet = agc_settled_rms(&g, 0.05, 30.0);
    dsp_agc_init(&g, 20000.0f, (float)target, -6.0f, 18.0f, 200.0f, 3000.0f, -50.0f);
    dsp_agc_set_enabled(&g, 1);
    const double loud = agc_settled_rms(&g, 0.5, 30.0);
    const double ratio_db = 20.0 * log10(loud / quiet);
    char buf[64];
    snprintf(buf, sizeof buf, "%.1f dB apart", ratio_db);
    check(fabs(ratio_db) < 6.0,
          "a 20 dB source difference comes out under 6 dB apart", buf);

    /* The gain is bounded. Without this, silence between tracks would wind it
     * up until the next note arrived at full scale. */
    dsp_agc_init(&g, 20000.0f, (float)target, -6.0f, 18.0f, 200.0f, 3000.0f, -50.0f);
    dsp_agc_set_enabled(&g, 1);
    agc_settled_rms(&g, 0.0005, 30.0);
    check(g.gain <= g.max_gain + 1e-6f, "gain stays inside its ceiling", NULL);

    /* The gate. Once the detector is below it the gain must not move at all:
     * on speech, every pause would otherwise lift the noise floor between
     * words. Note the gate cannot stop the ramp on the way DOWN to silence,
     * only hold the gain once there, which is the behaviour that matters. */
    dsp_agc_init(&g, 20000.0f, (float)target, -6.0f, 18.0f, 200.0f, 3000.0f, -50.0f);
    dsp_agc_set_enabled(&g, 1);
    g.rms2 = 0.0f;              /* already below the gate */
    g.gain = 1.0f;
    agc_settled_rms(&g, 0.000001, 5.0);
    check(fabsf(g.gain - 1.0f) < 1e-6f,
          "freezes below the gate rather than winding up", NULL);

    /* Off must mean off, not "unity gain by coincidence". */
    dsp_agc_init(&g, 20000.0f, (float)target, -6.0f, 18.0f, 200.0f, 3000.0f, -50.0f);
    dsp_agc_set_enabled(&g, 0);
    rms = agc_settled_rms(&g, 0.05, 5.0);
    check_near(rms, 0.05 / sqrt(2.0), 0.002, "passes through untouched when off");
}

static void test_carrier_level(void)
{
    puts("carrier level");

    enum { N = 512 };
    static uint16_t full[N], half[N];
    dsp_build_duty_table(full, N, 8, 0.7f, 1.0f);
    dsp_build_duty_table(half, N, 8, 0.7f, 0.5f);

    /* Level scales the radiated envelope, so the peak amplitude at half level
     * must be half. This is the property that makes it a power control rather
     * than just another gain. */
    const float a_full = dsp_duty_q8_to_amplitude(full[N - 1], 8);
    const float a_half = dsp_duty_q8_to_amplitude(half[N - 1], 8);
    char buf[64];
    snprintf(buf, sizeof buf, "%.3f vs %.3f", a_full, a_half);
    check_near(a_half / a_full, 0.5, 0.02, "half level halves the peak amplitude");

    /* And the modulation depth must survive the change: turning the power
     * down must not also turn the modulation down. */
    const float lo_f = dsp_duty_q8_to_amplitude(full[0], 8);
    const float lo_h = dsp_duty_q8_to_amplitude(half[0], 8);
    const double m_full = (a_full - lo_f) / (a_full + lo_f);
    const double m_half = (a_half - lo_h) / (a_half + lo_h);
    snprintf(buf, sizeof buf, "%.3f vs %.3f", m_full, m_half);
    check_near(m_half, m_full, 0.02, "modulation depth is unchanged by level");
}

static void test_duty_table(void)
{
    puts("arcsine duty table (the fix for LL-02 / KI-01)");

    enum { N = 512 };
    static uint16_t tbl[N];
    const float depth = 0.70f;
    dsp_build_duty_table(tbl, N, 8, depth, 1.0f);

    int monotonic = 1;
    for (int i = 1; i < N; i++) if (tbl[i] < tbl[i - 1]) monotonic = 0;
    check(monotonic, "duty rises monotonically with the sample", NULL);

    /* The positive peak sits at 50% duty, the most a square wave can radiate. */
    check_near(tbl[N - 1] / 256.0, 128.0, 0.6, "peak lands on 50% duty");

    /* Unmodulated carrier at 1/(1+m) of the peak amplitude. */
    const float a_mid = dsp_duty_q8_to_amplitude(tbl[N / 2], 8);
    check_near(a_mid, 1.0 / (1.0 + depth), 0.002,
               "quiescent carrier at 1/(1+m) of full amplitude");

    /* The whole point: radiated amplitude linear in the audio sample. */
    double worst = 0.0;
    for (int i = 0; i < N; i++) {
        const double s = 2.0 * i / (double)(N - 1) - 1.0;
        const double want = (1.0 + depth * s) / (1.0 + depth);
        const double got = dsp_duty_q8_to_amplitude(tbl[i], 8);
        if (fabs(got - want) > worst) worst = fabs(got - want);
    }
    char buf[80];
    snprintf(buf, sizeof buf, "worst deviation %.5f of full scale", worst);
    check(worst < 0.004, "envelope is linear in the sample to better than 0.4%",
          buf);

    /* Modulation depth is what it says on the tin. */
    const double a_hi = dsp_duty_q8_to_amplitude(tbl[N - 1], 8);
    const double a_lo = dsp_duty_q8_to_amplitude(tbl[0], 8);
    const double m = (a_hi - a_lo) / (a_hi + a_lo);
    check_near(m, depth, 0.005, "measured modulation depth matches the setting");

    /* And the counter-example, so the reason for all this is on the record:
     * modulating +/- 25% of a period about 50% duty barely moves the
     * radiated amplitude at all. */
    const double naive_hi = sin(M_PI * 0.75);   /* 75% duty */
    const double naive_lo = sin(M_PI * 0.25);   /* 25% duty */
    snprintf(buf, sizeof buf, "%.3f vs %.3f, i.e. no modulation at all",
             naive_hi, naive_lo);
    check(fabs(naive_hi - naive_lo) < 1e-6,
          "the brief's 50%-centred scheme is confirmed flat", buf);

    /* Depth must never exceed the safe ceiling by construction. */
    static uint16_t tbl75[N];
    dsp_build_duty_table(tbl75, N, 8, 0.75f, 1.0f);
    check(tbl75[0] > 0 && tbl75[N - 1] <= 255 * 256,
          "duty stays inside the LEDC register range at 75% depth", NULL);
}

static void test_chain(void)
{
    puts("end-to-end chain");

    dsp_chain_t c;
    dsp_chain_init(&c, 44100.0f, 20000.0f, 4500.0f, 3, 0.95f, 1.0f, 250.0f);
    dsp_chain_set_gain(&c, 1.0f);

    enum { NIN = 4410 };            /* 100 ms */
    static int16_t in[NIN];
    static int16_t out[NIN];

    for (int i = 0; i < NIN; i++) {
        in[i] = (int16_t)lrint(20000.0 * sin(2.0 * M_PI * 1000.0 * i / 44100.0));
    }

    int total = 0;
    /* Feed it in decoder-sized blocks, as the real decode task does. */
    for (int off = 0; off < NIN; off += 1152) {
        const int n = (NIN - off) < 1152 ? (NIN - off) : 1152;
        total += dsp_chain_process(&c, in + off, n, out + total,
                                   (int)(sizeof out / sizeof out[0]) - total);
    }

    const double want = NIN * 20000.0 / 44100.0;
    char buf[96];
    snprintf(buf, sizeof buf, "got %d want ~%.0f", total, want);
    check(fabs(total - want) < 8.0, "block-by-block rate is right", buf);

    /* Amplitude survives: 20000/32768 in, roughly the same out. The first
     * few ms are filter settling, so measure the tail. */
    double peak = 0.0;
    for (int i = total / 2; i < total; i++) {
        const double v = fabs(out[i] / 32768.0);
        if (v > peak) peak = v;
    }
    check_near(peak, 20000.0 / 32768.0, 0.03, "1 kHz passes at the right level");

    /* A 12 kHz tone must be gone: on the air it would be splatter. */
    dsp_chain_init(&c, 44100.0f, 20000.0f, 4500.0f, 3, 0.95f, 1.0f, 250.0f);
    for (int i = 0; i < NIN; i++) {
        in[i] = (int16_t)lrint(20000.0 * sin(2.0 * M_PI * 12000.0 * i / 44100.0));
    }
    total = dsp_chain_process(&c, in, NIN, out,
                              (int)(sizeof out / sizeof out[0]));
    peak = 0.0;
    for (int i = total / 2; i < total; i++) {
        const double v = fabs(out[i] / 32768.0);
        if (v > peak) peak = v;
    }
    snprintf(buf, sizeof buf, "peak %.5f of full scale", peak);
    check(peak < 0.01, "12 kHz is rejected by more than 35 dB", buf);
}

static void test_downmix(void)
{
    puts("downmix");

    const int16_t st[8] = { 100, 200, -100, 100, 32767, 32767, -32768, -32768 };
    int16_t mono[4];
    dsp_downmix(st, 4, 2, mono);

    check(mono[0] == 150, "averages two channels", NULL);
    check(mono[1] == 0, "handles opposite signs", NULL);
    check(mono[2] == 32767, "full-scale correlated material does not wrap",
          NULL);
    check(mono[3] == -32768, "and neither does the negative rail", NULL);

    int16_t copy[4];
    dsp_downmix(st, 4, 1, copy);
    check(copy[0] == 100 && copy[3] == 100, "mono input is a straight copy",
          NULL);
}

int main(void)
{
    test_lpf();
    test_dcblock();
    test_resampler();
    test_limiter();
    test_agc();
    test_duty_table();
    test_carrier_level();
    test_chain();
    test_downmix();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
