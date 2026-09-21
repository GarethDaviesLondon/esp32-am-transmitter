/* settings_rf.c: the `rf` NVS namespace.
 *
 * One rf_settings_t per transmitter: carrier frequency, modulation depth,
 * audio low-pass, gain, level, AGC and the fade simulation. Its own file
 * because `rf` is one of the four namespaces in design 04 section 4, and
 * because it is the one whose keys are all per chain -- every read and write
 * below goes through chain_key(), which is the rule that lets a board upgraded
 * from a single-carrier build keep its settings.
 *
 * Nothing here logs. A transmitter setting that fails to save is reported to
 * the caller as an esp_err_t and surfaces in the reply the operator is already
 * looking at.
 */

#include "settings_internal.h"

static rf_settings_t s_rf[RF_CHAINS];

/* Each chain's compiled-in starting frequency. Only the defaults differ;
 * everything else about the two transmitters starts the same. */
static uint32_t default_carrier_hz(int ch)
{
#if RF_CHAINS > 1
    if (ch == 1) return RF_CARRIER_HZ_B;
#endif
    (void)ch;
    return RF_CARRIER_HZ;
}

/* ------------------------------------------------------------- persistence */

void settings_rf_load(void)
{
    nvs_handle_t h;
    const bool opened = nvs_open(NS_RF, NVS_READONLY, &h) == ESP_OK;

    for (int ch = 0; ch < RF_CHAINS; ch++) {
        s_rf[ch].rf_on = true;
        s_rf[ch].carrier_hz = default_carrier_hz(ch);
        s_rf[ch].depth_pct = (int)(AM_MODULATION_DEPTH * 100.0f + 0.5f);
        s_rf[ch].lpf_hz = AUDIO_LPF_CUTOFF_HZ;
        s_rf[ch].gain_pct = AUDIO_GAIN_PERCENT_DEFAULT;
        s_rf[ch].level_pct = 100;
        /* AGC on by default. Stations differ by more than 10 dB and a board
         * that sounds different on every station reads as broken. */
        s_rf[ch].agc_on = true;
        s_rf[ch].agc_target_pct = AUDIO_AGC_TARGET_PERCENT;
        s_rf[ch].fade_mode = 0;
        s_rf[ch].fade_rate_mhz = 200;
        s_rf[ch].fade_depth_pct = 60;

        /* Each key is optional: a saved value overrides its default and a
         * missing one leaves it, so a firmware that adds a key finds sensible
         * values on a board that has never written it. */
        if (!opened) continue;

        char key[16];
        int32_t v;
        uint8_t b;
        chain_key(key, sizeof key, "on", ch);
        if (nvs_get_u8(h, key, &b) == ESP_OK) s_rf[ch].rf_on = (b != 0);
        chain_key(key, sizeof key, "hz", ch);
        if (nvs_get_i32(h, key, &v) == ESP_OK) s_rf[ch].carrier_hz = (uint32_t)v;
        chain_key(key, sizeof key, "depth", ch);
        if (nvs_get_i32(h, key, &v) == ESP_OK) s_rf[ch].depth_pct = (int)v;
        chain_key(key, sizeof key, "lpf", ch);
        if (nvs_get_i32(h, key, &v) == ESP_OK) s_rf[ch].lpf_hz = (int)v;
        chain_key(key, sizeof key, "gain", ch);
        if (nvs_get_i32(h, key, &v) == ESP_OK) s_rf[ch].gain_pct = (int)v;
        chain_key(key, sizeof key, "lvl", ch);
        if (nvs_get_i32(h, key, &v) == ESP_OK) s_rf[ch].level_pct = (int)v;
        chain_key(key, sizeof key, "agc", ch);
        if (nvs_get_u8(h, key, &b) == ESP_OK) s_rf[ch].agc_on = (b != 0);
        chain_key(key, sizeof key, "agct", ch);
        if (nvs_get_i32(h, key, &v) == ESP_OK) s_rf[ch].agc_target_pct = (int)v;
        chain_key(key, sizeof key, "fdm", ch);
        if (nvs_get_i32(h, key, &v) == ESP_OK) s_rf[ch].fade_mode = (int)v;
        chain_key(key, sizeof key, "fdr", ch);
        if (nvs_get_i32(h, key, &v) == ESP_OK) s_rf[ch].fade_rate_mhz = (int)v;
        chain_key(key, sizeof key, "fdd", ch);
        if (nvs_get_i32(h, key, &v) == ESP_OK) s_rf[ch].fade_depth_pct = (int)v;
    }

    if (opened) nvs_close(h);
}

/* --------------------------------------------------------------------- API */

void settings_rf_get(int ch, rf_settings_t *out)
{
    *out = s_rf[chain_ok(ch) ? ch : 0];
}

esp_err_t settings_rf_set(int ch, const rf_settings_t *in)
{
    if (!chain_ok(ch)) return ESP_ERR_INVALID_ARG;
    s_rf[ch] = *in;

    carrier_suspend();                  /* KI-07, settings_internal.h */

    nvs_handle_t h;
    const bool opened = nvs_open(NS_RF, NVS_READWRITE, &h) == ESP_OK;
    if (opened) {
        char key[16];
        chain_key(key, sizeof key, "on", ch);
        nvs_set_u8(h, key, s_rf[ch].rf_on ? 1 : 0);
        chain_key(key, sizeof key, "hz", ch);
        nvs_set_i32(h, key, (int32_t)s_rf[ch].carrier_hz);
        chain_key(key, sizeof key, "depth", ch);
        nvs_set_i32(h, key, s_rf[ch].depth_pct);
        chain_key(key, sizeof key, "lpf", ch);
        nvs_set_i32(h, key, s_rf[ch].lpf_hz);
        chain_key(key, sizeof key, "gain", ch);
        nvs_set_i32(h, key, s_rf[ch].gain_pct);
        chain_key(key, sizeof key, "lvl", ch);
        nvs_set_i32(h, key, s_rf[ch].level_pct);
        chain_key(key, sizeof key, "agc", ch);
        nvs_set_u8(h, key, s_rf[ch].agc_on ? 1 : 0);
        chain_key(key, sizeof key, "agct", ch);
        nvs_set_i32(h, key, s_rf[ch].agc_target_pct);
        chain_key(key, sizeof key, "fdm", ch);
        nvs_set_i32(h, key, s_rf[ch].fade_mode);
        chain_key(key, sizeof key, "fdr", ch);
        nvs_set_i32(h, key, s_rf[ch].fade_rate_mhz);
        chain_key(key, sizeof key, "fdd", ch);
        nvs_set_i32(h, key, s_rf[ch].fade_depth_pct);
        nvs_commit(h);
        nvs_close(h);
    }

    carrier_resume();
    /* The RAM cache is updated either way, so the change is live even if it
     * did not persist. The caller is told which of the two it got. */
    return opened ? ESP_OK : ESP_FAIL;
}
