/* settings.h: everything that must survive a power cycle.
 *
 * Four NVS namespaces -- `radio`, `wifi`, `rf` and `sys` -- all with keys of 15
 * characters or fewer as NVS requires. The schema table in
 * docs/design/04_web_and_storage.md section 4 is the contract: a new key is a
 * change to that table, in the same commit.
 *
 * This is the only header a caller outside store/ needs. Behind it there is one
 * .c file per namespace, sharing store/settings_internal.h; which file a
 * function lives in is not something a caller should have to know.
 *
 * Stations and Wi-Fi credentials are cached in RAM and written through, so
 * the web handlers never block on flash while holding anything.
 */
#ifndef AMTX_SETTINGS_H
#define AMTX_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "am_config.h"
#include "esp_err.h"

typedef struct {
    char name[STATION_NAME_MAX];
    char url[STATION_URL_MAX];
    /* Per-station level trim, percent, 100 being no change. The AGC handles
     * most of the difference between stations; this is for the cases where it
     * gets one subjectively wrong. */
    int gain_pct;
} station_t;

typedef struct {
    bool rf_on;
    uint32_t carrier_hz;
    int depth_pct;      /* modulation depth, 5..90 */
    int lpf_hz;         /* audio low-pass cutoff */
    int gain_pct;       /* programme gain before the AGC */
    int level_pct;      /* carrier level, 5..100 */
    bool agc_on;
    int agc_target_pct; /* wanted average level, 5..90 */
    int fade_mode;      /* carrier_fade_t */
    int fade_rate_mhz;  /* millihertz */
    int fade_depth_pct;
} rf_settings_t;

/* Mounts NVS and loads the caches. On a factory-fresh device, seeds one
 * station per transmitter from STATION_A_* and STATION_B_* and makes each that
 * transmitter's power-on station, so the thing plays out of the box. */
esp_err_t settings_init(void);

/* -------------------------------------------------------------- stations */

int  settings_station_count(void);
bool settings_station_get(int idx, station_t *out);
esp_err_t settings_station_add(const char *name, const char *url);
esp_err_t settings_station_set_gain(int idx, int gain_pct);
esp_err_t settings_station_delete(int idx);
esp_err_t settings_station_move(int from, int to);

/* Per chain. Both indices name a station in the one shared list: the two
 * transmitters pick from the same 20 stations. */
int  settings_last_station(int ch);
void settings_set_last_station(int ch, int idx);

/* The station played at power-on. Returns -1 when none has been chosen, in
 * which case the board falls back to whatever it was playing last.
 *
 * Stored as an index, so every operation that renumbers the list has to carry
 * it along: reordering must not silently change which station the board wakes
 * up on. settings_station_move() and settings_station_delete() do that. */
int  settings_default_station(int ch);
esp_err_t settings_set_default_station(int ch, int idx);  /* idx < 0 clears */

/* ------------------------------------------------------------------ Wi-Fi */

int  settings_wifi_count(void);
/* Copies into the caller's buffers; pass NULL for a field you do not want. */
bool settings_wifi_get(int idx, char *ssid, size_t ssid_len,
                       char *pass, size_t pass_len);
/* Replaces the entry if the SSID is already known, otherwise appends.
 * A newly added network goes to the front: the operator just chose it. */
esp_err_t settings_wifi_add(const char *ssid, const char *pass);
esp_err_t settings_wifi_delete(int idx);

/* --------------------------------------------------------------------- RF */

/* Per chain: the two transmitters have entirely separate carrier frequency,
 * modulation depth, audio low-pass and gain. */
void settings_rf_get(int ch, rf_settings_t *out);
esp_err_t settings_rf_set(int ch, const rf_settings_t *in);

/* ----------------------------------------------------------------- system */

/* The mDNS and DHCP hostname. Never empty: AMTX_HOSTNAME until one is set. */
const char *settings_hostname(void);
/* 1 to 31 letters, digits and hyphens, not starting or ending with a hyphen.
 * Stored lower case. ESP_ERR_INVALID_ARG for anything else, and nothing is
 * saved. */
esp_err_t settings_set_hostname(const char *name);

/* Whether the web interface, and with it the setup portal, runs. On until
 * turned off. Design 07 section 4. */
bool settings_web_enabled(void);
esp_err_t settings_set_web_enabled(bool on);

/* Erases every namespace and restarts the board, so it comes back factory
 * fresh: no stations but the compiled-in two, no saved networks, default RF
 * settings, default hostname, web on. Design 07 section 7.
 *
 * Does NOT return when it works. Returns the NVS error when it does not, and
 * then the board is still running with everything it had. */
esp_err_t settings_factory_reset(void);

#endif /* AMTX_SETTINGS_H */
