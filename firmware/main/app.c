/* app.c: see app.h. */

#include "app.h"

#include <inttypes.h>
#include <string.h>

#include "am_config.h"
#include "audio/decoder.h"
#include "esp_check.h"
#include "esp_log.h"
#include "net/stream.h"
#include "rf/carrier.h"

static const char *TAG = "app";

static int s_current[RF_CHAINS];

/* Per-station trim, remembered per chain so it can be folded into the gain
 * every time either the station or the chain gain changes. */
static int s_trim_pct[RF_CHAINS];

static inline bool chain_ok(int ch) { return ch >= 0 && ch < RF_CHAINS; }

/* Push one chain's saved settings into the live hardware. Used at startup and
 * whenever the operator applies a change. */
static void apply_rf(int ch, const rf_settings_t *rf)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(carrier_set_frequency(ch, rf->carrier_hz));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        carrier_set_depth(ch, (float)rf->depth_pct / 100.0f));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        carrier_set_level(ch, (float)rf->level_pct / 100.0f));
    carrier_set_enabled(ch, rf->rf_on);

    const carrier_fade_cfg_t fade = {
        .mode = (carrier_fade_t)rf->fade_mode,
        .rate_mhz = (uint32_t)rf->fade_rate_mhz,
        .depth_pct = rf->fade_depth_pct,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(carrier_set_fade(ch, &fade));

    decoder_set_lpf(ch, rf->lpf_hz);
    decoder_set_agc(ch, rf->agc_on, rf->agc_target_pct);

    /* The station trim multiplies the chain gain rather than replacing it, so
     * "this station is quiet" and "this transmitter runs hot" stay separate
     * knobs that compose. */
    const int trim = s_trim_pct[ch] > 0 ? s_trim_pct[ch] : 100;
    decoder_set_gain(ch, rf->gain_pct * trim / 100);
}

/* Re-apply the current chain gain with whatever trim is now in force. */
static void refresh_gain(int ch)
{
    rf_settings_t rf;
    settings_rf_get(ch, &rf);
    const int trim = s_trim_pct[ch] > 0 ? s_trim_pct[ch] : 100;
    decoder_set_gain(ch, rf.gain_pct * trim / 100);
}

esp_err_t app_init(void)
{
    /* Carriers first: they must be initialised from a task on the audio core,
     * and app_init() is called from one. The decoders feed them, so they have
     * to exist before any decode task starts pushing. One call brings up every
     * chain and the single modulation timer they share. */
    ESP_RETURN_ON_ERROR(carrier_init(), TAG, "carrier");

    for (int ch = 0; ch < RF_CHAINS; ch++) {
        s_current[ch] = -1;
        s_trim_pct[ch] = 100;
        rf_settings_t rf;
        settings_rf_get(ch, &rf);
        apply_rf(ch, &rf);
    }

    ESP_RETURN_ON_ERROR(stream_init(), TAG, "stream");
    ESP_RETURN_ON_ERROR(decoder_init(), TAG, "decoder");

    /* Come up on each chain's power-on station if one has been chosen,
     * otherwise on whatever that chain was playing last. A transmitter that
     * comes back silent after a power cut is a transmitter someone has to go
     * and find a phone for; a transmitter that comes back on a station nobody
     * picked is worse, which is why an explicit default wins. */
    if (settings_station_count() > 0) {
        for (int ch = 0; ch < RF_CHAINS; ch++) {
            const int def = settings_default_station(ch);
            const int start = (def >= 0) ? def : settings_last_station(ch);
            app_play_index(ch, start);
        }
    }
    return ESP_OK;
}

esp_err_t app_play_index(int ch, int idx)
{
    if (!chain_ok(ch)) return ESP_ERR_INVALID_ARG;

    station_t st;
    if (!settings_station_get(idx, &st)) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "[%d] station %d: %s (trim %d%%)", ch, idx, st.name,
             st.gain_pct);
    s_trim_pct[ch] = st.gain_pct > 0 ? st.gain_pct : 100;
    refresh_gain(ch);
    carrier_flush(ch);

    const esp_err_t err = stream_play(ch, st.url);
    if (err != ESP_OK) return err;

    s_current[ch] = idx;
    settings_set_last_station(ch, idx);
    return ESP_OK;
}

esp_err_t app_play_url(int ch, const char *url)
{
    if (!chain_ok(ch)) return ESP_ERR_INVALID_ARG;
    if (!url || url[0] == '\0') return ESP_ERR_INVALID_ARG;

    /* A URL played directly has no station entry, so no trim. */
    s_trim_pct[ch] = 100;
    refresh_gain(ch);
    carrier_flush(ch);
    const esp_err_t err = stream_play(ch, url);
    if (err == ESP_OK) s_current[ch] = -1;
    return err;
}

void app_stop(int ch)
{
    if (!chain_ok(ch)) return;
    stream_stop(ch);
    carrier_flush(ch);
    s_current[ch] = -1;
    ESP_LOGI(TAG, "[%d] stopped", ch);
}

int app_current_index(int ch)
{
    return chain_ok(ch) ? s_current[ch] : -1;
}

bool app_is_playing(int ch)
{
    return chain_ok(ch) && stream_is_playing(ch);
}

esp_err_t app_set_station_gain(int ch, int idx, int gain_pct)
{
    const esp_err_t err = settings_station_set_gain(idx, gain_pct);
    if (err != ESP_OK) return err;

    if (chain_ok(ch) && s_current[ch] == idx) {
        station_t st;
        if (settings_station_get(idx, &st)) {
            s_trim_pct[ch] = st.gain_pct > 0 ? st.gain_pct : 100;
            refresh_gain(ch);
        }
    }
    return ESP_OK;
}

esp_err_t app_set_rf(int ch, const rf_settings_t *rf)
{
    if (!chain_ok(ch)) return ESP_ERR_INVALID_ARG;

    rf_settings_t next = *rf;

    /* Clamp before applying, so a bad web request cannot put the modulator
     * somewhere the hardware will not follow. */
    if (next.depth_pct < 5) next.depth_pct = 5;
    if (next.depth_pct > 90) next.depth_pct = 90;
    if (next.gain_pct < 0) next.gain_pct = 0;
    if (next.gain_pct > 800) next.gain_pct = 800;
    if (next.lpf_hz < 500) next.lpf_hz = 500;
    if (next.lpf_hz > AM_SAMPLE_RATE_HZ / 2 - 500) {
        next.lpf_hz = AM_SAMPLE_RATE_HZ / 2 - 500;
    }
    if (next.level_pct < 5) next.level_pct = 5;
    if (next.level_pct > 100) next.level_pct = 100;
    if (next.agc_target_pct < 5) next.agc_target_pct = 5;
    if (next.agc_target_pct > 90) next.agc_target_pct = 90;

    const esp_err_t ferr = carrier_set_frequency(ch, next.carrier_hz);
    if (ferr != ESP_OK) {
        /* Keep the old frequency rather than half-applying the change. */
        ESP_LOGW(TAG, "[%d] rejected %" PRIu32 " Hz", ch, next.carrier_hz);
        return ferr;
    }
    carrier_set_depth(ch, (float)next.depth_pct / 100.0f);
    carrier_set_level(ch, (float)next.level_pct / 100.0f);
    carrier_set_enabled(ch, next.rf_on);

    const carrier_fade_cfg_t fade = {
        .mode = (carrier_fade_t)next.fade_mode,
        .rate_mhz = (uint32_t)next.fade_rate_mhz,
        .depth_pct = next.fade_depth_pct,
    };
    carrier_set_fade(ch, &fade);

    decoder_set_lpf(ch, next.lpf_hz);
    decoder_set_agc(ch, next.agc_on, next.agc_target_pct);

    const esp_err_t err = settings_rf_set(ch, &next);
    refresh_gain(ch);
    return err;
}

void app_get_rf(int ch, rf_settings_t *out)
{
    settings_rf_get(ch, out);
    if (!chain_ok(ch)) return;

    /* Report what the hardware is actually doing, not what was asked for. */
    out->carrier_hz = carrier_get_frequency(ch);
    out->depth_pct = (int)(carrier_get_depth(ch) * 100.0f + 0.5f);
    out->rf_on = carrier_is_enabled(ch);
}
