/* settings_stations.c: the `radio` NVS namespace.
 *
 * The station list, and the two per-transmitter indices into it: `last`, what
 * that transmitter was playing, and `def`, what it should play at power-on.
 * Its own file because `radio` is one of the four namespaces in design 04
 * section 4, and because it is the only one whose contents are a list that can
 * be reordered -- which is where all the interesting code is (see
 * remap_after_move below).
 *
 * The whole list is cached in RAM and written through, so a web handler never
 * blocks on flash while holding anything.
 */

#include "settings_internal.h"

#include <string.h>

#include "esp_log.h"

/* Same tag as the other settings_*.c files on purpose: this is one subsystem
 * to anyone reading the log or filtering it, and splitting the source should
 * not split the log. */
static const char *TAG = "settings";

static station_t s_stations[STATION_MAX];
static int s_station_count;
/* Per chain, and both are indices into the one shared list: the transmitters
 * pick from the same stations. Stored under the chain keys described in
 * settings_internal.h. */
static int s_last_station[RF_CHAINS];
static int s_default_station[RF_CHAINS];

/* ------------------------------------------------------------- persistence */

static void stations_save(void)
{
    carrier_suspend();                  /* KI-07, settings_internal.h */

    nvs_handle_t h;
    if (nvs_open(NS_RADIO, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "cnt", s_station_count);
        for (int ch = 0; ch < RF_CHAINS; ch++) {
            char key[16];
            chain_key(key, sizeof key, "last", ch);
            nvs_set_i32(h, key, s_last_station[ch]);
            chain_key(key, sizeof key, "def", ch);
            nvs_set_i32(h, key, s_default_station[ch]);
        }
        for (int i = 0; i < s_station_count; i++) {
            nvs_put_str_at(h, "na", i, s_stations[i].name);
            nvs_put_str_at(h, "ur", i, s_stations[i].url);
            char key[16];
            snprintf(key, sizeof key, "tr%d", i);
            nvs_set_i32(h, key, s_stations[i].gain_pct);
        }
        nvs_commit(h);
        nvs_close(h);
    }

    carrier_resume();
}

/* Appends a compiled-in station and makes it chain `ch`'s power-on and last
 * station. An empty URL seeds nothing, so a build can opt out per chain. */
static void seed_station(int ch, const char *name, const char *url)
{
    if (!url || url[0] == '\0' || s_station_count >= STATION_MAX) return;

    station_t *st = &s_stations[s_station_count];
    memset(st, 0, sizeof *st);
    snprintf(st->name, sizeof st->name, "%s", (name && name[0]) ? name : url);
    snprintf(st->url, sizeof st->url, "%s", url);
    st->gain_pct = 100;

    s_last_station[ch] = s_station_count;
    s_default_station[ch] = s_station_count;
    s_station_count++;
}

void settings_stations_load(void)
{
    nvs_handle_t h;
    s_station_count = 0;
    for (int ch = 0; ch < RF_CHAINS; ch++) {
        s_last_station[ch] = 0;
        s_default_station[ch] = -1;
    }

    if (nvs_open(NS_RADIO, NVS_READONLY, &h) == ESP_OK) {
        int32_t cnt = 0;
        nvs_get_i32(h, "cnt", &cnt);
        if (cnt < 0) cnt = 0;
        if (cnt > STATION_MAX) cnt = STATION_MAX;

        for (int i = 0; i < cnt; i++) {
            station_t *st = &s_stations[s_station_count];
            memset(st, 0, sizeof *st);
            st->gain_pct = 100;
            if (nvs_get_str_at(h, "na", i, st->name, sizeof st->name) &&
                nvs_get_str_at(h, "ur", i, st->url, sizeof st->url)) {
                char key[16];
                int32_t g = 100;
                snprintf(key, sizeof key, "tr%d", i);
                nvs_get_i32(h, key, &g);
                st->gain_pct = (g >= 10 && g <= 400) ? (int)g : 100;
                s_station_count++;
            }
        }
        for (int ch = 0; ch < RF_CHAINS; ch++) {
            char key[16];

            int32_t last = 0;
            chain_key(key, sizeof key, "last", ch);
            nvs_get_i32(h, key, &last);
            s_last_station[ch] =
                (last >= 0 && last < s_station_count) ? (int)last : 0;

            int32_t def = -1;
            chain_key(key, sizeof key, "def", ch);
            nvs_get_i32(h, key, &def);
            s_default_station[ch] =
                (def >= 0 && def < s_station_count) ? (int)def : -1;
        }
        nvs_close(h);
    }

    /* Factory-fresh: seed from the compiled-in stations so a first flash
     * plays something on each transmitter without anyone opening the web UI. */
    if (s_station_count == 0) {
        seed_station(0, STATION_A_NAME, STATION_A_URL);
#if RF_CHAINS > 1
        seed_station(1, STATION_B_NAME, STATION_B_URL);
#endif
        if (s_station_count > 0) {
            stations_save();
            ESP_LOGI(TAG, "seeded %d station(s) from the compiled defaults",
                     s_station_count);
        }
    }
}

/* --------------------------------------------------------------------- API */

int settings_station_count(void) { return s_station_count; }

bool settings_station_get(int idx, station_t *out)
{
    if (idx < 0 || idx >= s_station_count) return false;
    *out = s_stations[idx];
    return true;
}

esp_err_t settings_station_add(const char *name, const char *url)
{
    if (!name || !url || url[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (s_station_count >= STATION_MAX) return ESP_ERR_NO_MEM;

    station_t *st = &s_stations[s_station_count];
    memset(st, 0, sizeof *st);
    snprintf(st->name, sizeof st->name, "%s", name[0] ? name : url);
    snprintf(st->url, sizeof st->url, "%s", url);
    st->gain_pct = 100;
    s_station_count++;

    stations_save();
    return ESP_OK;
}

esp_err_t settings_station_set_gain(int idx, int gain_pct)
{
    if (idx < 0 || idx >= s_station_count) return ESP_ERR_INVALID_ARG;
    if (gain_pct < 10) gain_pct = 10;
    if (gain_pct > 400) gain_pct = 400;
    if (s_stations[idx].gain_pct == gain_pct) return ESP_OK;

    s_stations[idx].gain_pct = gain_pct;
    stations_save();
    return ESP_OK;
}

esp_err_t settings_station_delete(int idx)
{
    if (idx < 0 || idx >= s_station_count) return ESP_ERR_INVALID_ARG;

    for (int i = idx; i < s_station_count - 1; i++) {
        s_stations[i] = s_stations[i + 1];
    }
    s_station_count--;

    for (int ch = 0; ch < RF_CHAINS; ch++) {
        /* Keep `last` pointing at the same station where it still can. */
        if (s_last_station[ch] == idx) s_last_station[ch] = 0;
        else if (s_last_station[ch] > idx) s_last_station[ch]--;

        /* The default is a deliberate choice, so deleting the station it
         * names clears it rather than silently promoting a neighbour. */
        if (s_default_station[ch] == idx) s_default_station[ch] = -1;
        else if (s_default_station[ch] > idx) s_default_station[ch]--;
    }

    stations_save();
    return ESP_OK;
}

/* Where index `i` ends up once the item at `from` has been spliced out and
 * reinserted at `to`. Everything between the two shifts by one; everything
 * outside that span is untouched. A negative index (no default set) is passed
 * through unchanged. */
static int remap_after_move(int i, int from, int to)
{
    if (i < 0) return i;
    if (i == from) return to;
    if (from < to && i > from && i <= to) return i - 1;
    if (from > to && i >= to && i < from) return i + 1;
    return i;
}

esp_err_t settings_station_move(int from, int to)
{
    if (from < 0 || from >= s_station_count) return ESP_ERR_INVALID_ARG;
    if (to < 0 || to >= s_station_count) return ESP_ERR_INVALID_ARG;
    if (from == to) return ESP_OK;

    const station_t moved = s_stations[from];
    if (from < to) {
        for (int i = from; i < to; i++) s_stations[i] = s_stations[i + 1];
    } else {
        for (int i = from; i > to; i--) s_stations[i] = s_stations[i - 1];
    }
    s_stations[to] = moved;

    /* Both saved indices name a station, not a position, so they have to
     * follow it through the renumbering. Without this, dragging the list
     * around quietly changes which station the board boots on. */
    for (int ch = 0; ch < RF_CHAINS; ch++) {
        s_last_station[ch] = remap_after_move(s_last_station[ch], from, to);
        s_default_station[ch] =
            remap_after_move(s_default_station[ch], from, to);
    }

    stations_save();
    return ESP_OK;
}

int settings_last_station(int ch)
{
    return chain_ok(ch) ? s_last_station[ch] : 0;
}

int settings_default_station(int ch)
{
    return chain_ok(ch) ? s_default_station[ch] : -1;
}

esp_err_t settings_set_default_station(int ch, int idx)
{
    if (!chain_ok(ch)) return ESP_ERR_INVALID_ARG;
    if (idx >= s_station_count) return ESP_ERR_INVALID_ARG;
    if (idx < 0) idx = -1;                  /* any negative means "cleared" */
    if (idx == s_default_station[ch]) return ESP_OK;

    s_default_station[ch] = idx;

    carrier_suspend();                  /* KI-07, settings_internal.h */

    nvs_handle_t h;
    if (nvs_open(NS_RADIO, NVS_READWRITE, &h) == ESP_OK) {
        char key[16];
        chain_key(key, sizeof key, "def", ch);
        nvs_set_i32(h, key, s_default_station[ch]);
        nvs_commit(h);
        nvs_close(h);
    }

    carrier_resume();
    return ESP_OK;
}

void settings_set_last_station(int ch, int idx)
{
    if (!chain_ok(ch)) return;
    if (idx < 0 || idx >= s_station_count) return;
    if (idx == s_last_station[ch]) return;

    s_last_station[ch] = idx;

    /* Only the one key: rewriting the whole list on every station change
     * would wear the flash for nothing. */
    carrier_suspend();                  /* KI-07, settings_internal.h */

    nvs_handle_t h;
    if (nvs_open(NS_RADIO, NVS_READWRITE, &h) == ESP_OK) {
        char key[16];
        chain_key(key, sizeof key, "last", ch);
        nvs_set_i32(h, key, s_last_station[ch]);
        nvs_commit(h);
        nvs_close(h);
    }

    carrier_resume();
}
