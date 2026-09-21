/* carrier.c: see carrier.h. */

#include "carrier.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "am_config.h"
#include "audio/dsp.h"
#include "audio/sample_fifo.h"

#include "driver/gptimer.h"
#include "esp_timer.h"
#include "driver/ledc.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "hal/ledc_ll.h"
#include "soc/ledc_struct.h"

static const char *TAG = "carrier";

#define RF_LEDC_MODE LEDC_LOW_SPEED_MODE   /* the ESP32-S3 has no high-speed mode */

/* LEDC_USE_APB_CLK is 80 MHz on the ESP32-S3. Named here rather than pulled
 * from a soc header so the reachability check below cannot silently follow a
 * clock-source change made elsewhere. */
#define RF_LEDC_SRC_HZ 80000000ULL

#define DUTY_FULL (1u << RF_DUTY_RES_BITS)    /* 256 at 8 bits */
#define DUTY_MIN  1u
#define DUTY_MAX  (DUTY_FULL - 1u)

/* Sample-to-table-index shift. int16 + 32768 spans 16 bits; the table has
 * AM_DUTY_TABLE_ENTRIES entries, so throw away the bottom bits. The table is
 * finer than the 8-bit duty register, so no interpolation is needed. */
#if AM_DUTY_TABLE_ENTRIES == 512
#define DUTY_IDX_SHIFT 7
#elif AM_DUTY_TABLE_ENTRIES == 256
#define DUTY_IDX_SHIFT 8
#elif AM_DUTY_TABLE_ENTRIES == 1024
#define DUTY_IDX_SHIFT 6
#else
#error "AM_DUTY_TABLE_ENTRIES must be 256, 512 or 1024"
#endif

/* ------------------------------------------------------------------ state */

/* Per chain, and everything in here is touched by the ISR, so all of it lives
 * in DRAM. A flash access from this path is a crash the moment anything writes
 * flash, not a slow tick.
 *
 * Two chains cost 2 kB of DRAM for the tables plus 8 kB for the FIFOs. That
 * stays in internal memory deliberately: PSRAM is reached over SPI and must
 * never be in the modulation ISR's way. */
typedef struct {
    sample_fifo_t fifo;
    uint16_t duty_q8[AM_DUTY_TABLE_ENTRIES];
    int32_t err;             /* noise-shaping error feedback */
    int16_t last;            /* last sample, for underrun decay */
    uint32_t underruns;
    bool output_on;
    /* Flush handshake: the producer bumps the request, the ISR acts on it and
     * acknowledges. One 32-bit compare per tick. See sample_fifo_drain(). */
    volatile uint32_t flush_req;
    uint32_t flush_ack;
} chain_rt_t;

static DRAM_ATTR chain_rt_t s_rt[RF_CHAINS];

/* The parts the ISR never reads. One LEDC timer per chain, so the two
 * carriers can run at unrelated frequencies. */
typedef struct {
    ledc_timer_t timer;
    ledc_channel_t channel;
    int gpio;
    uint32_t carrier_hz;
    float depth;
    float level;             /* what the operator asked for */
    float level_now;         /* after fading: what is actually on air */
    carrier_fade_cfg_t fade;
    float fade_phase;        /* radians, wraps */
} chain_cfg_t;

static chain_cfg_t s_cfg[RF_CHAINS] = {
    { LEDC_TIMER_0, LEDC_CHANNEL_0, RF_CARRIER_GPIO,   RF_CARRIER_HZ,
      AM_MODULATION_DEPTH, 1.0f, 1.0f, { CARRIER_FADE_OFF, 200, 60 }, 0.0f },
#if RF_CHAINS > 1
    { LEDC_TIMER_1, LEDC_CHANNEL_1, RF_CARRIER_GPIO_B, RF_CARRIER_HZ_B,
      AM_MODULATION_DEPTH, 1.0f, 1.0f, { CARRIER_FADE_OFF, 200, 60 }, 0.0f },
#endif
};

static gptimer_handle_t s_timer;
static esp_timer_handle_t s_fade_timer;
static bool s_inited;
static bool s_suspended;

static inline bool chain_ok(int ch) { return ch >= 0 && ch < RF_CHAINS; }

/* ---------------------------------------------------------- the modulator */

/* Write a duty register directly. ledc_set_duty()/ledc_update_duty() are
 * not ISR-safe: they take a lock, they log, and they live in flash. The LL
 * layer is three register writes with no such baggage.
 *
 * If this fails to compile after an ESP-IDF upgrade, that is expected:
 * hal/ledc_ll.h is a private header and its signatures move between minor
 * versions. See KI-03. */
static inline void IRAM_ATTR ledc_write_duty(ledc_channel_t channel,
                                             uint32_t duty)
{
    ledc_ll_set_duty_int_part(&LEDC, RF_LEDC_MODE, channel, duty);
    ledc_ll_set_duty_start(&LEDC, RF_LEDC_MODE, channel, true);
    ledc_ll_ls_channel_update(&LEDC, RF_LEDC_MODE, channel);
}

/* One chain's work for one tick. Kept separate so the loop below reads as
 * what it is, and force-inlined so two chains cost no call overhead. */
static inline void IRAM_ATTR modulate_one(chain_rt_t *rt, ledc_channel_t channel)
{
    if (rt->flush_req != rt->flush_ack) {
        rt->flush_ack = rt->flush_req;
        sample_fifo_drain(&rt->fifo);
    }

    int16_t sample;
    if (!sample_fifo_read(&rt->fifo, &sample)) {
        rt->underruns++;
        /* Decay towards the quiescent carrier rather than jumping to it: a
         * step in the envelope is a click across the whole receiver band,
         * and underruns cluster, so the jump would repeat. About 3 ms to
         * silence at 20 kHz. */
        sample = (int16_t)(rt->last - (rt->last >> 6));
    }
    rt->last = sample;

    const uint32_t idx = ((uint32_t)((int32_t)sample + 32768)) >> DUTY_IDX_SHIFT;
    int32_t q = (int32_t)rt->duty_q8[idx];

#if RF_NOISE_SHAPING
    /* First-order error feedback. The duty register has 8 bits; the table has
     * 16. Carrying the rounding error into the next tick shapes the
     * quantisation noise away from the audio band, which is the bottom
     * quarter of the 10 kHz Nyquist. Two adds. */
    q += rt->err;
#endif

    int32_t duty = q >> 8;
    if (duty < (int32_t)DUTY_MIN) duty = (int32_t)DUTY_MIN;
    if (duty > (int32_t)DUTY_MAX) duty = (int32_t)DUTY_MAX;

#if RF_NOISE_SHAPING
    rt->err = q - (duty << 8);
    /* Bound the accumulator so a sustained clamp at either rail cannot wind
     * it up into a burst when the signal comes back. */
    if (rt->err > 4096) rt->err = 4096;
    if (rt->err < -4096) rt->err = -4096;
#endif

    if (rt->output_on) {
        ledc_write_duty(channel, (uint32_t)duty);
    }
}

/* One timer serves every chain. Two chains is two FIFO reads, two table
 * lookups and six register writes per 50 us tick, which is why a second
 * transmitter costs almost nothing here: the expensive half of a chain is the
 * decoder, and that runs in a task. */
static bool IRAM_ATTR on_modulation_tick(gptimer_handle_t timer,
                                         const gptimer_alarm_event_data_t *edata,
                                         void *user_ctx)
{
    (void)timer;
    (void)edata;
    (void)user_ctx;

    for (int ch = 0; ch < RF_CHAINS; ch++) {
        modulate_one(&s_rt[ch], s_cfg[ch].channel);
    }
    return false;   /* no task woken */
}

/* ------------------------------------------------------------------- setup */

/* The LEDC divider is 10 integer bits plus 8 fractional, minimum 1.0, so the
 * reachable frequency range at a given duty resolution is bounded at both
 * ends. Check before asking the driver, so a bad web request gets an error
 * instead of an abort. */
static bool frequency_is_reachable(uint32_t hz)
{
    if (hz == 0) return false;
    const uint64_t div = (RF_LEDC_SRC_HZ << 8) / ((uint64_t)hz * DUTY_FULL);
    return div >= 256 && div < (1 << 18);
}

static void rebuild_duty_table(int ch, float depth, float level)
{
    dsp_build_duty_table(s_rt[ch].duty_q8, AM_DUTY_TABLE_ENTRIES,
                         RF_DUTY_RES_BITS, depth, level);
}

static uint32_t quiescent_duty(int ch)
{
    return (uint32_t)(s_rt[ch].duty_q8[AM_DUTY_TABLE_ENTRIES / 2] >> 8);
}

static esp_err_t chain_init(int ch)
{
    chain_cfg_t *cfg = &s_cfg[ch];
    chain_rt_t *rt = &s_rt[ch];

    if (!frequency_is_reachable(cfg->carrier_hz)) {
        ESP_LOGE(TAG, "chain %d: %" PRIu32 " Hz is out of range at %d-bit duty",
                 ch, cfg->carrier_hz, RF_DUTY_RES_BITS);
        return ESP_ERR_INVALID_ARG;
    }

    sample_fifo_reset(&rt->fifo);
    cfg->level_now = cfg->level;
    rebuild_duty_table(ch, cfg->depth, cfg->level);
    rt->err = 0;
    rt->last = 0;
    rt->underruns = 0;

    const ledc_timer_config_t tcfg = {
        .speed_mode = RF_LEDC_MODE,
        .timer_num = cfg->timer,
        .duty_resolution = (ledc_timer_bit_t)RF_DUTY_RES_BITS,
        .freq_hz = cfg->carrier_hz,
        .clk_cfg = LEDC_USE_APB_CLK,   /* 80 MHz, and not gated by DFS */
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&tcfg), TAG, "ledc timer %d", ch);

    const ledc_channel_config_t ccfg = {
        .gpio_num = cfg->gpio,
        .speed_mode = RF_LEDC_MODE,
        .channel = cfg->channel,
        .timer_sel = cfg->timer,
        .duty = quiescent_duty(ch),
        .hpoint = 0,
        .intr_type = LEDC_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ccfg), TAG, "ledc channel %d", ch);

    rt->output_on = true;

    ESP_LOGI(TAG, "chain %d: carrier on GPIO %d at %" PRIu32 " Hz, %d-bit duty, "
                  "depth %.2f, level %.2f, quiescent duty %u/%u",
             ch, cfg->gpio, cfg->carrier_hz, RF_DUTY_RES_BITS,
             (double)cfg->depth, (double)cfg->level, (unsigned)quiescent_duty(ch),
             (unsigned)DUTY_FULL);
    return ESP_OK;
}

/* --------------------------------------------------------------- fading */

/* The level is baked into the duty table, because the table is what turns a
 * wanted amplitude into a duty cycle. So fading means rebuilding the table,
 * and that is why this runs at CARRIER_FADE_UPDATE_HZ from a task-context
 * timer rather than from the modulation ISR: the rebuild is 512 arcsines and
 * has no business anywhere near a 20 kHz interrupt.
 *
 * The ISR reads one entry per tick and every entry is individually valid, so
 * rebuilding underneath it costs at worst one sample from the old curve. Same
 * argument as a depth change, and no lock. */

/* 0..1, where 1 is full level and 0 is the bottom of the dip. */
static float fade_envelope(chain_cfg_t *cfg, float dt)
{
    const float rate_hz = (float)cfg->fade.rate_mhz / 1000.0f;
    cfg->fade_phase += 2.0f * (float)M_PI * rate_hz * dt;
    if (cfg->fade_phase > 2.0f * (float)M_PI) {
        cfg->fade_phase -= 2.0f * (float)M_PI;
    }

    const float p = cfg->fade_phase;
    float w;

    switch (cfg->fade.mode) {
    case CARRIER_FADE_SLOW:
    case CARRIER_FADE_FLUTTER:
        /* Same shape; the two modes differ only in the rates the UI offers
         * and in what an operator means by the word. */
        w = 0.5f + 0.5f * sinf(p);
        break;

    case CARRIER_FADE_RANDOM:
        /* Three incommensurate components. The sum never repeats, which is
         * what makes it sound like propagation rather than an effect: real
         * fading is several paths interfering, not one oscillator. */
        w = (sinf(p) + sinf(p * 0.37f + 1.1f) + sinf(p * 1.61f + 2.3f)) / 3.0f;
        w = 0.5f + 0.5f * w;
        break;

    default:
        return 1.0f;
    }

    const float depth = (float)cfg->fade.depth_pct / 100.0f;
    return 1.0f - depth * (1.0f - w);
}

static void on_fade_tick(void *arg)
{
    (void)arg;
    const float dt = 1.0f / (float)CARRIER_FADE_UPDATE_HZ;

    for (int ch = 0; ch < RF_CHAINS; ch++) {
        chain_cfg_t *cfg = &s_cfg[ch];
        if (cfg->fade.mode == CARRIER_FADE_OFF) continue;

        const float want = cfg->level * fade_envelope(cfg, dt);

        /* Only rebuild when the change is visible in the duty register.
         * Below that the table would come out identical and the arcsines
         * would be wasted. */
        if (fabsf(want - cfg->level_now) < 0.002f) continue;

        cfg->level_now = want;
        rebuild_duty_table(ch, cfg->depth, want);
    }
}

/* Runs only while at least one chain is fading. */
static void fade_timer_update(void)
{
    bool any = false;
    for (int ch = 0; ch < RF_CHAINS; ch++) {
        if (s_cfg[ch].fade.mode != CARRIER_FADE_OFF) any = true;
    }
    if (!s_fade_timer) return;

    if (any) {
        if (!esp_timer_is_active(s_fade_timer)) {
            esp_timer_start_periodic(s_fade_timer,
                                     1000000 / CARRIER_FADE_UPDATE_HZ);
        }
    } else if (esp_timer_is_active(s_fade_timer)) {
        esp_timer_stop(s_fade_timer);
        /* Put every chain back to its commanded level, or a chain would be
         * left stuck wherever the fade happened to stop. */
        for (int ch = 0; ch < RF_CHAINS; ch++) {
            if (s_cfg[ch].level_now == s_cfg[ch].level) continue;
            s_cfg[ch].level_now = s_cfg[ch].level;
            rebuild_duty_table(ch, s_cfg[ch].depth, s_cfg[ch].level);
        }
    }
}

esp_err_t carrier_init(void)
{
    if (s_inited) return ESP_OK;

    for (int ch = 0; ch < RF_CHAINS; ch++) {
        ESP_RETURN_ON_ERROR(chain_init(ch), TAG, "chain %d", ch);
    }

    const gptimer_config_t gcfg = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,          /* 1 us ticks */
        .intr_priority = 3,                /* above Wi-Fi, below panic */
    };
    ESP_RETURN_ON_ERROR(gptimer_new_timer(&gcfg, &s_timer), TAG, "gptimer");

    const gptimer_alarm_config_t acfg = {
        .alarm_count = 1000000 / AM_SAMPLE_RATE_HZ,   /* 50 us at 20 kHz */
        .reload_count = 0,
        .flags.auto_reload_on_alarm = true,
    };
    ESP_RETURN_ON_ERROR(gptimer_set_alarm_action(s_timer, &acfg), TAG, "alarm");

    const gptimer_event_callbacks_t cbs = { .on_alarm = on_modulation_tick };
    ESP_RETURN_ON_ERROR(gptimer_register_event_callbacks(s_timer, &cbs, NULL),
                        TAG, "callbacks");
    ESP_RETURN_ON_ERROR(gptimer_enable(s_timer), TAG, "enable");
    ESP_RETURN_ON_ERROR(gptimer_start(s_timer), TAG, "start");

    const esp_timer_create_args_t fade = {
        .callback = on_fade_tick,
        .name = "carrierfade",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&fade, &s_fade_timer), TAG, "fade timer");

    s_inited = true;
    ESP_LOGI(TAG, "%d chain(s) running, %d Hz modulation", RF_CHAINS,
             AM_SAMPLE_RATE_HZ);
    fade_timer_update();
    return ESP_OK;
}

/* ------------------------------------------------------------------- API */

void carrier_set_enabled(int ch, bool on)
{
    if (!chain_ok(ch)) return;
    s_rt[ch].output_on = on;
    if (!s_inited) return;

    if (on) {
        /* ledc_stop() cleared sig_out_en. The public API puts it back, and
         * this runs in a task, so its locks and logging are fine here; only
         * the ISR needs the LL path. */
        ledc_set_duty(RF_LEDC_MODE, s_cfg[ch].channel, quiescent_duty(ch));
        ledc_update_duty(RF_LEDC_MODE, s_cfg[ch].channel);
    } else {
        /* Stop with the pin low: idling high would put a DC level on
         * whatever is coupled to the output network. */
        ledc_stop(RF_LEDC_MODE, s_cfg[ch].channel, 0);
    }
    ESP_LOGI(TAG, "chain %d carrier %s", ch, on ? "on" : "off");
}

bool carrier_is_enabled(int ch)
{
    return chain_ok(ch) ? s_rt[ch].output_on : false;
}

esp_err_t carrier_set_frequency(int ch, uint32_t hz)
{
    if (!chain_ok(ch)) return ESP_ERR_INVALID_ARG;
    if (!frequency_is_reachable(hz)) return ESP_ERR_INVALID_ARG;

    if (s_inited) {
        const esp_err_t err = ledc_set_freq(RF_LEDC_MODE, s_cfg[ch].timer, hz);
        if (err != ESP_OK) return err;
    }
    s_cfg[ch].carrier_hz = hz;
    ESP_LOGI(TAG, "chain %d retuned to %" PRIu32 " Hz", ch, hz);
    return ESP_OK;
}

uint32_t carrier_get_frequency(int ch)
{
    if (!chain_ok(ch)) return 0;
    return s_inited ? ledc_get_freq(RF_LEDC_MODE, s_cfg[ch].timer)
                    : s_cfg[ch].carrier_hz;
}

esp_err_t carrier_set_depth(int ch, float depth)
{
    if (!chain_ok(ch)) return ESP_ERR_INVALID_ARG;
    if (!(depth >= 0.05f && depth <= 0.90f)) return ESP_ERR_INVALID_ARG;

    /* The ISR reads one entry per tick and every entry is individually valid,
     * so rebuilding underneath it produces at worst one sample from the old
     * curve and the next from the new. No lock needed, and a lock here would
     * be a lock the ISR could contend on. */
    rebuild_duty_table(ch, depth, s_cfg[ch].level_now);
    s_cfg[ch].depth = depth;
    ESP_LOGI(TAG, "chain %d modulation depth %.2f", ch, (double)depth);
    return ESP_OK;
}

float carrier_get_depth(int ch)
{
    return chain_ok(ch) ? s_cfg[ch].depth : 0.0f;
}

esp_err_t carrier_set_level(int ch, float level)
{
    if (!chain_ok(ch)) return ESP_ERR_INVALID_ARG;
    if (!(level >= 0.05f && level <= 1.0f)) return ESP_ERR_INVALID_ARG;

    s_cfg[ch].level = level;

    /* If a fade is running it owns the live level and will pick the new
     * ceiling up on its next tick. Otherwise apply it now. */
    if (s_cfg[ch].fade.mode == CARRIER_FADE_OFF) {
        s_cfg[ch].level_now = level;
        rebuild_duty_table(ch, s_cfg[ch].depth, level);
    }
    ESP_LOGI(TAG, "chain %d carrier level %.2f", ch, (double)level);
    return ESP_OK;
}

float carrier_get_level(int ch)
{
    return chain_ok(ch) ? s_cfg[ch].level : 0.0f;
}

esp_err_t carrier_set_fade(int ch, const carrier_fade_cfg_t *cfg)
{
    if (!chain_ok(ch) || !cfg) return ESP_ERR_INVALID_ARG;
    if (cfg->mode < CARRIER_FADE_OFF || cfg->mode > CARRIER_FADE_RANDOM) {
        return ESP_ERR_INVALID_ARG;
    }

    carrier_fade_cfg_t next = *cfg;
    if (next.rate_mhz < 10) next.rate_mhz = 10;          /* 0.01 Hz */
    if (next.rate_mhz > 12000) next.rate_mhz = 12000;    /* 12 Hz */
    if (next.depth_pct < 0) next.depth_pct = 0;
    if (next.depth_pct > 100) next.depth_pct = 100;

    s_cfg[ch].fade = next;
    ESP_LOGI(TAG, "chain %d fade mode %d, %.2f Hz, depth %d%%", ch,
             (int)next.mode, (double)next.rate_mhz / 1000.0, next.depth_pct);

    fade_timer_update();
    return ESP_OK;
}

void carrier_get_fade(int ch, carrier_fade_cfg_t *out)
{
    if (!chain_ok(ch)) {
        memset(out, 0, sizeof *out);
        return;
    }
    *out = s_cfg[ch].fade;
}

uint32_t carrier_push(int ch, const int16_t *samples, uint32_t n)
{
    if (!chain_ok(ch)) return 0;
    return sample_fifo_write(&s_rt[ch].fifo, samples, n);
}

void carrier_flush(int ch)
{
    if (!chain_ok(ch)) return;
    /* Ask the ISR to do it. The producer must not write `tail`. */
    s_rt[ch].flush_req++;
}

void carrier_suspend(void)
{
    if (!s_inited || s_suspended) return;
    gptimer_stop(s_timer);
    s_suspended = true;
}

void carrier_resume(void)
{
    if (!s_inited || !s_suspended) return;

    /* Whatever is still in a FIFO went stale while the timer was stopped;
     * playing it out would be a burst of late audio against a carrier that
     * never paused. Ask the first tick after the restart to drop it. */
    for (int ch = 0; ch < RF_CHAINS; ch++) carrier_flush(ch);

    gptimer_start(s_timer);
    s_suspended = false;
}

void carrier_get_stats(int ch, carrier_stats_t *out)
{
    if (!chain_ok(ch)) {
        memset(out, 0, sizeof *out);
        return;
    }
    out->underruns = s_rt[ch].underruns;
    out->fifo_count = sample_fifo_count(&s_rt[ch].fifo);
    out->fifo_capacity = SAMPLE_FIFO_LEN;
    out->carrier_hz = carrier_get_frequency(ch);
    out->depth = s_cfg[ch].depth;
    out->level = s_cfg[ch].level;
    out->level_now = s_cfg[ch].level_now;
    out->enabled = s_rt[ch].output_on;
}
