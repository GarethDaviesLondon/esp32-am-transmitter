/* cmd_radio.c: see cmd_radio.h. */

#include "cmd_radio.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "am_config.h"
#include "app.h"
#include "audio/decoder.h"
#include "cli_reply.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net/discover.h"
#include "store/settings.h"

/* No TAG here: every line this module prints is a command's reply or the
 * output before it, and design 07 fixes both. Logging is a different channel,
 * which a program driving the console turns off. */

/* ----------------------------------------------------------- play and stop */

static int cmd_play(int argc, char **argv)
{
    if (argc != 3) return reply_error("usage: play <tx> <index|url>");
    const int ch = parse_tx(argv[1]);
    if (ch < 0) return reply_error("no transmitter '%s': use a or b", argv[1]);

    if (strstr(argv[2], "://")) {
        const esp_err_t err = app_play_url(ch, argv[2]);
        if (err != ESP_OK) return reply_error("%s", esp_err_to_name(err));
        return reply_ok("%s playing %s", tx_name(ch), argv[2]);
    }

    int idx;
    if (!parse_int(argv[2], 0, STATION_MAX - 1, &idx)) {
        return reply_error("'%s' is neither a station index nor a URL", argv[2]);
    }
    station_t st;
    if (!settings_station_get(idx, &st)) {
        return reply_error("no station %d: there are %d", idx, settings_station_count());
    }
    const esp_err_t err = app_play_index(ch, idx);
    if (err != ESP_OK) return reply_error("%s", esp_err_to_name(err));
    return reply_ok("%s playing %d, %s", tx_name(ch), idx, st.name);
}

static int cmd_stop(int argc, char **argv)
{
    if (argc != 2) return reply_error("usage: stop <tx>");
    const int ch = parse_tx(argv[1]);
    if (ch < 0) return reply_error("no transmitter '%s': use a or b", argv[1]);
    app_stop(ch);
    return reply_ok("%s stopped", tx_name(ch));
}

/* ---------------------------------------------------------------- stations */

static int station_usage(void)
{
    return reply_error("usage: station list | add <name> <url> | delete <index> | "
                       "move <from> <to> | default <tx> <index|none> | "
                       "trim <index> <10..400>");
}

/* Parses a station index argument that must name an existing station. */
static bool station_index(const char *s, int *out)
{
    return parse_int(s, 0, settings_station_count() - 1, out);
}

static int cmd_station(int argc, char **argv)
{
    if (argc < 2) return station_usage();
    const char *sub = argv[1];

    if (!strcmp(sub, "list") && argc == 2) {
        /* index, power-on flags per transmitter, trim, name, URL. For reading:
         * a program should parse `state`. */
        const int n = settings_station_count();
        for (int i = 0; i < n; i++) {
            station_t st;
            if (!settings_station_get(i, &st)) continue;
            char flags[RF_CHAINS + 1];
            for (int ch = 0; ch < RF_CHAINS; ch++) {
                flags[ch] = settings_default_station(ch) == i ? tx_name(ch)[0] : '-';
            }
            flags[RF_CHAINS] = '\0';
            printf("%2d  %s  %3d%%  %s  %s\n", i, flags, st.gain_pct, st.name, st.url);
        }
        return reply_ok("%d station(s)", n);
    }

    if (!strcmp(sub, "add") && argc == 4) {
        const esp_err_t err = settings_station_add(argv[2], argv[3]);
        if (err == ESP_ERR_NO_MEM) return reply_error("the list is full (%d)", STATION_MAX);
        if (err != ESP_OK) return reply_error("%s", esp_err_to_name(err));
        return reply_ok("added as station %d", settings_station_count() - 1);
    }

    if (!strcmp(sub, "delete") && argc == 3) {
        int idx;
        if (!station_index(argv[2], &idx)) return reply_error("no station '%s'", argv[2]);
        return reply_err(settings_station_delete(idx), "deleted");
    }

    if (!strcmp(sub, "move") && argc == 4) {
        int from, to;
        if (!station_index(argv[2], &from)) return reply_error("no station '%s'", argv[2]);
        if (!station_index(argv[3], &to)) return reply_error("no station '%s'", argv[3]);
        return reply_err(settings_station_move(from, to), "moved");
    }

    if (!strcmp(sub, "default") && argc == 4) {
        const int ch = parse_tx(argv[2]);
        if (ch < 0) return reply_error("no transmitter '%s': use a or b", argv[2]);
        int idx = -1;
        if (strcasecmp(argv[3], "none") != 0 && !station_index(argv[3], &idx)) {
            return reply_error("no station '%s'", argv[3]);
        }
        const esp_err_t err = settings_set_default_station(ch, idx);
        if (err != ESP_OK) return reply_error("%s", esp_err_to_name(err));
        if (idx < 0) return reply_ok("%s has no power-on station", tx_name(ch));
        return reply_ok("%s plays station %d at power-on", tx_name(ch), idx);
    }

    if (!strcmp(sub, "trim") && argc == 4) {
        int idx, pct;
        if (!station_index(argv[2], &idx)) return reply_error("no station '%s'", argv[2]);
        if (!parse_int(argv[3], 10, 400, &pct)) {
            return reply_error("trim is a percentage from 10 to 400");
        }
        /* Once per transmitter: the first call saves it, and each applies it
         * live if that transmitter is the one playing the station. */
        for (int ch = 0; ch < RF_CHAINS; ch++) {
            const esp_err_t err = app_set_station_gain(ch, idx, pct);
            if (err != ESP_OK) return reply_error("%s", esp_err_to_name(err));
        }
        return reply_ok("station %d trim %d%%", idx, pct);
    }

    return station_usage();
}

/* ---------------------------------------------------------------------- rf */

static const char *const FADE_NAMES[] = { "off", "slow", "flutter", "random" };
#define FADE_MODES ((int)(sizeof FADE_NAMES / sizeof FADE_NAMES[0]))

static void rf_print(int ch)
{
    rf_settings_t rf;
    app_get_rf(ch, &rf);
    printf("on=%d hz=%" PRIu32 " depth=%d lpf=%d gain=%d level=%d agc=%d agct=%d"
           " fade=%d faderate=%d fadedepth=%d\n",
           rf.rf_on ? 1 : 0, rf.carrier_hz, rf.depth_pct, rf.lpf_hz, rf.gain_pct,
           rf.level_pct, rf.agc_on ? 1 : 0, rf.agc_target_pct, rf.fade_mode,
           rf.fade_rate_mhz, rf.fade_depth_pct);
}

/* Applies one key to `rf`. Returns NULL, or what was wrong with it. The ranges
 * are wide on purpose where app_set_rf() clamps anyway; they exist to catch a
 * typo, not to restate the hardware limits. */
static const char *rf_apply_key(rf_settings_t *rf, const char *key, const char *val)
{
    int n;
    bool b;

    if (!strcmp(key, "on")) {
        if (!parse_onoff(val, &b)) return "on takes on or off";
        rf->rf_on = b;
    } else if (!strcmp(key, "hz")) {
        if (!parse_int(val, 1000, 1000000, &n)) return "hz is the carrier frequency in Hz";
        rf->carrier_hz = (uint32_t)n;
    } else if (!strcmp(key, "depth")) {
        if (!parse_int(val, 5, 90, &n)) return "depth is a percentage from 5 to 90";
        rf->depth_pct = n;
    } else if (!strcmp(key, "lpf")) {
        if (!parse_int(val, 500, AM_SAMPLE_RATE_HZ / 2, &n)) return "lpf is a cutoff in Hz";
        rf->lpf_hz = n;
    } else if (!strcmp(key, "gain")) {
        if (!parse_int(val, 0, 800, &n)) return "gain is a percentage from 0 to 800";
        rf->gain_pct = n;
    } else if (!strcmp(key, "level")) {
        if (!parse_int(val, 5, 100, &n)) return "level is a percentage from 5 to 100";
        rf->level_pct = n;
    } else if (!strcmp(key, "agc")) {
        if (!parse_onoff(val, &b)) return "agc takes on or off";
        rf->agc_on = b;
    } else if (!strcmp(key, "agct")) {
        if (!parse_int(val, 5, 90, &n)) return "agct is a percentage from 5 to 90";
        rf->agc_target_pct = n;
    } else if (!strcmp(key, "fade")) {
        int mode = -1;
        for (int i = 0; i < FADE_MODES; i++) {
            if (!strcasecmp(val, FADE_NAMES[i])) mode = i;
        }
        if (mode < 0 && !parse_int(val, 0, FADE_MODES - 1, &mode)) {
            return "fade is off, slow, flutter or random";
        }
        rf->fade_mode = mode;
    } else if (!strcmp(key, "faderate")) {
        if (!parse_int(val, 1, 100000, &n)) return "faderate is in millihertz";
        rf->fade_rate_mhz = n;
    } else if (!strcmp(key, "fadedepth")) {
        if (!parse_int(val, 0, 100, &n)) return "fadedepth is a percentage from 0 to 100";
        rf->fade_depth_pct = n;
    } else {
        return "unknown key: on hz depth lpf gain level agc agct fade faderate fadedepth";
    }
    return NULL;
}

static int cmd_rf(int argc, char **argv)
{
    if (argc < 2 || (argc > 2 && argc % 2 != 0)) {
        return reply_error("usage: rf <tx> [<key> <value> ...]");
    }
    const int ch = parse_tx(argv[1]);
    if (ch < 0) return reply_error("no transmitter '%s': use a or b", argv[1]);

    if (argc == 2) {
        rf_print(ch);
        return reply_ok(NULL);
    }

    /* Start from what is live, so only the named keys change, and apply
     * nothing unless every key parses: half a change is worse than none. */
    rf_settings_t rf;
    app_get_rf(ch, &rf);
    for (int i = 2; i < argc; i += 2) {
        const char *why = rf_apply_key(&rf, argv[i], argv[i + 1]);
        if (why) return reply_error("%s: %s", argv[i], why);
    }

    const esp_err_t err = app_set_rf(ch, &rf);
    if (err != ESP_OK) return reply_error("%s", esp_err_to_name(err));
    rf_print(ch);
    return reply_ok("%s applied and saved", tx_name(ch));
}

/* -------------------------------------------------------------------- tone */

static int cmd_tone(int argc, char **argv)
{
    if (argc < 3 || argc > 4) return reply_error("usage: tone <tx> on [hz] | tone <tx> off");
    const int ch = parse_tx(argv[1]);
    if (ch < 0) return reply_error("no transmitter '%s': use a or b", argv[1]);

    bool on;
    if (!parse_onoff(argv[2], &on)) return reply_error("tone takes on or off");
    if (!on) {
        if (argc != 3) return reply_error("usage: tone <tx> off");
        decoder_set_tone(ch, false, 1000);
        return reply_ok("%s tone off", tx_name(ch));
    }

    int hz = 1000;
    if (argc == 4 && !parse_int(argv[3], 50, 5000, &hz)) {
        return reply_error("the tone is 50 to 5000 Hz");
    }
    decoder_set_tone(ch, true, hz);
    return reply_ok("%s tone on at %d Hz", tx_name(ch), hz);
}

/* ---------------------------------------------------------------- discover */

/* A search runs the TLS handshake on the calling task's stack. The REPL task
 * has 4 kB, which is not enough; the web server's 6 kB is. So the search runs
 * on its own task, created for the search and gone after it, which holds the
 * extra internal RAM only while a search is running (design 07 section 2,
 * LL-07). The results stay here for `discover play` and `discover save`. */
#define DISCOVER_TASK_STACK 6144
#define DISCOVER_WAIT_MS    30000

static struct {
    discover_entry_t *found;    /* DISCOVER_RESULTS_MAX entries, kept for reuse */
    int count;                  /* of the last search to finish; -1 failed */
    discover_by_t by;
    char query[160];
    char error[80];
    TaskHandle_t waiter;
    volatile bool busy;
} s_disc;

static void discover_task(void *arg)
{
    (void)arg;
    const int n = discover_search(s_disc.by, s_disc.query, s_disc.found,
                                  DISCOVER_RESULTS_MAX);
    snprintf(s_disc.error, sizeof s_disc.error, "%s",
             n < 0 ? discover_last_error() : "");
    s_disc.count = n;
    s_disc.busy = false;
    xTaskNotifyGive(s_disc.waiter);
    vTaskDelete(NULL);
}

/* Parses a result number from the last search. On failure it has already
 * printed the error reply, and returns false. */
static bool discover_result(const char *s, int *out)
{
    if (s_disc.busy) {
        reply_error("a search is still running");
        return false;
    }
    if (s_disc.count <= 0) {
        reply_error("no results: search first");
        return false;
    }
    if (!parse_int(s, 0, s_disc.count - 1, out)) {
        reply_error("no result '%s': there are %d", s, s_disc.count);
        return false;
    }
    return true;
}

static int cmd_discover(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "play")) {
        const int ch = parse_tx(argv[2]);
        if (ch < 0) return reply_error("no transmitter '%s': use a or b", argv[2]);
        int i;
        if (!discover_result(argv[3], &i)) return 0;
        const discover_entry_t *e = &s_disc.found[i];
        const esp_err_t err = app_play_url(ch, e->url);
        if (err != ESP_OK) return reply_error("%s", esp_err_to_name(err));
        return reply_ok("%s playing %s", tx_name(ch), e->name);
    }

    if (argc == 3 && !strcmp(argv[1], "save")) {
        int i;
        if (!discover_result(argv[2], &i)) return 0;
        const discover_entry_t *e = &s_disc.found[i];
        const esp_err_t err = settings_station_add(e->name, e->url);
        if (err == ESP_ERR_NO_MEM) return reply_error("the list is full (%d)", STATION_MAX);
        if (err != ESP_OK) return reply_error("%s", esp_err_to_name(err));
        return reply_ok("saved %s as station %d", e->name, settings_station_count() - 1);
    }

    if (argc < 3 || (strcmp(argv[1], "name") && strcmp(argv[1], "tag") &&
                     strcmp(argv[1], "country"))) {
        return reply_error("usage: discover <name|tag|country> <query> | "
                           "discover play <tx> <n> | discover save <n>");
    }
    if (s_disc.busy) return reply_error("a search is still running");

    if (!s_disc.found) {
        /* PSRAM: nothing here is touched with the flash cache off. Station
         * names and URLs are copied out before any NVS write. */
        s_disc.found = heap_caps_calloc(DISCOVER_RESULTS_MAX, sizeof *s_disc.found,
                                        MALLOC_CAP_SPIRAM);
        if (!s_disc.found) {
            s_disc.found = calloc(DISCOVER_RESULTS_MAX, sizeof *s_disc.found);
        }
        if (!s_disc.found) return reply_error("out of memory");
    }

    /* The query is everything after the kind, so `discover name bbc world`
     * works without quotes. */
    s_disc.query[0] = '\0';
    for (int i = 2; i < argc; i++) {
        size_t used = strlen(s_disc.query);
        snprintf(s_disc.query + used, sizeof s_disc.query - used, "%s%s",
                 i > 2 ? " " : "", argv[i]);
    }
    s_disc.by = discover_by_from_string(argv[1]);
    s_disc.count = 0;
    s_disc.waiter = xTaskGetCurrentTaskHandle();
    s_disc.busy = true;

    printf("searching Radio-Browser by %s for \"%s\" ...\n", argv[1], s_disc.query);
    if (xTaskCreatePinnedToCore(discover_task, "clidisc", DISCOVER_TASK_STACK, NULL,
                                5, NULL, TASK_CORE_NET) != pdPASS) {
        s_disc.busy = false;
        return reply_error("could not start the search: out of memory");
    }

    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(DISCOVER_WAIT_MS)) == 0) {
        /* The task still owns the buffer and finishes by itself; `busy`
         * refuses another search, and play or save, until it has. */
        return reply_error("no answer from the directory in %d s", DISCOVER_WAIT_MS / 1000);
    }
    if (s_disc.count < 0) return reply_error("%s", s_disc.error);

    for (int i = 0; i < s_disc.count; i++) {
        const discover_entry_t *e = &s_disc.found[i];
        printf("%d  %s | %s | %d kbit/s | %s\n", i, e->name,
               e->country[0] ? e->country : "unknown", e->bitrate, e->url);
    }
    return reply_ok("%d found", s_disc.count);
}

/* ------------------------------------------------------------------- table */

/* Design 07 section 2 is the reference for every command here, and for the
 * ok / error: line each ends with. The help strings are what `help` prints, so
 * they are as much a contract as the replies are. */
const esp_console_cmd_t cmd_radio_table[] = {
    { .command = "play",     .help = "play <tx> <index|url>: tx is a or b",
      .func = cmd_play },
    { .command = "stop",     .help = "stop <tx>: stop the stream, carrier stays up",
      .func = cmd_stop },
    { .command = "station",  .help = "station list|add <name> <url>|delete <i>|"
                                     "move <from> <to>|default <tx> <i|none>|"
                                     "trim <i> <pct>",
      .func = cmd_station },
    { .command = "rf",       .help = "rf <tx> [<key> <value> ...]: keys on hz depth "
                                     "lpf gain level agc agct fade faderate fadedepth",
      .func = cmd_rf },
    { .command = "tone",     .help = "tone <tx> on [hz] | tone <tx> off",
      .func = cmd_tone },
    { .command = "discover", .help = "discover <name|tag|country> <query> | "
                                     "discover play <tx> <n> | discover save <n>",
      .func = cmd_discover },
};

const size_t cmd_radio_count = sizeof cmd_radio_table / sizeof cmd_radio_table[0];
