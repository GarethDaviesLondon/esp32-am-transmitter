/* cmd_system.c: see cmd_system.h. */

#include "cmd_system.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "am_config.h"
#include "cJSON.h"
#include "cli_reply.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net/stream.h"
#include "net/webui.h"
#include "net/wifi_conn.h"
#include "rf/carrier.h"
#include "state_doc.h"
#include "store/settings.h"

/* No TAG here either. `log` reaches every tag through esp_log_level_set("*"),
 * so it needs none of its own, and the rest of this module prints command
 * output rather than log lines. */

/* The level `log on` restores. Kept in one place so the two sites that raise
 * and lower logging cannot drift apart. */
#define CLI_LOG_LEVEL_DEFAULT ESP_LOG_INFO

static bool s_log_on = true;

static const char *state_name(wifi_conn_state_t s)
{
    switch (s) {
    case WIFI_MODE_JOINING: return "joining";
    case WIFI_MODE_JOINED:  return "connected";
    case WIFI_MODE_PORTAL:  return "portal";
    default:                return "offline";
    }
}

/* ------------------------------------------------------------------ status */

/* One line, fixed field order, no log decoration: meant to be polled and
 * parsed. Anything a script needs to decide whether the board is healthy
 * belongs here, and nothing else does. */
int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    wifi_conn_status_t st;
    wifi_conn_get_status(&st);

    /* Shared state first, then one group per transmitter suffixed with its
     * chain index, so a script can find ch0/ch1 fields by name rather than by
     * counting columns. host and web come last so a script that did count
     * columns before they existed still finds what it expected. */
    printf("state=%s ssid=\"%s\" ip=%s rssi=%d drops=%" PRIu32
           " saved=%d heap=%" PRIu32 " heapint=%" PRIu32
           " heapintmax=%" PRIu32 " uptime=%" PRId64,
           state_name(st.state), st.ssid, st.ip, st.rssi, st.disconnects,
           settings_wifi_count(),
           (uint32_t)esp_get_free_heap_size(),
           (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           esp_timer_get_time() / 1000000);

    for (int ch = 0; ch < RF_CHAINS; ch++) {
        carrier_stats_t cs;
        carrier_get_stats(ch, &cs);
        printf(" carrier%d=%s hz%d=%" PRIu32 " depth%d=%d playing%d=%d"
               " underruns%d=%" PRIu32,
               ch, cs.enabled ? "on" : "off",
               ch, cs.carrier_hz,
               ch, (int)(cs.depth * 100.0f + 0.5f),
               ch, stream_is_playing(ch) ? 1 : 0,
               ch, cs.underruns);
    }
    printf(" host=%s web=%s\n", settings_hostname(),
           webui_running() ? "on" : "off");
    return 0;
}

/* ------------------------------------------------------------------- state */

/* The /api/state document, so a program has the same view whether it talks to
 * the page's API or to this console, and still has one with the web off. */
static int cmd_state(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    cJSON *doc = state_doc_build();
    char *text = doc ? cJSON_PrintUnformatted(doc) : NULL;
    cJSON_Delete(doc);
    if (!text) return reply_error("out of memory");

    /* One printf for the whole line: log output takes the same stdout lock, so
     * it can land before or after the document but never inside it. */
    printf("%s\n", text);
    free(text);
    return reply_ok(NULL);
}

/* --------------------------------------------------------------------- log */

static int cmd_log(int argc, char **argv)
{
    if (argc < 2) {
        printf("log is %s\n", s_log_on ? "on" : "off");
        return 0;
    }

    if (strcmp(argv[1], "off") == 0) {
        esp_log_level_set("*", ESP_LOG_NONE);
        s_log_on = false;
        printf("log off\n");
        return 0;
    }

    if (strcmp(argv[1], "on") == 0) {
        esp_log_level_set("*", CLI_LOG_LEVEL_DEFAULT);
        /* Restore the one tag webui.c deliberately quietens, so `log on` does
         * not resurrect the captive-portal probe warnings. */
        esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
        s_log_on = true;
        printf("log on\n");
        return 0;
    }

    printf("usage: log on|off\n");
    return 1;
}

/* ------------------------------------------------------------------ reboot */

static int cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("rebooting\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return 0;
}

/* ------------------------------------------------------------------- table */

/* `status`, `log` and `reboot` deliberately have no ok / error: reply line:
 * `status` is the one line a script polls, and `reboot` never returns. Design
 * 07 section 2 says so, and a program parsing this console relies on it. */
/* Design 07 section 7. A word rather than a y/n prompt, because a program
 * drives this console as well as a person (section 2) and it should not have
 * to answer a question it could not see coming. */
static int cmd_factory_reset(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "confirm") != 0) {
        printf("this erases EVERYTHING saved on the board:\n");
        printf("  every station, every saved Wi-Fi network,\n");
        printf("  both transmitters' settings, the hostname,\n");
        printf("  and whether the web interface runs.\n");
        printf("the firmware itself is untouched.\n");
        return reply_error("say 'factory-reset confirm' if that is what you want");
    }

    printf("erasing everything saved, then restarting\n");
    fflush(stdout);
    const esp_err_t err = settings_factory_reset();
    /* Only reached when the erase failed: it restarts the board otherwise. */
    return reply_error("%s", esp_err_to_name(err));
}

const esp_console_cmd_t cmd_system_table[] = {
    { .command = "status",   .help = "One parseable line of board state",
      .func = cmd_status },
    { .command = "state",    .help = "The whole state as one line of JSON, as /api/state",
      .func = cmd_state },
    { .command = "log",      .help = "log on|off: console message scrolling",
      .func = cmd_log },
    { .command = "factory-reset",
      .help = "factory-reset confirm: erase everything saved and restart",
      .func = cmd_factory_reset },
    { .command = "reboot",   .help = "Restart the board",
      .func = cmd_reboot },
};

const size_t cmd_system_count = sizeof cmd_system_table / sizeof cmd_system_table[0];
