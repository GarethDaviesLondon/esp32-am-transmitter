/* app.h: the one place that knows how the pieces fit together.
 *
 * Everything with a control surface, the web UI today and a front panel
 * later (EH-02), goes through these functions rather than reaching into the
 * stream, decoder, carrier and settings modules directly. That is what keeps
 * adding a knob from becoming a second, subtly different code path.
 *
 * There are RF_CHAINS independent transmitters, indexed 0 and 1. Each has its
 * own station, carrier frequency, modulation depth, audio low-pass and gain.
 * They share one station list and one Wi-Fi connection, and nothing else.
 */
#ifndef AMTX_APP_H
#define AMTX_APP_H

#include <stdbool.h>

#include "am_config.h"
#include "esp_err.h"
#include "store/settings.h"

/* Starts every carrier, decoder and stream task, and tunes each chain to its
 * power-on station. Wi-Fi must already be up. */
esp_err_t app_init(void);

esp_err_t app_play_index(int ch, int idx);
esp_err_t app_play_url(int ch, const char *url);
void app_stop(int ch);

/* -1 when what is playing on this chain is not one of the saved stations. */
int app_current_index(int ch);
bool app_is_playing(int ch);

/* Sets a station's level trim, and applies it at once if that station is the
 * one currently playing on `ch`. */
esp_err_t app_set_station_gain(int ch, int idx, int gain_pct);

/* Applies and persists in one step: RF settings that are live but not saved
 * are a trap for anyone who power-cycles the board mid-experiment. */
esp_err_t app_set_rf(int ch, const rf_settings_t *rf);
void app_get_rf(int ch, rf_settings_t *out);

#endif /* AMTX_APP_H */
