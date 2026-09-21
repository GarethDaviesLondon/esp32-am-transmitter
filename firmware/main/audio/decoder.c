/* decoder.c: see decoder.h. */

#include "decoder.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "am_config.h"
#include "dsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mp3dec.h"
#include "net/stream.h"
#include "rf/carrier.h"

static const char *TAG = "decoder";

/* Longest MP3 frame is about 1440 bytes; two of them plus slack means a
 * decode never fails purely because the frame straddled a read. */
#define IN_BUF_BYTES  4096
/* Layer III gives at most 1152 samples per channel per frame. */
#define PCM_SAMPLES   (1152 * 2)
/* Output at 20 kHz from at most 1152 input samples: fewer than the input, but
 * size for the upsampling case (a 16 kHz stream) with room to spare. */
#define OUT_SAMPLES   2048

/* One of these per chain. The buffers were file statics; with two decode
 * tasks running concurrently they have to be per-context or the two would
 * corrupt each other.
 *
 * They are heap pointers into PSRAM rather than arrays in .bss, and that is
 * not a micro-optimisation. As statics they were about 15 kB per chain, 30 kB
 * of internal DRAM between them, and internal DRAM is the scarce resource on
 * this board once PSRAM is enabled: PSRAM reserves 32 kB of it for DMA, Wi-Fi
 * wants its share, and mbedTLS wants a great deal more. Internal free had
 * fallen to about 5 kB, which is how station discovery started failing with
 * MBEDTLS_ERR_SSL_ALLOC_FAILED while the reported heap still read 8 MB.
 *
 * None of this is touched by an ISR. The decode task has a 26 ms budget per
 * frame and is using a fraction of it, so PSRAM's latency is affordable here
 * in a way it would never be on the modulation path. */
typedef struct {
    int ch;
    HMP3Decoder mp3;
    dsp_chain_t chain;

    uint8_t *in;
    int in_len;
    int16_t *pcm;
    int16_t *mono;
    int16_t *out;

    int rate;
    int channels;
    int bitrate;
    uint32_t frames, errors, resyncs;
    int32_t trim_ppm;
    float tone_phase;

    volatile int lpf_hz;
    volatile int gain_pct;
    volatile bool tone_on;
    volatile int tone_hz;
    volatile bool agc_on;
    volatile int agc_target_pct;
    volatile bool reconfigure;
} dec_ctx_t;

static dec_ctx_t s_dec[RF_CHAINS];

static inline bool chain_ok(int ch) { return ch >= 0 && ch < RF_CHAINS; }

/* ------------------------------------------------------ the fill controller */

/* The station's clock and this board's crystal are independent. Over minutes
 * they drift by tens of ppm, which is enough to walk the FIFO to one end and
 * click. Nudge the resampler so the FIFO sits at its target instead.
 *
 * Proportional only: the plant is a pure integrator, the disturbance is a
 * constant offset, and the loop runs once per decoded frame (about 40 times a
 * second). Integral action would buy nothing but overshoot. */
static int32_t compute_trim_ppm(uint32_t fill)
{
    const int32_t target = SAMPLE_FIFO_TARGET;
    const int32_t error = (int32_t)fill - target;

    /* Full correction when the FIFO is a whole target away from where it
     * should be. */
    int32_t ppm = (error * RESAMPLER_TRIM_PPM_MAX) / target;
    if (ppm > RESAMPLER_TRIM_PPM_MAX) ppm = RESAMPLER_TRIM_PPM_MAX;
    if (ppm < -RESAMPLER_TRIM_PPM_MAX) ppm = -RESAMPLER_TRIM_PPM_MAX;
    return ppm;
}

static void reconfigure_chain(dec_ctx_t *d, int rate)
{
    dsp_chain_init(&d->chain, (float)rate, (float)AM_SAMPLE_RATE_HZ,
                   (float)d->lpf_hz, AUDIO_LPF_SECTIONS, AUDIO_LIMIT_CEILING,
                   AUDIO_LIMIT_ATTACK_MS, AUDIO_LIMIT_RELEASE_MS);
    dsp_chain_set_gain(&d->chain, (float)d->gain_pct / 100.0f);
    dsp_chain_set_agc(&d->chain, d->agc_on ? 1 : 0,
                      (float)d->agc_target_pct / 100.0f);
    d->reconfigure = false;
    ESP_LOGI(TAG, "[%d] chain: %d Hz in, %d Hz out, %d Hz low-pass, gain %d%%",
             d->ch, rate, AM_SAMPLE_RATE_HZ, d->lpf_hz, d->gain_pct);
}

/* ------------------------------------------------------------ input buffer */

/* Top the assembly buffer up from the ring buffer. Returns bytes added. */
static int refill(dec_ctx_t *d, uint32_t timeout_ms)
{
    if (d->in_len >= IN_BUF_BYTES) return 0;

    size_t got = 0;
    const uint8_t *chunk = stream_acquire(d->ch,
                                          (size_t)(IN_BUF_BYTES - d->in_len),
                                          timeout_ms, &got);
    if (!chunk) return 0;

    memcpy(d->in + d->in_len, chunk, got);
    d->in_len += (int)got;
    stream_release(d->ch, chunk);
    return (int)got;
}

static void consume(dec_ctx_t *d, int bytes)
{
    if (bytes <= 0) return;
    if (bytes >= d->in_len) {
        d->in_len = 0;
        return;
    }
    memmove(d->in, d->in + bytes, (size_t)(d->in_len - bytes));
    d->in_len -= bytes;
}

/* ------------------------------------------------------------- generators */

/* Push samples, waiting for room rather than dropping. The FIFO holds 100 ms
 * and the modulator drains it at a fixed rate, so this settles into pacing
 * the decode task off the 20 kHz timer, which is exactly what we want: it is
 * the modulator's clock that is real, not the network's. */
static void push_all(dec_ctx_t *d, const int16_t *samples, int n)
{
    int off = 0;
    while (off < n) {
        const uint32_t took =
            carrier_push(d->ch, samples + off, (uint32_t)(n - off));
        off += (int)took;
        if (off < n) vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static void generate_silence(dec_ctx_t *d, int n)
{
    static const int16_t zeros[256] = { 0 };   /* read-only, safe to share */
    while (n > 0) {
        const int block = n < 256 ? n : 256;
        push_all(d, zeros, block);
        n -= block;
    }
}

static void generate_tone(dec_ctx_t *d, int n)
{
    const float step = 2.0f * (float)M_PI * (float)d->tone_hz /
                       (float)AM_SAMPLE_RATE_HZ;

    while (n > 0) {
        const int block = n < 256 ? n : 256;
        for (int i = 0; i < block; i++) {
            d->out[i] = (int16_t)lrintf(24000.0f * sinf(d->tone_phase));
            d->tone_phase += step;
            if (d->tone_phase > 2.0f * (float)M_PI) {
                d->tone_phase -= 2.0f * (float)M_PI;
            }
        }
        push_all(d, d->out, block);
        n -= block;
    }
}

/* --------------------------------------------------------------- the task */

static void decode_one_frame(dec_ctx_t *d)
{
    const int sync = MP3FindSyncWord(d->in, d->in_len);
    if (sync < 0) {
        /* No frame header anywhere in the buffer. Keep the tail: the header
         * may straddle the boundary. */
        d->resyncs++;
        consume(d, d->in_len > 3 ? d->in_len - 3 : 0);
        return;
    }
    if (sync > 0) {
        consume(d, sync);
    }

    unsigned char *ptr = d->in;
    int bytes_left = d->in_len;

    const int err = MP3Decode(d->mp3, &ptr, &bytes_left, d->pcm, 0);
    if (err != ERR_MP3_NONE) {
        if (err == ERR_MP3_INDATA_UNDERFLOW) {
            return;      /* partial frame; come back when more has arrived */
        }
        /* A corrupt frame: step over its sync word so the next search does
         * not find the same one again, and carry on. Dropping 20 ms of audio
         * beats resetting the decoder. */
        d->errors++;
        consume(d, 2);
        return;
    }

    consume(d, d->in_len - bytes_left);

    MP3FrameInfo info;
    MP3GetLastFrameInfo(d->mp3, &info);
    d->frames++;
    d->bitrate = info.bitrate;

    if (info.samprate != d->rate || info.nChans != d->channels ||
        d->reconfigure) {
        d->rate = info.samprate;
        d->channels = info.nChans;
        reconfigure_chain(d, d->rate);
    }

    const int frames = info.outputSamps / (info.nChans > 0 ? info.nChans : 1);
    if (frames <= 0) return;

    dsp_downmix(d->pcm, frames, info.nChans, d->mono);

    /* Retune the resampler before processing, so the correction is applied to
     * this block rather than a block late. Each chain tracks its own FIFO:
     * two stations drift against this crystal independently. */
    carrier_stats_t cs;
    carrier_get_stats(d->ch, &cs);
    d->trim_ppm = compute_trim_ppm(cs.fifo_count);
    dsp_chain_set_trim(&d->chain, d->trim_ppm);

    const int n = dsp_chain_process(&d->chain, d->mono, frames, d->out,
                                    OUT_SAMPLES);
    push_all(d, d->out, n);
}

static void decode_task(void *arg)
{
    dec_ctx_t *d = arg;

    /* libhelix wants internal RAM for its tables and will not take PSRAM. Two
     * instances is the real cost of a second chain on this core. */
    d->mp3 = MP3InitDecoder();
    if (!d->mp3) {
        ESP_LOGE(TAG, "[%d] libhelix would not initialise: out of internal RAM",
                 d->ch);
        vTaskDelete(NULL);
        return;
    }
    reconfigure_chain(d, d->rate);

    for (;;) {
        if (d->reconfigure) reconfigure_chain(d, d->rate);

        if (d->tone_on) {
            generate_tone(d, 256);
            continue;
        }

        if (!stream_is_playing(d->ch)) {
            /* Keep the modulator fed so the carrier sits clean and
             * unmodulated rather than decaying through the underrun path. */
            d->in_len = 0;
            generate_silence(d, 256);
            continue;
        }

        refill(d, 20);

        /* Below one maximum frame there may be nothing decodable yet; asking
         * the decoder anyway just burns a sync search. */
        if (d->in_len < 1600) {
            if (refill(d, 20) == 0 && d->in_len < 512) {
                generate_silence(d, 64);
            }
            continue;
        }

        decode_one_frame(d);
    }
}

/* ------------------------------------------------------------------- API */

esp_err_t decoder_init(void)
{
    /* Both decode tasks are pinned to core 1, alongside the modulation ISR.
     * That is required, not incidental: each sample FIFO is a lockless
     * single-producer, single-consumer queue whose contract assumes the
     * producer and the ISR share a core. */
    for (int ch = 0; ch < RF_CHAINS; ch++) {
        dec_ctx_t *d = &s_dec[ch];
        d->ch = ch;
        d->rate = 44100;
        d->channels = 2;
        d->lpf_hz = AUDIO_LPF_CUTOFF_HZ;
        d->gain_pct = AUDIO_GAIN_PERCENT_DEFAULT;
        d->tone_hz = 1000;
        d->agc_on = true;
        d->agc_target_pct = AUDIO_AGC_TARGET_PERCENT;
        d->reconfigure = true;

        /* PSRAM first, internal only if the board has none. */
        d->in = heap_caps_malloc(IN_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        d->pcm = heap_caps_malloc(sizeof(int16_t) * PCM_SAMPLES,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        d->mono = heap_caps_malloc(sizeof(int16_t) * (PCM_SAMPLES / 2),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        d->out = heap_caps_malloc(sizeof(int16_t) * OUT_SAMPLES,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!d->in || !d->pcm || !d->mono || !d->out) {
            ESP_LOGW(TAG, "[%d] no PSRAM for the decode buffers, using DRAM",
                     ch);
            if (!d->in) d->in = malloc(IN_BUF_BYTES);
            if (!d->pcm) d->pcm = malloc(sizeof(int16_t) * PCM_SAMPLES);
            if (!d->mono) d->mono = malloc(sizeof(int16_t) * (PCM_SAMPLES / 2));
            if (!d->out) d->out = malloc(sizeof(int16_t) * OUT_SAMPLES);
        }
        if (!d->in || !d->pcm || !d->mono || !d->out) return ESP_ERR_NO_MEM;

        char name[12];
        snprintf(name, sizeof name, "decode%d", ch);
        if (xTaskCreatePinnedToCore(decode_task, name, TASK_STACK_DECODE, d,
                                    TASK_PRIO_DECODE, NULL,
                                    TASK_CORE_AUDIO) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

void decoder_set_lpf(int ch, int cutoff_hz)
{
    if (!chain_ok(ch)) return;
    if (cutoff_hz < 500) cutoff_hz = 500;
    if (cutoff_hz > AM_SAMPLE_RATE_HZ / 2 - 500) {
        cutoff_hz = AM_SAMPLE_RATE_HZ / 2 - 500;
    }
    s_dec[ch].lpf_hz = cutoff_hz;
    s_dec[ch].reconfigure = true;
}

void decoder_set_gain(int ch, int percent)
{
    if (!chain_ok(ch)) return;
    if (percent < 0) percent = 0;
    if (percent > 800) percent = 800;
    s_dec[ch].gain_pct = percent;
    s_dec[ch].reconfigure = true;
}

void decoder_set_agc(int ch, bool on, int target_pct)
{
    if (!chain_ok(ch)) return;
    if (target_pct < 5) target_pct = 5;
    if (target_pct > 90) target_pct = 90;
    s_dec[ch].agc_on = on;
    s_dec[ch].agc_target_pct = target_pct;
    s_dec[ch].reconfigure = true;
}

void decoder_set_tone(int ch, bool on, int tone_hz)
{
    if (!chain_ok(ch)) return;
    if (tone_hz >= 50 && tone_hz <= 5000) s_dec[ch].tone_hz = tone_hz;
    s_dec[ch].tone_on = on;
    ESP_LOGI(TAG, "[%d] test tone %s (%d Hz)", ch, on ? "on" : "off",
             s_dec[ch].tone_hz);
}

bool decoder_tone_active(int ch)
{
    return chain_ok(ch) ? s_dec[ch].tone_on : false;
}

void decoder_get_stats(int ch, decoder_stats_t *out)
{
    memset(out, 0, sizeof *out);
    if (!chain_ok(ch)) return;
    const dec_ctx_t *d = &s_dec[ch];

    out->sample_rate = d->rate;
    out->channels = d->channels;
    out->bitrate = d->bitrate;
    out->frames = d->frames;
    out->errors = d->errors;
    out->resyncs = d->resyncs;
    out->trim_ppm = d->trim_ppm;
    out->agc_db = dsp_chain_agc_gain_db(&d->chain);
    out->peak = d->chain.peak;
}
